#!/usr/bin/env bash

set -e
set -u
set -o pipefail

# Override only when needed, for example:
# QT_PREFIX=/custom/qt VTK_PREFIX=/custom/vtk ./deploy_linux_old_distros.sh
QT_PREFIX="${QT_PREFIX:-/opt/qt}"
VTK_PREFIX="${VTK_PREFIX:-/opt/vtk-qt6}"
QMAKE="${QMAKE:-$QT_PREFIX/bin/qmake6}"

export APPIMAGE_EXTRACT_AND_RUN=1
export QT_PLUGIN_PATH="$QT_PREFIX/plugins"
export LD_LIBRARY_PATH="$QT_PREFIX/lib:$VTK_PREFIX/lib:$VTK_PREFIX/lib64:${LD_LIBRARY_PATH:-}"

if [[ -d build ]]; then
    echo "build directory found, deleting..."
    rm -rf build
fi

if [[ -d export_linux ]]; then
    echo "export_linux directory found, deleting..."
    rm -rf export_linux
fi

if [[ -f /etc/os-release ]]; then
    os_name=$(grep ^NAME= /etc/os-release | cut -d '=' -f2 | tr -d '"')
    os_version=$(grep ^VERSION_ID= /etc/os-release | cut -d '=' -f2 | tr -d '"')
    os_info_string="${os_name}_${os_version}"
    echo "OS: $os_info_string"
else
    os_info_string="unknown_linux"
fi

