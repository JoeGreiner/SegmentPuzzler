#!/usr/bin/env bash
set -euo pipefail

root=${CI_ROOT:?Set CI_ROOT}
mkdir -p "$root/logs"
exec > >(tee "$root/logs/smoke.log") 2>&1
shopt -s nullglob
packages=("$root/packages/"*.AppImage)
[[ ${#packages[@]} = 1 ]] || { echo 'Expected exactly one AppImage' >&2; exit 1; }
chmod +x "${packages[0]}"
Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp > "$root/logs/xvfb.log" 2>&1 &
xvfb=$!
trap 'kill "$xvfb" 2>/dev/null || true' EXIT
for _ in {1..50}; do
    [[ -S /tmp/.X11-unix/X99 ]] && break
    kill -0 "$xvfb"
    sleep 0.1
done
test -S /tmp/.X11-unix/X99

# Exercise the packaged X11 plugin in a clean environment, without relying on FUSE.
set +e
env -i HOME="${HOME:-/tmp}" PATH=/usr/bin:/bin DISPLAY=:99 \
    QT_QPA_PLATFORM=xcb QT_DEBUG_PLUGINS=1 LIBGL_ALWAYS_SOFTWARE=1 \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    timeout 60 "${packages[0]}" --log-level package-smoke \
    > "$root/logs/startup.log" 2>&1
result=$?
set -e
cat "$root/logs/startup.log"
[[ $result = 1 ]]
grep -Fq 'Invalid value for --log-level: package-smoke' "$root/logs/startup.log"
