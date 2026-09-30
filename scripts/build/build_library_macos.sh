#!/usr/bin/env bash
# Build the Kimodo shared library for macOS (Apple Silicon) and package
# everything a host application needs to run it in its own process:
# libkimodo.dylib (the engine and ggml with its Metal backend, in one file,
# exporting the C API alone), kimodo_capi.h, the licences, VERSION.json and
# the weights from this repository's cache.  See docs/SHARED_LIBRARY_PLAN.md.
#
#   scripts/build/build_library_macos.sh                  release package in dist/kimodo-macos
#   scripts/build/build_library_macos.sh --debug          debug build
#   scripts/build/build_library_macos.sh --out <folder>   package somewhere else
#   scripts/build/build_library_macos.sh --no-weights     leave the package's weights as they are
#
# Runs on a Mac with Apple Silicon; the library needs macOS 13.3 or later.
# Needs Xcode or its command line tools (xcode-select --install), CMake 3.25+
# and Ninja (brew install cmake ninja), and Git.  The library is checked on
# the way: it must export the C API and nothing else, load only system
# libraries and frameworks, and load and answer from the package.  The last
# lines printed are the commands that generate a clip through it.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build/build_library_macos.sh [--release | --debug] [--out <folder>] [--no-weights]

  --release     optimised build (the default)
  --debug       debug build
  --out         the package folder (default dist/kimodo-macos)
  --no-weights  leave the package's weights as they are, for a rebuild
                that only changes the library
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
    echo "Run this on a Mac.  Other hosts: build_library_windows.bat, build_library_linux.sh." >&2
    exit 1
fi
missing=()
if ! xcode-select -p >/dev/null 2>&1; then
    missing+=("Xcode's command line tools (xcode-select --install)")
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
native=yes
if [[ "$(uname -m)" != arm64 ]]; then
    native=no
    echo "Note: this Mac is $(uname -m); the library is built for Apple Silicon (arm64) and cannot be loaded here." >&2
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

# ---- Build and package
[[ -n "$out" ]] || out="$root/dist/kimodo-macos"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
build="$root/build/macos-$config"

cd "$root"
echo "Configuring the macos-$config preset..."
cmake --preset "macos-$config"
echo
echo "Building libkimodo.dylib ($config)..."
cmake --build --preset "macos-library-$config"
echo
echo "Checking and packaging into $out..."
cmake -DKIMODO_TARGET=macos "-DKIMODO_SOURCE_DIR=$root" "-DKIMODO_BUILD_DIR=$build" "-DKIMODO_PACKAGE_DIR=$out" \
      "-DKIMODO_WEIGHTS=$weights" -P "$here/package.cmake"
if [[ "$native" == yes ]]; then
    echo
    echo "Loading the packaged library..."
    "$out/tools/kimodo-capi-smoke" --library "$out/lib/libkimodo.dylib"
fi

cat <<EOF

Packaged the Kimodo library ($config) in $out

To generate through it on the GPU (Metal) -- it opens the weights, prints the
Metal device, generates a clip, pins a keyframe as structs and as JSON, and
ends with "ok":
    "$out/tools/kimodo-capi-smoke" --library "$out/lib/libkimodo.dylib" \\
        --data "$out/weights" --frames 90 --steps 20 --keyframe --embedding --log
The same on the CPU, to compare: add --cpu.
EOF
