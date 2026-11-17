#!/usr/bin/env bash
set -euo pipefail

repository=$(cd "$(dirname "$0")/../.." && pwd)
app_source=${APP_SOURCE_DIR:-$repository}
root=${CI_ROOT:?Set CI_ROOT to an absolute build directory}
: "${QT_ROOT_DIR:?Set QT_ROOT_DIR to the Qt installation prefix}"
[[ "$root" = /* ]] || { echo 'CI_ROOT must be absolute' >&2; exit 2; }
mkdir -p "$root/logs"
exec > >(tee "$root/logs/app.log") 2>&1
targets=(SegmentPuzzler)
testing=OFF
if [[ -f "$app_source/cmake/ZlibNg.cmake" ]]; then
    targets+=(check_volume_storage)
    testing=ON
fi
configure=(
    -S "$app_source" -B "$root/build/app" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR;$root/install/itk;$root/install/vtk"
    -DSEGMENT_PUZZLER_QT_MAJOR=6 -DDEPLOY=ON "-DBUILD_TESTING=$testing"
    -DBUILD_BENCHMARKS=OFF -DUSE_OMP=ON -DLINK_TIME_OPTIMIZATION=OFF
)
if [[ $(uname -s) = Darwin ]]; then
    configure+=(
        -DCMAKE_OSX_ARCHITECTURES=arm64
        "-DSEGMENT_PUZZLER_USE_ICON_COMPOSER=${SEGMENT_PUZZLER_USE_ICON_COMPOSER:-ON}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-15.0}"
        "-DOpenMP_ROOT=${OpenMP_ROOT:?Set OpenMP_ROOT to the libomp installation prefix}"
    )
fi
cmake "${configure[@]}"
cmake --build "$root/build/app" --target "${targets[@]}" --parallel "${BUILD_PARALLEL:-3}"
