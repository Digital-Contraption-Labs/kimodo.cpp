#!/usr/bin/env bash
# Copy a Kimodo library package into ContraptionFabricator's tree (see
# docs/SHARED_LIBRARY_PLAN.md, section 8):
#
#   the library, header, licences, VERSION.json  ->  <CF>/external/kimodo/<target>/
#   the weights                                  ->  <CF>/Assets/kimodo/
#
#   scripts/build/stage_to_cf.sh --cf <CF folder> --target macos|ios|linux|android|windows
#   scripts/build/stage_to_cf.sh --cf <CF folder> --target <t> --package <package folder>
#   scripts/build/stage_to_cf.sh --cf <CF folder> --target <t> --no-weights
#
# The package is dist/kimodo-<target> unless --package names another; build
# it first with scripts/build/build_library_<target>.  The library folder is
# made an exact copy of the package, so a file the package dropped goes from
# CF too.  The weights are only added and updated: Assets/kimodo may hold
# CF's own files.  Unchanged files are not copied again.  On Windows,
# stage_to_cf.bat does the same.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build/stage_to_cf.sh --cf <CF folder> --target <target> [--package <folder>] [--no-weights]

  --cf          the ContraptionFabricator folder (it has Assets/)
  --target      macos, ios, linux, android or windows, for dist/kimodo-<target>
  --package     a package folder other than dist/kimodo-<target>
  --no-weights  copy the library only
EOF
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cf=""
target=""
package=""
weights=yes

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --no-weights) weights=no ;;
        --cf|--target|--package)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 needs a value." >&2; exit 2; }
            case "$1" in
                --cf) cf="$2" ;;
                --target) target="$2" ;;
                --package) package="$2" ;;
            esac
            shift ;;
        *) echo "Unknown argument: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$cf" || -z "$target" ]]; then
    echo "--cf and --target are required." >&2
    echo >&2
    usage >&2
    exit 2
fi
command -v rsync >/dev/null 2>&1 || { echo "rsync was not found (apt-get install rsync; macOS has it)." >&2; exit 1; }
[[ -n "$package" ]] || package="$root/dist/kimodo-$target"

# A wrong --cf must not become a folder full of copies: CF has Assets/.
[[ -d "$cf/Assets" ]] || { echo "$cf has no Assets folder, so it is not a ContraptionFabricator tree." >&2; exit 1; }
if [[ ! -f "$package/VERSION.json" || ! -f "$package/include/kimodo/kimodo_capi.h" ]]; then
    echo "$package is not a Kimodo package.  Build it with scripts/build/build_library_$target." >&2
    exit 1
fi
if [[ "$weights" == yes && ! -d "$package/weights" ]]; then
    echo "$package has no weights.  Build it without --no-weights, or stage with --no-weights." >&2
    exit 1
fi
cf="$(cd "$cf" && pwd)"
package="$(cd "$package" && pwd)"

destination="$cf/external/kimodo/$target"
echo "Staging $package"
echo "    library into $destination"
mkdir -p "$destination"
rsync -a --delete --exclude /weights/ "$package/" "$destination/"
if [[ "$weights" == yes ]]; then
    echo "    weights into $cf/Assets/kimodo"
    mkdir -p "$cf/Assets/kimodo"
    rsync -a --exclude .sha256-cache "$package/weights/" "$cf/Assets/kimodo/"
fi
echo
echo "Staged."
