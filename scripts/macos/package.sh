#!/usr/bin/env bash
set -euo pipefail

root=${CI_ROOT:?Set CI_ROOT}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$root/packages" "$root/logs"
exec > >(tee "$root/logs/package.log") 2>&1
cpack --verbose --config "$root/build/app/CPackConfig.cmake" -G DragNDrop -B "$root/packages" \
    -D "CPACK_COMMAND_HDIUTIL=\"$script_dir/hdiutil-retry.sh\""
