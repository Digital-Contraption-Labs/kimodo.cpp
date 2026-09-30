#!/usr/bin/env bash
# Build the Kimodo shared library for Android (arm64-v8a) on Linux or macOS
# and package it for an app: libkimodo.so (the engine and ggml in one file,
# exporting the C API alone; API 29, the static C++ runtime, aligned for
# 16 KB pages), its unstripped copy for crash reports, kimodo_capi.h, the
# licences, VERSION.json and the motion models.  The 8 GB text encoder is
# left out: no phone holds it beside an app, so the library takes prompt
# embeddings made elsewhere.  On Windows, scripts\build\build_library_android.bat
# does the same.  See docs/SHARED_LIBRARY_PLAN.md.
#
#   scripts/build/build_library_android.sh                  release package in dist/kimodo-android
#   scripts/build/build_library_android.sh --debug          debug build
#   scripts/build/build_library_android.sh --out <folder>   package somewhere else
#   scripts/build/build_library_android.sh --no-weights     leave the package's weights as they are
#   scripts/build/build_library_android.sh --ndk <folder>   an NDK other than the one found
#
# Needs the Android NDK 27.2.12479018 (ContraptionFabricator's), found in
# ANDROID_NDK_HOME, ANDROID_NDK_ROOT or the SDK's ndk/27.2.12479018
# (~/Android/Sdk on Linux, ~/Library/Android/sdk on macOS); CMake 3.25+,
# Ninja, a C++ compiler for this machine (ggml builds its shader compiler
# here), glslc (the Vulkan SDK's, or the NDK's own shader-tools one) and Git.
# The library cannot run here; the package's tools/ folder has a program that
# checks it on a device through adb (printed at the end).
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build/build_library_android.sh [--release | --debug] [--out <folder>] [--no-weights] [--ndk <folder>]

  --release     optimised build (the default)
  --debug       debug build
  --out         the package folder (default dist/kimodo-android)
  --no-weights  leave the package's weights as they are
  --ndk         the Android NDK (default: ANDROID_NDK_HOME, ANDROID_NDK_ROOT,
                or the SDK's ndk/27.2.12479018)
EOF
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
config=release
out=""
weights=ON
ndk=""
ndk_version=27.2.12479018

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --release) config=release ;;
        --debug) config=debug ;;
        --no-weights) weights=OFF ;;
        --out|--ndk)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 needs a folder." >&2; exit 2; }
            if [[ "$1" == --out ]]; then out="$2"; else ndk="$2"; fi
            shift ;;
        *) echo "Unknown argument: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---- The NDK: --ndk, then the environment, then the SDK's side-by-side folder
case "$(uname -s)" in
    Darwin) host=darwin-x86_64; sdk_default="$HOME/Library/Android/sdk" ;;
    Linux) host=linux-x86_64; sdk_default="$HOME/Android/Sdk" ;;
    *) echo "Run this on Linux or macOS; on Windows use build_library_android.bat." >&2; exit 1 ;;
esac
if [[ -z "$ndk" ]]; then
    for candidate in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
                     "${ANDROID_HOME:-$sdk_default}/ndk/$ndk_version" "$sdk_default/ndk/$ndk_version"; do
        if [[ -n "$candidate" && -f "$candidate/build/cmake/android.toolchain.cmake" ]]; then ndk="$candidate"; break; fi
    done
fi
if [[ -z "$ndk" || ! -f "$ndk/build/cmake/android.toolchain.cmake" ]]; then
    echo "The Android NDK $ndk_version was not found.  Install it with Android Studio's SDK" >&2
    echo "Manager (SDK Tools, \"NDK (Side by side)\"), or: sdkmanager \"ndk;$ndk_version\"" >&2
    echo "and set ANDROID_NDK_HOME, or pass --ndk <folder>." >&2
    exit 1
fi
ndk="$(cd "$ndk" && pwd)"
found="$(sed -n 's/^Pkg.Revision *= *//p' "$ndk/source.properties" 2>/dev/null || true)"
if [[ "$found" != "$ndk_version" ]]; then
    echo "Warning: NDK ${found:-of unknown version} in $ndk; ContraptionFabricator builds with $ndk_version." >&2
