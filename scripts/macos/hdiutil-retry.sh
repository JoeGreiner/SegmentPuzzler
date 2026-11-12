#!/usr/bin/env bash
set -euo pipefail

if [[ -n ${CI_ROOT:-} ]]; then
    exec 2>>"$CI_ROOT/logs/hdiutil.log"
fi

for delay in 1 2 4 8 0; do
    if /usr/bin/hdiutil "$@"; then
        exit 0
    else
        result=$?
    fi
    if [[ ${1:-} != detach || $result -ne 16 || $delay -eq 0 ]]; then
        exit "$result"
    fi
    echo "Disk image is busy; retrying detach in ${delay}s." >&2
    sleep "$delay"
done
