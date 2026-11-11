#!/usr/bin/env bash
set -euo pipefail

root=${CI_ROOT:?Set CI_ROOT}
mkdir -p "$root/packages" "$root/logs"
exec > >(tee "$root/logs/package.log") 2>&1
cpack --config "$root/build/app/CPackConfig.cmake" -G DragNDrop -B "$root/packages"
