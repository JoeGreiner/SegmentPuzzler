#!/bin/sh
set -eu

fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
mkdir -p "$fixture/app/usr/bin" "$fixture/bin"
cp "$(dirname "$0")/AppRun" "$fixture/app/AppRun"

# The launcher uses settings resolved by the executable.
cat > "$fixture/app/usr/bin/SegmentPuzzler" <<'EOF'
#!/bin/sh
test "${APPDIR:-}" = "$TEST_APPDIR" || exit 8
for argument in "$@"; do
    case "$argument" in
        --print-graphics-settings) printf '%s %s\n' "$TEST_MODE" "${TEST_PLATFORM:-auto}"; exit "${TEST_QUERY_STATUS:-0}" ;;
        --reset-graphics) printf 'reset\n'; exit 0 ;;
    esac
done
printf '%s|%s|%s\n' "${__NV_PRIME_RENDER_OFFLOAD:-unset}" "${__GLX_VENDOR_LIBRARY_NAME:-unset}" "${DRI_PRIME:-unset}"
EOF
cat > "$fixture/bin/switcherooctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" >> "$TEST_CALLS"
case "$1" in
    version) printf '3.0\n' ;;
    launch)
        shift
        export __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia
        exec "$@"
        ;;
esac
EOF
# Also runs on macOS without a GNU timeout installation.
cat > "$fixture/bin/timeout" <<'EOF'
#!/bin/sh
shift 3
exec "$@"
EOF
chmod +x "$fixture/app/AppRun" "$fixture/app/usr/bin/SegmentPuzzler" "$fixture/bin/timeout" "$fixture/bin/switcherooctl"
cat > "$fixture/bin/xrandr" <<'EOF'
#!/bin/sh
printf 'xrandr\n' >> "$TEST_CALLS"
exit 1
EOF
chmod +x "$fixture/bin/xrandr"

run() {
    env -i PATH="$fixture/bin:/usr/bin:/bin" TEST_APPDIR="$fixture/app" TEST_CALLS="$fixture/calls" XDG_SESSION_TYPE=wayland \
        "$@" "$fixture/app/AppRun"
}

test "$(run TEST_MODE=system)" = 'unset|unset|unset'
test ! -e "$fixture/calls"
test "$(run TEST_MODE=system SEGMENTPUZZLER_GPU=auto)" = 'unset|unset|unset'
test ! -e "$fixture/calls"
test "$(run TEST_MODE=auto)" = '1|nvidia|unset'
test "$(cat "$fixture/calls")" = "$(printf 'version\nlaunch')"
rm "$fixture/calls"
test "$(run TEST_MODE=auto DRI_PRIME=pci-0000_00_02_0)" = 'unset|unset|pci-0000_00_02_0'
test ! -e "$fixture/calls"
test "$(run TEST_MODE=auto SSH_CONNECTION=remote)" = 'unset|unset|unset'
test ! -e "$fixture/calls"

test "$(env -i PATH="$fixture/bin:/usr/bin:/bin" TEST_APPDIR="$fixture/app" TEST_MODE=system \
    "$fixture/app/AppRun" --print-graphics-settings)" = 'system auto'
test "$(env -i PATH="$fixture/bin:/usr/bin:/bin" TEST_APPDIR="$fixture/app" SEGMENTPUZZLER_GPU=invalid \
    "$fixture/app/AppRun" --reset-graphics)" = reset
if run TEST_MODE=invalid TEST_QUERY_STATUS=7 > /dev/null; then
    exit 1
else
    test "$?" = 7
fi
test ! -e "$fixture/calls"

# Use the saved platform for GPU detection when session hints disagree.
test "$(run TEST_MODE=auto TEST_PLATFORM=wayland XDG_SESSION_TYPE=x11)" = '1|nvidia|unset'
test "$(cat "$fixture/calls")" = "$(printf 'version\nlaunch')"
rm "$fixture/calls"
# Explicit Qt environment and command-line choices take precedence over the stored choice.
test "$(run TEST_MODE=auto TEST_PLATFORM=wayland XDG_SESSION_TYPE=x11 QT_QPA_PLATFORM=xcb)" = 'unset|unset|unset'
test "$(cat "$fixture/calls")" = "$(printf 'version\nlaunch\nxrandr')"
rm "$fixture/calls"
test "$(env -i PATH="$fixture/bin:/usr/bin:/bin" TEST_APPDIR="$fixture/app" TEST_CALLS="$fixture/calls" \
    TEST_MODE=auto TEST_PLATFORM=wayland XDG_SESSION_TYPE=x11 \
    "$fixture/app/AppRun" -platform xcb)" = 'unset|unset|unset'
test "$(cat "$fixture/calls")" = "$(printf 'version\nlaunch\nxrandr')"
printf 'AppRun tests passed\n'
