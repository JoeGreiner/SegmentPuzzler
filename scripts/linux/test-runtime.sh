#!/usr/bin/env bash
set -euo pipefail

repository=$(cd "$(dirname "$0")/../.." && pwd)
root=${CI_ROOT:?Set CI_ROOT}
appimage=${1:?Pass the packaged AppImage}
mkdir -p "$root/logs" "$root/runtime-tests"
exec > >(tee "$root/logs/runtime-tests.log") 2>&1
test_dir=$(mktemp -d "$root/runtime-tests/check.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT

# Exercise the actual 2.2 launcher code, including memfd and its preload helper.
url=$(jq -er '.appimagelauncher_source.url' "$repository/scripts/linux/tools.json")
checksum=$(jq -er '.appimagelauncher_source.sha256' "$repository/scripts/linux/tools.json")
curl --fail --location --retry 3 "$url" -o "$test_dir/launcher.tar.gz"
printf '%s  %s\n' "$checksum" "$test_dir/launcher.tar.gz" | sha256sum --check
mkdir "$test_dir/launcher"
tar -xzf "$test_dir/launcher.tar.gz" -C "$test_dir/launcher" --strip-components=1
launcher_build="$root/build/appimagelauncher-test"
cmake --fresh -S "$test_dir/launcher/src/binfmt-bypass" -B "$launcher_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -D_private_libdir=lib
cmake --build "$launcher_build" --parallel "${BUILD_PARALLEL:-4}"
launcher="$launcher_build/binfmt-bypass"

# SquashFS stores its compression ID at byte 20; 1 is gzip.
offset=$("$appimage" --appimage-offset)
compression=$(od -An -tu2 -j "$((offset + 20))" -N 2 "$appimage" | tr -d '[:space:]')
[[ "$compression" = 1 ]] || { echo 'Expected gzip compression' >&2; exit 1; }

probe_dir="$test_dir/Probe.AppDir"
mkdir "$probe_dir"
cat > "$probe_dir/AppRun" <<'EOF'
#!/bin/sh
set -eu
test -z "${LD_PRELOAD:-}"
test -z "${REDIRECT_APPIMAGE:-}"
test "$#" -eq 2
test "$1" = 'argument with spaces'
test "$2" = '--literal=*'
printf 'Runtime probe passed\n'
exit 37
EOF
chmod +x "$probe_dir/AppRun"
cp "$repository/resources/images/icon-appimage.png" "$probe_dir/probe.png"
cat > "$probe_dir/probe.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Runtime probe
Exec=AppRun
Icon=probe
Categories=Utility;
EOF
probe="$root/runtime-tests/runtime probe.AppImage"
rm -f "$probe"
ARCH=x86_64 "$root/tools/appimagetool-extracted/squashfs-root/AppRun" \
    --comp gzip --runtime-file "$root/runtime/runtime-x86_64" "$probe_dir" "$probe"

for mode in direct legacy target; do
    case "$mode" in
        direct) command=("$probe") ;;
        legacy) command=("$launcher" "$probe") ;;
        # Modern launchers supply TARGET_APPIMAGE to a runtime without payload.
        target) command=(env "TARGET_APPIMAGE=$probe" "$root/runtime/runtime-x86_64") ;;
    esac
    env -i PATH=/usr/bin:/bin "${command[@]}" --appimage-help > "$root/logs/runtime-help-$mode.log" 2>&1
    grep -Fq -- '--appimage-extract' "$root/logs/runtime-help-$mode.log"
    set +e
    output=$(env -i PATH=/usr/bin:/bin APPIMAGE_EXTRACT_AND_RUN=1 \
        timeout 30 "${command[@]}" 'argument with spaces' '--literal=*' 2>&1)
    result=$?
    set -e
    printf '%s: %s (exit %s)\n' "$mode" "$output" "$result"
    [[ "$result" = 37 && "$output" = 'Runtime probe passed' ]]
done