fi
ndk_bin="$ndk/toolchains/llvm/prebuilt/$host/bin"
export ANDROID_NDK_HOME="$ndk" # the presets read it from here

# ---- Prerequisites, each named with what installs it
missing=()
need() { command -v "$1" >/dev/null 2>&1 || missing+=("$1 ($2)"); }
need cmake "cmake.org, or your package manager"
need ninja "ninja-build, or brew install ninja"
need git "git"
if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    missing+=("a C++ compiler for this machine (g++, or Xcode's command line tools)")
fi
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing prerequisites:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi
# glslc: the Vulkan SDK's when there is one, else the NDK's.
glslc_args=()
if ! command -v glslc >/dev/null 2>&1; then
    if [[ -x "$ndk/shader-tools/$host/glslc" ]]; then
        glslc_args=("-DVulkan_GLSLC_EXECUTABLE=$ndk/shader-tools/$host/glslc")
    else
        echo "glslc was not found: install the Vulkan SDK (vulkan.lunarg.com)." >&2
        exit 1
    fi
fi

# ---- Submodules: check out a missing one at the commit this repository pins
for module in ggml eigen vulkan-headers spirv-headers; do
    case "$module" in
        ggml) probe=ggml/CMakeLists.txt ;;
        eigen) probe=eigen/Eigen/Sparse ;;
        vulkan-headers) probe=vulkan-headers/include/vulkan/vulkan.hpp ;;
        spirv-headers) probe=spirv-headers/include/spirv/unified1/spirv.hpp ;;
    esac
    if [[ ! -e "$root/$probe" ]]; then
        echo "Checking out the $module submodule..."
        git -C "$root" submodule update --init --recursive -- "$module" || {
            echo "git submodule update failed; run 'git submodule update --init --recursive' in $root to see why." >&2
            exit 1
        }
    fi
done

# ---- Build and package
[[ -n "$out" ]] || out="$root/dist/kimodo-android"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
build="$root/build/android-arm64-v8a-$config"

cd "$root"
echo "Configuring the android-arm64-$config preset with the NDK in $ndk..."
cmake --preset "android-arm64-$config" ${glslc_args[@]+"${glslc_args[@]}"}
echo
echo "Building libkimodo.so for arm64-v8a ($config)..."
cmake --build --preset "android-arm64-library-$config"
echo
echo "Checking and packaging into $out..."
cmake -DKIMODO_TARGET=android -DKIMODO_ANDROID_ABI=arm64-v8a "-DKIMODO_SOURCE_DIR=$root" \
      "-DKIMODO_BUILD_DIR=$build" "-DKIMODO_PACKAGE_DIR=$out" \
      "-DKIMODO_WEIGHTS=$weights" -DKIMODO_TEXT_WEIGHTS=OFF \
      "-DKIMODO_NM=$ndk_bin/llvm-nm" "-DKIMODO_READELF=$ndk_bin/llvm-readelf" "-DKIMODO_STRIP=$ndk_bin/llvm-strip" \
      -P "$here/package.cmake"

cat <<EOF

Packaged the Kimodo library for Android ($config) in $out

To check it on a phone or headset (USB debugging on, adb from the SDK's
platform-tools): make a prompt's embedding where the text encoder runs -- a
desktop package's tools/kimodo-capi-smoke with --data <its weights>
--save-embedding walk.f32 -- then generate from it on the device:
    adb push "$out/lib/arm64-v8a/libkimodo.so" "$out/tools/arm64-v8a/kimodo-capi-smoke" walk.f32 \\
        "$out/weights/kimodo-soma-seed-v1.1-f32.gguf" /data/local/tmp/
    adb shell "cd /data/local/tmp && chmod +x kimodo-capi-smoke && ./kimodo-capi-smoke --library ./libkimodo.so --data . --no-text --embedding-file walk.f32"
EOF
