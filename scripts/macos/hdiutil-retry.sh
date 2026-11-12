#!/usr/bin/env bash
set -euo pipefail

if [[ -n ${CI_ROOT:-} ]]; then
    exec 2>>"$CI_ROOT/logs/hdiutil.log"
fi

operation=${1:-}
log_dir=$(mktemp -d "${TMPDIR:-/tmp}/segmentpuzzler-hdiutil.XXXXXX")
trap 'rm -rf "$log_dir"' EXIT
attempt=0

for delay in 1 2 4 8 0; do
    attempt=$((attempt + 1))
    # Regular files keep disk image helpers from holding an output pipe open.
    if LC_ALL=C /usr/bin/hdiutil "$@" >"$log_dir/stdout" 2>"$log_dir/stderr"; then
        result=0
    else
        result=$?
    fi
    cat "$log_dir/stdout"
    cat "$log_dir/stderr" >&2
    if [[ $result -eq 0 ]]; then
        exit 0
    fi
    echo "hdiutil $operation: attempt $attempt failed (exit $result)." >&2
    if [[ $delay -eq 0 ]]; then
        exit "$result"
    fi
    case "$operation" in
        detach) [[ $result -eq 16 ]] || exit "$result" ;;
        create) grep -Fq 'hdiutil: create failed - Resource busy' "$log_dir/stdout" "$log_dir/stderr" || exit "$result" ;;
        *) exit "$result" ;;
    esac
    echo "Disk image is busy; retrying $operation in ${delay}s." >&2
    sleep "$delay"
done
