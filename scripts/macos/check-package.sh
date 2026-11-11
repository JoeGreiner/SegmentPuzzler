#!/usr/bin/env bash
set -euo pipefail

root=${CI_ROOT:?Set CI_ROOT}
mkdir -p "$root/logs" "$root/package-check"
exec > >(tee "$root/logs/check-package.log") 2>&1
shopt -s nullglob
packages=("$root/packages/"*.dmg)
[[ ${#packages[@]} = 1 ]] || { echo 'Expected exactly one DMG' >&2; exit 1; }
mount_dir=$(mktemp -d "$root/package-check/mount.XXXXXX")
trap 'hdiutil detach "$mount_dir" || true; rmdir "$mount_dir" || true' EXIT
hdiutil attach "${packages[0]}" -readonly -nobrowse -mountpoint "$mount_dir"
app="$mount_dir/SegmentPuzzler.app"
test -x "$app/Contents/MacOS/SegmentPuzzler"
test -f "$app/Contents/PlugIns/platforms/libqcocoa.dylib"
test -f "$app/Contents/PlugIns/tls/libqsecuretransportbackend.dylib"
codesign --verify --deep --strict --verbose=2 "$app"

while IFS= read -r -d '' binary; do
    file -b "$binary" | grep -q 'Mach-O' || continue
    lipo "$binary" -verify_arch arm64
    otool -L "$binary"
    # Universal binaries have an unindented header for each architecture.
    if otool -L "$binary" | awk '/^[[:space:]]/ {print $1}' | grep -E '^/' | grep -Ev '^(/usr/lib/|/System/Library/)'; then
        echo "External library dependency in $binary" >&2
        exit 1
    fi
done < <(find "$app" -type f -print0)

# This checks dynamic libraries and Cocoa initialisation before the 3D window is created.
set +e
env -i HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    QT_DEBUG_PLUGINS=1 "$app/Contents/MacOS/SegmentPuzzler" --log-level package-check \
    > "$root/logs/startup.log" 2>&1 &
process=$!
(sleep 60; kill "$process" 2>/dev/null) &
watchdog=$!
wait "$process"
result=$?
kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null
set -e
cat "$root/logs/startup.log"
[[ $result = 1 ]]
grep -Fq 'Invalid value for --log-level: package-check' "$root/logs/startup.log"
