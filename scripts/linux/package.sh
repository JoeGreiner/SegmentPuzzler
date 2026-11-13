#!/usr/bin/env bash
# shellcheck disable=SC2016 # The ELF loader expands $ORIGIN, not this shell.
set -euo pipefail

repository=$(cd "$(dirname "$0")/../.." && pwd)
app_source=${APP_SOURCE_DIR:-$repository}
root=${CI_ROOT:?Set CI_ROOT}
: "${QT_ROOT_DIR:?Set QT_ROOT_DIR}"
: "${GLIBC_MAX:?Set GLIBC_MAX via scripts/linux/run.sh}"
case "${LINUX_PROFILE:?Set LINUX_PROFILE via scripts/linux/run.sh}" in
    standard) suffix= ;;
    legacy) suffix=-legacy ;;
    *) echo "Unknown Linux profile: $LINUX_PROFILE" >&2; exit 2 ;;
esac
mkdir -p "$root/tools" "$root/packages" "$root/logs"
exec > >(tee "$root/logs/package.log") 2>&1
test -x "$root/runtime/runtime-x86_64"
test -x "$root/runtime/mksquashfs"
for tool in linuxdeploy appimagetool; do
    url=$(jq -er --arg tool "$tool" '.[$tool].url' "$repository/scripts/linux/tools.json")
    checksum=$(jq -er --arg tool "$tool" '.[$tool].sha256' "$repository/scripts/linux/tools.json")
    curl --fail --location --retry 3 "$url" -o "$root/tools/$tool"
    printf '%s  %s\n' "$checksum" "$root/tools/$tool" | sha256sum --check
    chmod +x "$root/tools/$tool"
done

# Replace appimagetool's zstd-only packer in its extracted bundle.
mkdir -p "$root/tools/appimagetool-extracted"
(
    cd "$root/tools/appimagetool-extracted"
    "$root/tools/appimagetool" --appimage-extract > "$root/logs/appimagetool-extract.log"
)
tool_dir="$root/tools/appimagetool-extracted/squashfs-root"
test -x "$tool_dir/usr/bin/mksquashfs"
install -m 755 "$root/runtime/mksquashfs" "$tool_dir/usr/bin/mksquashfs"

appdir=$(mktemp -d "$root/SegmentPuzzler.AppDir.XXXXXX")
mkdir -p "$appdir/usr/bin" "$appdir/usr/plugins"
cp "$app_source/resources/images/icon-appimage.png" "$appdir/SegmentPuzzler.png"
cat > "$appdir/SegmentPuzzler.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=SegmentPuzzler
Comment=Interactive 3D segmentation
Exec=SegmentPuzzler
Icon=SegmentPuzzler
Terminal=false
Categories=Science;Graphics;
PrefersNonDefaultGPU=true
X-KDE-RunOnDiscreteGpu=true
EOF
desktop-file-validate "$appdir/SegmentPuzzler.desktop"

deploy_args=(
    --appdir "$appdir" --executable "$root/build/app/SegmentPuzzler"
    --desktop-file "$appdir/SegmentPuzzler.desktop" --icon-file "$appdir/SegmentPuzzler.png"
    --library /usr/lib64/libssl.so.3 --library /usr/lib64/libcrypto.so.3
)
# Explicitly include dynamically loaded Qt plugins and their dependencies.
for plugin_dir in platforms imageformats tls xcbglintegrations \
    wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
    [[ -d "$QT_ROOT_DIR/plugins/$plugin_dir" ]] || continue
    mkdir -p "$appdir/usr/plugins/$plugin_dir"
    while IFS= read -r -d '' plugin; do
        if [[ "$plugin_dir" = platforms ]]; then
            case $(basename "$plugin") in libqxcb.so|libqwayland*.so) ;; *) continue ;; esac
        fi
        destination="$appdir/usr/plugins/$plugin_dir/$(basename "$plugin")"
        cp -L "$plugin" "$destination"
        deploy_args+=(--deploy-deps-only "$destination")
    done < <(find "$QT_ROOT_DIR/plugins/$plugin_dir" -maxdepth 1 -name '*.so' -print0)
done
test -f "$appdir/usr/plugins/platforms/libqxcb.so"
test -f "$appdir/usr/plugins/imageformats/libqtiff.so"
test -f "$appdir/usr/plugins/tls/libqopensslbackend.so"
export APPIMAGE_EXTRACT_AND_RUN=1
export LD_LIBRARY_PATH="$QT_ROOT_DIR/lib:$root/install/vtk/lib:$root/install/vtk/lib64"
"$root/tools/linuxdeploy" "${deploy_args[@]}"
unset LD_LIBRARY_PATH

cat > "$appdir/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
EOF
# Replace linuxdeploy's launcher with a relative, self-contained entry point.
rm -f "$appdir/AppRun"
install -m 755 "$repository/scripts/linux/AppRun" "$appdir/AppRun"
patchelf --set-rpath '$ORIGIN/../lib' "$appdir/usr/bin/SegmentPuzzler"
while IFS= read -r -d '' library; do
    patchelf --set-rpath '$ORIGIN' "$library"
done < <(find "$appdir/usr/lib" -type f -name '*.so*' -print0)
while IFS= read -r -d '' plugin; do
    patchelf --set-rpath '$ORIGIN/../../lib' "$plugin"
done < <(find "$appdir/usr/plugins" -type f -name '*.so' -print0)

while IFS= read -r -d '' binary; do
    file -b "$binary" | grep -q ELF || continue
    echo "Checking $binary"
    dependencies=$(ldd "$binary")
    printf '%s\n' "$dependencies"
    if grep -q 'not found' <<< "$dependencies"; then exit 1; fi
    if grep -E '/work/(qt|install|build)/' <<< "$dependencies"; then
        echo 'Dependency escaped the AppDir' >&2; exit 1
    fi
    readelf --version-info "$binary" | awk '{for (i=1; i<=NF; i++) if ($i ~ /^GLIBC_[0-9]/) print $i}' | sort -Vu > "$root/logs/glibc-current.txt"
    if awk -F '[_.]' -v limit="$GLIBC_MAX" \
        'BEGIN {split(limit, max, ".")} $2 > max[1] || ($2 == max[1] && $3 > max[2]) {bad=1} END {exit !bad}' \
        "$root/logs/glibc-current.txt"; then
        echo "Packaged library requires glibc newer than $GLIBC_MAX" >&2; exit 1
    fi
done < <(find "$appdir/usr" -type f -print0)

version=$(sed -nE 's/^project\(.*VERSION ([0-9.]+)\).*/\1/p' "$app_source/CMakeLists.txt")
test -n "$version"
ARCH=x86_64 "$tool_dir/AppRun" --comp gzip --runtime-file "$root/runtime/runtime-x86_64" \
    "$appdir" "$root/packages/SegmentPuzzler-$version-linux-x86_64$suffix.AppImage"
