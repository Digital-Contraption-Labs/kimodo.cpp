#!/usr/bin/env bash
# Build the Kimodo library for iOS and package it for an app:
# kimodo.xcframework (a dynamic kimodo.framework for iPhone and iPad and one
# for the simulator on Apple Silicon: the engine and ggml with its Metal
# backend, exporting the C API alone; iOS 16.4 and later), kimodo_capi.h, the
# licences, VERSION.json and the motion models.  The 8 GB text encoder is
# left out: no phone holds it beside an app, so the library takes prompt
# embeddings made elsewhere.  The app embeds and signs the framework; this
# does not sign it.  See docs/SHARED_LIBRARY_PLAN.md.
#
#   scripts/build/build_library_ios.sh                  release package in dist/kimodo-ios
#   scripts/build/build_library_ios.sh --debug          debug build
#   scripts/build/build_library_ios.sh --out <folder>   package somewhere else
#   scripts/build/build_library_ios.sh --no-weights     leave the package's weights as they are
#
# Runs on a Mac with Xcode itself installed (not only its command line
# tools: the iOS SDKs and xcodebuild come with Xcode), CMake 3.25+ and Ninja
# (brew install cmake ninja), and Git.  The framework's bundle identifier is
# the CMake cache variable KIMODO_FRAMEWORK_IDENTIFIER.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build/build_library_ios.sh [--release | --debug] [--out <folder>] [--no-weights]

  --release     optimised build (the default)
  --debug       debug build
  --out         the package folder (default dist/kimodo-ios)
  --no-weights  leave the package's weights as they are
EOF
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
config=release
out=""
weights=ON

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --release) config=release ;;
        --debug) config=debug ;;
        --no-weights) weights=OFF ;;
        --out)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 needs a folder." >&2; exit 2; }
            out="$2"
            shift ;;
        *) echo "Unknown argument: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---- Prerequisites, each named with what installs it
if [[ "$(uname -s)" != Darwin ]]; then
    echo "Run this on a Mac with Xcode." >&2
    exit 1
fi
missing=()
if ! xcodebuild -version >/dev/null 2>&1; then
    missing+=("Xcode (from the App Store; then: sudo xcode-select -s /Applications/Xcode.app)")
else
    for sdk in iphoneos iphonesimulator; do
        xcrun --sdk "$sdk" --show-sdk-path >/dev/null 2>&1 || missing+=("the $sdk SDK (Xcode > Settings > Platforms > iOS)")
    done
fi
command -v cmake >/dev/null 2>&1 || missing+=("cmake (brew install cmake)")
command -v ninja >/dev/null 2>&1 || missing+=("ninja (brew install ninja)")
command -v git >/dev/null 2>&1 || missing+=("git (xcode-select --install)")
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing prerequisites:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi
cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
IFS=. read -r cmake_major cmake_minor _ <<<"$cmake_version"
if (( cmake_major < 3 || (cmake_major == 3 && cmake_minor < 25) )); then
    echo "CMake $cmake_version is too old: 3.25 or later is needed (brew upgrade cmake)." >&2
    exit 1
fi

# ---- Submodules: check out a missing one at the commit this repository pins
for module in ggml eigen; do
    case "$module" in
        ggml) probe=ggml/CMakeLists.txt ;;
        eigen) probe=eigen/Eigen/Sparse ;;
    esac
    if [[ ! -e "$root/$probe" ]]; then
        echo "Checking out the $module submodule..."
        git -C "$root" submodule update --init --recursive -- "$module" || {
            echo "git submodule update failed; run 'git submodule update --init --recursive' in $root to see why." >&2
            exit 1
        }
    fi
done

# ---- Build both slices, join them, package
[[ -n "$out" ]] || out="$root/dist/kimodo-ios"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cd "$root"
for slice in device simulator; do
    echo "Configuring the ios-$slice-$config preset..."
    cmake --preset "ios-$slice-$config"
    echo
    echo "Building kimodo.framework for the $slice ($config)..."
    cmake --build --preset "ios-$slice-library-$config"
    echo
done

xcframework="$root/build/ios-$config/kimodo.xcframework"
rm -rf "$xcframework"
mkdir -p "$(dirname "$xcframework")"
echo "Joining the frameworks into $xcframework..."
xcodebuild -create-xcframework \
    -framework "$root/build/ios-device-$config/kimodo.framework" \
    -framework "$root/build/ios-simulator-$config/kimodo.framework" \
    -output "$xcframework"
echo
echo "Checking and packaging into $out..."
cmake -DKIMODO_TARGET=ios "-DKIMODO_SOURCE_DIR=$root" "-DKIMODO_BUILD_DIR=$root/build/ios-device-$config" \
      "-DKIMODO_PACKAGE_DIR=$out" "-DKIMODO_XCFRAMEWORK=$xcframework" \
      "-DKIMODO_WEIGHTS=$weights" -DKIMODO_TEXT_WEIGHTS=OFF -P "$here/package.cmake"

cat <<EOF

Packaged the Kimodo library for iOS ($config) in $out

An app embeds lib/kimodo.xcframework (Xcode: General > Frameworks, Libraries
and Embedded Content, "Embed & Sign") and ships the motion model it opens.
It cannot be run from here: test it from the app.
EOF
