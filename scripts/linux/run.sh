#!/usr/bin/env bash
set -euo pipefail

stage=${1:?Usage: run.sh image|toolchain|qt|itk|vtk|app|package|smoke}
repository=$(cd "$(dirname "$0")/../.." && pwd)
root=${CI_ROOT:?Set CI_ROOT to an absolute build directory on the host}
[[ "$root" = /* ]] || { echo 'CI_ROOT must be absolute' >&2; exit 2; }
image=segmentpuzzler-linux-ci
if [[ "$stage" = image ]]; then
    exec docker build --platform linux/amd64 -t "$image" -f "$repository/scripts/linux/Dockerfile" "$repository/scripts/linux"
fi
qt_version=$(jq -er '.qt.version' "$repository/scripts/ci/dependencies.json")
mkdir -p "$root/logs"
workdir=/source
case "$stage" in
    toolchain) command=(bash -c 'rpm -qa | LC_ALL=C sort; gcc --version; cmake --version; python3.12 -m pip freeze') ;;
    qt)
        workdir=/work/logs
        command=(python3.12 -m aqt install-qt linux desktop "$qt_version" linux_gcc_64 --outputdir /work/qt --modules qtimageformats)
        ;;
    itk|vtk) command=(bash scripts/ci/build-dependency.sh "$stage") ;;
    app) command=(bash scripts/ci/build-app.sh) ;;
    package|smoke) command=(bash "scripts/linux/$stage.sh") ;;
    *) echo "Unknown stage: $stage" >&2; exit 2 ;;
esac
exec docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --workdir "$workdir" \
    -v "$repository:/source:ro" -v "$root:/work" \
    -e HOME=/tmp -e "QT_ROOT_DIR=/work/qt/$qt_version/gcc_64" \
    -e "BUILD_PARALLEL=${BUILD_PARALLEL:-3}" \
    "$image" "${command[@]}"
