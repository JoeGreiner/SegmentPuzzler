#!/usr/bin/env zsh

# hint: build on oldest supported linux version, e.g. ubuntu 16.04, centos 6 etc
# then it should run on newer versions as well. if you build on a newer version, it might not run on older versions,
# e.g. because of libc version mismatch

setopt nullglob
list=(_CPack_Packages build CMakeFiles ITKFactoryRegistration SegmentPuzzler.app SegmentPuzzler_autogen *.cmake CMakeCache.txt Makefile qt.conf install_manifest*.txt)
unsetopt nullglob
for i in "${list[@]}"; do
    if [[ -d $i ]]; then
        echo "$i directory found, deleting..."
        rm -rf "$i"
    elif [[ -f $i ]]; then
        echo "$i file found, deleting..."
        rm "$i"
    fi
done


if [[ "$(uname)" == "Darwin" ]]; then
    os_name="macOS"
    os_version=$(sw_vers -productVersion)
    os_build=$(sw_vers -buildVersion)
    os_info_string="${os_name}_${os_version}_${os_build}"
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

cmake .. -B build/ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
make -C build/ -j 12

cd build/
cpack -G DragNDrop .

# rename appimage with version and os
mv SegmentPuzzler-*.dmg SegmentPuzzler-${project_version}-${os_info_string}.dmg