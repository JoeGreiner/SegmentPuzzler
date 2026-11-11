#!/usr/bin/env bash
set -euo pipefail

stage=${1:?Usage: build-dependency.sh itk|vtk}
case "$stage" in itk|vtk) ;; *) echo "Unknown dependency: $stage" >&2; exit 2 ;; esac
script_dir=$(cd "$(dirname "$0")" && pwd)
root=${CI_ROOT:?Set CI_ROOT to an absolute build directory}
[[ "$root" = /* ]] || { echo 'CI_ROOT must be absolute' >&2; exit 2; }
parallel=${BUILD_PARALLEL:-3}
source_dir="$root/src/$stage"
build_dir="$root/build/$stage"
install_dir="$root/install/$stage"
mkdir -p "$source_dir" "$build_dir" "$install_dir" "$root/logs"
exec > >(tee "$root/logs/$stage.log") 2>&1

repository=$(jq -er --arg stage "$stage" '.[$stage].repository' "$script_dir/dependencies.json")
commit=$(jq -er --arg stage "$stage" '.[$stage].commit' "$script_dir/dependencies.json")
echo "Building $stage at $commit"
curl --fail --location --retry 3 "https://github.com/$repository/archive/$commit.tar.gz" -o "$root/$stage.tar.gz"
tar -xzf "$root/$stage.tar.gz" --strip-components=1 -C "$source_dir"

configure=(
    -S "$source_dir" -B "$build_dir" -G Ninja
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$install_dir"
    -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_TESTING=OFF
)
if [[ $(uname -s) = Darwin ]]; then
    configure+=(-DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-15.0}")
fi
if [[ "$stage" = itk ]]; then
    configure+=(
        -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF
        -DITK_BUILD_DEFAULT_MODULES=ON -DITK_WRAP_PYTHON=OFF
        -DITK_USE_SYSTEM_LIBRARIES=OFF
    )
else
    : "${QT_ROOT_DIR:?Set QT_ROOT_DIR to the Qt installation prefix}"
    configure+=(
        -DBUILD_SHARED_LIBS=ON -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF
        -DVTK_WRAP_PYTHON=OFF -DVTK_WRAP_JAVA=OFF -DVTK_QT_VERSION=6
        "-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR" -DVTK_SMP_IMPLEMENTATION_TYPE=STDThread
        -DVTK_GROUP_ENABLE_MPI=NO
    )
    for group in StandAlone Rendering Imaging Qt Views Web; do
        configure+=("-DVTK_GROUP_ENABLE_${group}=DONT_WANT")
    done
    for module in CommonCore CommonDataModel FiltersCore FiltersGeneral FiltersSources \
        ImagingCore ImagingGeneral RenderingCore RenderingOpenGL2 RenderingAnnotation \
        InteractionStyle InteractionWidgets GUISupportQt; do
        configure+=("-DVTK_MODULE_ENABLE_VTK_${module}=YES")
    done
fi
cmake "${configure[@]}"
cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"
