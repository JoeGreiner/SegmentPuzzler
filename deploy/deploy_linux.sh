#!/usr/bin/env zsh

# hint: build on oldest supported linux version, e.g. ubuntu 16.04, centos 6 etc
# then it should run on newer versions as well. if you build on a newer version, it might not run on older versions,
# e.g. because of libc version mismatch

set -euo pipefail

fail() {
    echo "Error: $1" >&2
    exit 1
}

require_file() {
    local required_path=$1
    local description=$2
    [[ -f "$required_path" ]] || fail "$description not found at $required_path"
}

if [[ -d build ]]; then
    echo "build directory found, deleting..."
    rm -r build
fi

if [[ -d export_linux ]]; then
    echo "export_linux directory found, deleting..."
    rm -r export_linux
fi

if [[ -f /etc/os-release ]]; then
    os_name=$(grep ^NAME= /etc/os-release | cut -d '=' -f2 | tr -d '"')
    os_version=$(grep ^VERSION_ID= /etc/os-release | cut -d '=' -f2 | tr -d '"')
    os_info_string="${os_name}_${os_version}"
    echo "OS: $os_info_string"
fi

if [[ -f ../CMakeLists.txt ]]; then
    project_version=$(grep -E "project\(.*VERSION [0-9]+\.[0-9]+\.[0-9]+\)" ../CMakeLists.txt | \
        sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
    if [[ -n $project_version ]]; then
        echo "Project Version: $project_version"
    else
        echo "Project version not found in CMakeLists.txt."
    fi
else
    echo "CMakeLists.txt not found. Cannot determine project version."
fi

## get linuxdeploy and linuxdeploy-plugin-qt
# get linux deploy if it does not exist
if [[ ! -f linuxdeploy-x86_64.AppImage ]]; then
    echo "linuxdeploy-x86_64.AppImage not found. Downloading..."
#    wget https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
# this is weird: for newer versions, I get the error 'execv error: No such file or directory'
    wget https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20230713-1/linuxdeploy-x86_64.AppImage
else
    echo "linuxdeploy-x86_64.AppImage found."
fi
chmod u+x linuxdeploy-x86_64.AppImage

# get qt plugin
if [[ ! -f linuxdeploy-plugin-qt-x86_64.AppImage ]]; then
    echo "linuxdeploy-plugin-qt-x86_64.AppImage not found. Downloading..."
    wget https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
else
    echo "linuxdeploy-plugin-qt-x86_64.AppImage found."
fi
chmod u+x linuxdeploy-plugin-qt-x86_64.AppImage


## set environment variables

# make assertion QT6_DIR is set, this returns true if QT6_DIR is empty
if [[ -z ${QT6_DIR:-} ]]; then
    echo "QT6_DIR is not set."
    # check if tmp file exists, if so read and export
    if [[ -f tmp_QT6_env_var.txt ]]; then
        echo "tmp_QT6_env_var.txt found, reading QT6_DIR from file."
        QT6_DIR=$(cat tmp_QT6_env_var.txt)
        export QT6_DIR
        echo "QT6_DIR set to $QT6_DIR"
    else
      echo "Please set QT6_DIR, e.g. to: ../../QT/6.5.2/gcc_64/ or ../../QT/6.5.2/gcc_64/lib/cmake/Qt6"
      read -r QT6_DIR
      export QT6_DIR
    fi
fi

qt6_input_dir=${QT6_DIR#-DQt6_DIR=}
qt6_input_dir=${qt6_input_dir%/}

if [[ -d "$qt6_input_dir/bin" && -d "$qt6_input_dir/lib/cmake/Qt6" ]]; then
    QT6_ROOT_DIR=$qt6_input_dir
    QT6_CMAKE_DIR=$qt6_input_dir/lib/cmake/Qt6
elif [[ -d "$qt6_input_dir" && "$(basename "$qt6_input_dir")" == "Qt6" ]]; then
    QT6_CMAKE_DIR=$qt6_input_dir
    QT6_ROOT_DIR=$(cd "$qt6_input_dir/../../.." && pwd)
else
    fail "QT6_DIR must point to the Qt root or to lib/cmake/Qt6. Current value: $QT6_DIR"
fi

QT6_LIB_DIR=$QT6_ROOT_DIR/lib
QMAKE=$QT6_ROOT_DIR/bin/qmake
QT6_PLATFORM_PLUGIN=$QT6_ROOT_DIR/plugins/platforms/libqxcb.so

require_file "$QMAKE" "qmake"
require_file "$QT6_LIB_DIR/libQt6Core.so.6" "Qt6Core runtime"
require_file "$QT6_PLATFORM_PLUGIN" "Qt xcb platform plugin"
echo "$QT6_ROOT_DIR" > tmp_QT6_env_var.txt

export QT6_DIR=$QT6_CMAKE_DIR
export QMAKE
export CMAKE_PREFIX_PATH="$QT6_ROOT_DIR:$QT6_ROOT_DIR/lib/cmake:${CMAKE_PREFIX_PATH:-}"
export PATH="$QT6_ROOT_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$QT6_LIB_DIR:${LD_LIBRARY_PATH:-}"
export APPIMAGELAUNCHER_DISABLE=1

echo "Using Qt6 root: $QT6_ROOT_DIR"
echo "Using Qt6 CMake dir: $QT6_CMAKE_DIR"
echo "Using qmake: $QMAKE"
echo "Using Qt6 platform plugin: $QT6_PLATFORM_PLUGIN"
echo "Disabling AppImageLauncher integration prompts for local deploy tools"

cmake .. -B build/ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DQt6_DIR="$QT6_CMAKE_DIR"
make -C build/ -j 12

basePath=$(pwd)
mkdir -p export_linux/usr/bin
mkdir -p export_linux/usr/share/icons
mkdir -p export_linux/usr/share/applications


iconPath="usr/share/icons/SegmentPuzzler.png"
cp ../resources/images/icon.png "$basePath"/export_linux/$iconPath
cp ../resources/images/icon.png "$basePath"/export_linux/SegmentPuzzler.png

#mkdir -p export_linux/usr/share/icons/hicolor/16x16/apps
#mkdir -p export_linux/usr/share/icons/hicolor/32x32/apps
#mkdir -p export_linux/usr/share/icons/hicolor/64x64/apps
#mkdir -p export_linux/usr/share/icons/hicolor/128x128/apps
#mkdir -p export_linux/usr/share/icons/hicolor/256x256/apps
#iconPath16="usr/share/icons/hicolor/16x16/apps/SegmentPuzzler.png"
#iconPath32="usr/share/icons/hicolor/32x32/apps/SegmentPuzzler.png"
#iconPath64="usr/share/icons/hicolor/64x64/apps/SegmentPuzzler.png"
#iconPath128="usr/share/icons/hicolor/128x128/apps/SegmentPuzzler.png"
#iconPath256="usr/share/icons/hicolor/256x256/apps/SegmentPuzzler.png"
#convert ../resources/images/icon.png -resize 16x16 $basePath/export_linux/$iconPath16
#convert ../resources/images/icon.png -resize 32x32 $basePath/export_linux/$iconPath32
#convert ../resources/images/icon.png -resize 64x64 $basePath/export_linux/$iconPath64
#convert ../resources/images/icon.png -resize 128x128 $basePath/export_linux/$iconPath128
#convert ../resources/images/icon.png -resize 256x256 $basePath/export_linux/$iconPath256


# create a .desktop file
desktopPath=$basePath/export_linux/usr/share/applications/SegmentPuzzler.desktop
echo "Creating .desktop file at $desktopPath"
echo "[Desktop Entry]
Type=Application
Name=SegmentPuzzler
Comment=Program to create 3D segmentations interactively
Exec=SegmentPuzzler
Icon=SegmentPuzzler
Terminal=true
Categories=Utility;" > "$desktopPath"

# I'm likely doing something wrong, but if I don't put it here as well, it won't work in appimage
desktopPath2=$basePath/export_linux/SegmentPuzzler.desktop
cp "$desktopPath" "$desktopPath2"

# run linux deploy
./linuxdeploy-x86_64.AppImage --appdir=export_linux/ -e build/SegmentPuzzler --plugin qt

require_file "$basePath/export_linux/usr/bin/SegmentPuzzler" "Bundled SegmentPuzzler executable"
require_file "$basePath/export_linux/usr/lib/libQt6Core.so.6" "Bundled Qt6Core runtime"
require_file "$basePath/export_linux/usr/plugins/platforms/libqxcb.so" "Bundled Qt xcb platform plugin"

cat > "$basePath/export_linux/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ../
Plugins = plugins
Imports = qml
Qml2Imports = qml
EOF

cat > "$basePath/export_linux/AppRun" <<'EOF'
#!/usr/bin/env bash

set -euo pipefail

this_dir="$(readlink -f "$(dirname "$0")")"
app_dir="$this_dir"
qt_conf="$app_dir/usr/bin/qt.conf"
main_executable="$app_dir/usr/bin/SegmentPuzzler"
qt_core="$app_dir/usr/lib/libQt6Core.so.6"
qt_platform_dir="$app_dir/usr/plugins/platforms"
qt_platform_plugin="$qt_platform_dir/libqxcb.so"

show_error() {
    local message=$1
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title="SegmentPuzzler packaging error" --text="$message" || true
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --error "$message" --title "SegmentPuzzler packaging error" || true
    elif command -v xmessage >/dev/null 2>&1; then
        xmessage -center "$message" || true
    fi
    printf '%s\n' "$message" >&2
}

for path_info in \
    "$main_executable|SegmentPuzzler executable" \
    "$qt_conf|qt.conf" \
    "$qt_core|Qt6Core runtime" \
    "$qt_platform_plugin|Qt xcb platform plugin"
do
    path=${path_info%%|*}
    description=${path_info#*|}
    if [[ ! -f "$path" ]]; then
        show_error "SegmentPuzzler is missing ${description} at ${path}. The AppImage is incomplete. Rebuild it with deploy/deploy_linux.sh using one Qt 6 installation."
        exit 1
    fi
done

export QT_PLUGIN_PATH="$app_dir/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$qt_platform_dir"
export LD_LIBRARY_PATH="$app_dir/usr/lib:${LD_LIBRARY_PATH:-}"

case "${XDG_CURRENT_DESKTOP:-}" in
    *GNOME*|*gnome*|*XFCE*)
        export QT_QPA_PLATFORMTHEME=gtk2
        ;;
esac

exec "$app_dir/AppRun.wrapped" "$@"
EOF

chmod +x "$basePath/export_linux/AppRun"

# copy stuff
# now done on the fly in the code
#echo "Copying ../sampleData/Stack.nrrd to export_linux/"
#mkdir -p export_linux/sampleData
#cp ../sampleData/Stack.nrrd export_linux/sampleData/

# link to base folder from usr/bin/SegmentPuzzler
#echo "Creating link to usr/bin/SegmentPuzzler"
#ln -s usr/bin/SegmentPuzzler export_linux/SegmentPuzzler

wget "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage" -O appimagetool
chmod +x appimagetool
./appimagetool export_linux --verbose

# rename appimage with version and os
mv SegmentPuzzler-x86_64.AppImage SegmentPuzzler-"${project_version}"-"${os_info_string}"-x86_64.AppImage

#gio set -t 'string' "${APPIMAGE}" 'metadata::custom-icon' "file://${iconPath}"
