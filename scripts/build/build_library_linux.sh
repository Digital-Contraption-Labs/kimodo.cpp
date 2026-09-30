#!/usr/bin/env bash
# Build the Kimodo shared library for Linux (x86_64) and package everything a
# host application needs to run it in its own process: libkimodo.so (the
# engine and ggml in one file, exporting the C API alone), kimodo_capi.h, the
# licences, VERSION.json and the weights from this repository's cache.  See
# docs/SHARED_LIBRARY_PLAN.md.
#
#   scripts/build/build_library_linux.sh                   release package in dist/kimodo-linux
#   scripts/build/build_library_linux.sh --debug           debug build
#   scripts/build/build_library_linux.sh --out <folder>    package somewhere else
#   scripts/build/build_library_linux.sh --no-weights      leave the package's weights as they are
#   scripts/build/build_library_linux.sh --build-dir <d>   build folder (see below)
#
# Build on the oldest distribution the library must run on: it needs that
# glibc or later, and VERSION.json says which.  The plan builds in Debian 12
# (glibc 2.36); from Windows, scripts\build\build_library_linux.bat runs this
# script in WSL.
#
# Needs CMake 3.25+, Ninja, GCC 12+, glslc, the Vulkan loader's development
# files, Git and binutils.  On Debian or Ubuntu:
#     sudo apt-get install cmake ninja-build g++ glslc libvulkan-dev git binutils
# The Vulkan and SPIR-V headers come from the vulkan-headers and
# spirv-headers submodules.  The library is
# checked on the way: it must export the C API and nothing else, need nothing
# but glibc and the Vulkan loader, and load and answer from the package.
#
# When the repository is on a Windows drive (/mnt/...), the build folder goes
# to ~/.cache/kimodo.cpp: ggml writes thousands of small files, which crawl
# across that boundary.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build/build_library_linux.sh [--release | --debug] [--out <folder>] [--no-weights] [--build-dir <folder>]

  --release      optimised build (the default)
  --debug        debug build
  --out          the package folder (default dist/kimodo-linux)
  --no-weights   leave the package's weights as they are, for a rebuild
                 that only changes the library
  --build-dir    the build folder (default build/linux-<config>, or
                 ~/.cache/kimodo.cpp/linux-<config> for a repository on /mnt)
EOF
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
config=release
out=""
weights=ON
build=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --release) config=release ;;
        --debug) config=debug ;;
        --no-weights) weights=OFF ;;
        --out|--build-dir)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 needs a folder." >&2; exit 2; }
            if [[ "$1" == --out ]]; then out="$2"; else build="$2"; fi
            shift ;;
        *) echo "Unknown argument: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---- Prerequisites, each named with what installs it
missing=()
need() { command -v "$1" >/dev/null 2>&1 || missing+=("$1 (package $2)"); }
need cmake cmake
need ninja ninja-build
need g++ g++
need glslc glslc
need git git
need readelf binutils
need nm binutils
# (Not `| grep -q`: with pipefail, the writer's SIGPIPE would read as absent.)
if [[ -z "$(ldconfig -p 2>/dev/null | grep 'libvulkan\.so ' || true)" ]]; then
    missing+=("the Vulkan loader's development files (package libvulkan-dev)")
fi
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing prerequisites:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    echo "On Debian or Ubuntu, install them with:" >&2
    echo "    sudo apt-get install cmake ninja-build g++ glslc libvulkan-dev git binutils" >&2
    exit 1
fi
cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
IFS=. read -r cmake_major cmake_minor _ <<<"$cmake_version"
if (( cmake_major < 3 || (cmake_major == 3 && cmake_minor < 25) )); then
    echo "CMake $cmake_version is too old: 3.25 or later is needed." >&2
    exit 1
fi
gcc_major="$(g++ -dumpversion | cut -d. -f1)"
if [[ "$gcc_major" -lt 12 ]]; then
    echo "GCC $gcc_major is too old: GCC 12 or later is needed (C++23)." >&2
    exit 1
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

# ---- Build
if [[ -z "$build" ]]; then
    if [[ "$root" == /mnt/* ]]; then
        build="$HOME/.cache/kimodo.cpp/linux-$config"
    else
        build="$root/build/linux-$config"
    fi
fi
[[ -n "$out" ]] || out="$root/dist/kimodo-linux"
mkdir -p "$build" "$out"
build="$(cd "$build" && pwd)"
out="$(cd "$out" && pwd)"

cd "$root"
echo "Configuring the linux-$config preset in $build..."
cmake --preset "linux-$config" -B "$build"
echo
echo "Building libkimodo.so ($config)..."
cmake --build "$build" --target kimodo kimodo-capi-smoke
echo
echo "Checking and packaging into $out..."
cmake -DKIMODO_TARGET=linux "-DKIMODO_SOURCE_DIR=$root" "-DKIMODO_BUILD_DIR=$build" "-DKIMODO_PACKAGE_DIR=$out" \
      "-DKIMODO_WEIGHTS=$weights" -P "$here/package.cmake"
echo
echo "Loading the packaged library..."
"$build/kimodo-capi-smoke" --library "$out/lib/libkimodo.so"

cat <<EOF

Packaged the Kimodo library ($config) in $out

To generate a clip through it (the CPU in WSL, which has no real GPU; a first
run takes minutes):
    $build/kimodo-capi-smoke --library "$out/lib/libkimodo.so" --cpu \\
        --data "$out/weights"
EOF