if [[ -f ../CMakeLists.txt ]]; then
    project_version=$(grep -E "project\(.*VERSION [0-9]+\.[0-9]+\.[0-9]+\)" ../CMakeLists.txt | \
        sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
    if [[ -n ${project_version:-} ]]; then
        echo "Project Version: $project_version"
    else
        project_version="unknown_version"
        echo "Project version not found in CMakeLists.txt."
    fi
else
    project_version="unknown_version"
    echo "CMakeLists.txt not found. Cannot determine project version."
fi

[[ -f "$QMAKE" ]] || { echo "Error: qmake6 not found at $QMAKE" >&2; exit 1; }
[[ -f "$QT_PREFIX/plugins/platforms/libqxcb.so" ]] || { echo "Error: Qt xcb plugin not found at $QT_PREFIX/plugins/platforms/libqxcb.so" >&2; exit 1; }

# get linuxdeploy
if [[ ! -f linuxdeploy-x86_64.AppImage ]]; then
    echo "linuxdeploy-x86_64.AppImage not found. Downloading..."
    wget https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20230713-1/linuxdeploy-x86_64.AppImage
else
    echo "linuxdeploy-x86_64.AppImage found."
fi
chmod +x linuxdeploy-x86_64.AppImage
# Workaround for AppImage runtime problems in some Docker/CentOS containers.
dd if=/dev/zero bs=1 count=3 seek=8 conv=notrunc of=linuxdeploy-x86_64.AppImage >/dev/null 2>&1 || true

# get appimagetool
if [[ ! -f appimagetool ]]; then
    echo "appimagetool not found. Downloading..."
    wget "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage" -O appimagetool
else
    echo "appimagetool found."
fi
chmod +x appimagetool
# Same workaround as above.
dd if=/dev/zero bs=1 count=3 seek=8 conv=notrunc of=appimagetool >/dev/null 2>&1 || true

cmake .. -B build/ \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DSEGMENT_PUZZLER_QT_MAJOR=6 \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX;$VTK_PREFIX"

cmake --build build/ --parallel "$(nproc)"

basePath=$(pwd)
mkdir -p export_linux/usr/bin
mkdir -p export_linux/usr/share/icons
mkdir -p export_linux/usr/share/applications

iconPath="usr/share/icons/SegmentPuzzler.png"
cp ../resources/images/icon.png "$basePath"/export_linux/$iconPath
cp ../resources/images/icon.png "$basePath"/export_linux/SegmentPuzzler.png

# create a .desktop file
desktopPath=$basePath/export_linux/usr/share/applications/SegmentPuzzler.desktop
echo "Creating .desktop file at $desktopPath"
cat > "$desktopPath" <<'EOF_DESKTOP'
[Desktop Entry]
Type=Application
Name=SegmentPuzzler
Comment=Program to create 3D segmentations interactively
Exec=SegmentPuzzler
Icon=SegmentPuzzler
Terminal=true
Categories=Utility;
EOF_DESKTOP

# Put it here as well for appimagetool.
desktopPath2=$basePath/export_linux/SegmentPuzzler.desktop
cp "$desktopPath" "$desktopPath2"

# Run linuxdeploy without the Qt plugin. The Qt plugin was unstable with the custom Qt6 build.
APPIMAGE_EXTRACT_AND_RUN=1 ./linuxdeploy-x86_64.AppImage \
    --appdir=export_linux \
    --executable=build/SegmentPuzzler \
    --desktop-file=export_linux/usr/share/applications/SegmentPuzzler.desktop \
    --icon-file=export_linux/usr/share/icons/SegmentPuzzler.png

[[ -f export_linux/usr/bin/SegmentPuzzler ]] || { echo "Error: bundled SegmentPuzzler executable not found" >&2; exit 1; }
[[ -e export_linux/AppRun ]] || { echo "Error: AppRun not found" >&2; exit 1; }

echo "Copying Qt plugins manually..."
mkdir -p export_linux/usr/plugins/platforms
mkdir -p export_linux/usr/plugins/tls
mkdir -p export_linux/usr/plugins/imageformats
mkdir -p export_linux/usr/plugins/platformthemes
mkdir -p export_linux/usr/plugins/xcbglintegrations

cp -av "$QT_PREFIX/plugins/platforms/libqxcb.so" export_linux/usr/plugins/platforms/

if [[ -d "$QT_PREFIX/plugins/tls" ]]; then
    cp -av "$QT_PREFIX/plugins/tls/"*.so export_linux/usr/plugins/tls/ 2>/dev/null || true
fi

if [[ -d "$QT_PREFIX/plugins/imageformats" ]]; then
    cp -av "$QT_PREFIX/plugins/imageformats/"*.so export_linux/usr/plugins/imageformats/ 2>/dev/null || true
fi

if [[ -d "$QT_PREFIX/plugins/platformthemes" ]]; then
    cp -av "$QT_PREFIX/plugins/platformthemes/"*.so export_linux/usr/plugins/platformthemes/ 2>/dev/null || true
fi

if [[ -d "$QT_PREFIX/plugins/xcbglintegrations" ]]; then
    cp -av "$QT_PREFIX/plugins/xcbglintegrations/"*.so export_linux/usr/plugins/xcbglintegrations/ 2>/dev/null || true
fi

# This private Qt library is needed by the xcb platform plugin, but linuxdeploy does not see it
# because it is loaded through the plugin rather than through the main executable.
if [[ -f "$QT_PREFIX/lib/libQt6XcbQpa.so.6" ]]; then
    cp -av "$QT_PREFIX/lib/libQt6XcbQpa.so"* export_linux/usr/lib/
else
    echo "Warning: libQt6XcbQpa.so.6 not found in $QT_PREFIX/lib"
fi

echo "Copying extra Qt xcb runtime libraries..."
for lib in \
    libxcb-cursor.so.0 \
    libxcb-image.so.0 \
    libxcb-render-util.so.0 \
    libxcb-keysyms.so.1 \
    libxcb-icccm.so.4 \
    libxcb-xkb.so.1 \
    libxkbcommon-x11.so.0
    do
        path=$(ldconfig -p | awk -v lib="$lib" '$1 == lib { print $NF; exit }')
        if [[ -n "$path" ]]; then
            echo "Copying $lib from $path"
            cp -avL "$path" export_linux/usr/lib/
        else
            echo "Warning: $lib not found on build system"
        fi
    done

# qt.conf makes Qt look for plugins relative to the AppDir.
cat > export_linux/usr/bin/qt.conf <<'EOF_QTCONF'
[Paths]
Prefix = ..
Plugins = plugins
Imports = qml
Qml2Imports = qml
EOF_QTCONF
cp export_linux/usr/bin/qt.conf export_linux/qt.conf

echo "Fixing RPATHs..."
# Important: keep $ORIGIN in single quotes. Do not use "${ORIGIN}".
patchelf --set-rpath '$ORIGIN/../lib' export_linux/usr/bin/SegmentPuzzler

find export_linux/usr/lib -type f \( -name '*.so' -o -name '*.so.*' \) \
    -exec patchelf --set-rpath '$ORIGIN' {} \;

find export_linux/usr/plugins -type f -name '*.so' \
    -exec patchelf --set-rpath '$ORIGIN/../../lib' {} \;

echo "RPATH check for libqxcb.so:"
readelf -d export_linux/usr/plugins/platforms/libqxcb.so | grep -E 'RPATH|RUNPATH' || true

# Check executable dependencies with the AppDir libs.
echo "Checking bundled executable dependencies..."
LD_LIBRARY_PATH="$basePath/export_linux/usr/lib:${LD_LIBRARY_PATH:-}" \
    ldd export_linux/usr/bin/SegmentPuzzler | tee "$basePath/ldd-SegmentPuzzler-appdir.txt"

if grep -q "not found" "$basePath/ldd-SegmentPuzzler-appdir.txt"; then
    grep "not found" "$basePath/ldd-SegmentPuzzler-appdir.txt"
    echo "Error: Missing libraries in bundled executable" >&2
    exit 1
fi

# Check plugin dependencies without LD_LIBRARY_PATH. This verifies that RPATHs are actually correct.
echo "Checking Qt plugin dependencies without LD_LIBRARY_PATH..."
old_ld_library_path="${LD_LIBRARY_PATH:-}"
unset LD_LIBRARY_PATH
find export_linux/usr/plugins -name '*.so' -print0 \
    | xargs -0 -I{} sh -c 'echo "== {}"; ldd "{}" | grep "not found" || true' \
    | tee "$basePath/ldd-qt-plugins-no-ldpath.txt"
export LD_LIBRARY_PATH="$old_ld_library_path"

if grep -q "not found" "$basePath/ldd-qt-plugins-no-ldpath.txt"; then
    grep "not found" "$basePath/ldd-qt-plugins-no-ldpath.txt"
    echo "Error: Missing libraries in Qt plugins without LD_LIBRARY_PATH" >&2
    exit 1
fi

ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 ./appimagetool export_linux --verbose

# rename appimage with version and os
mv SegmentPuzzler-x86_64.AppImage SegmentPuzzler-"${project_version}"-"${os_info_string}"-x86_64.AppImage

echo "Created: SegmentPuzzler-${project_version}-${os_info_string}-x86_64.AppImage"

