#!/usr/bin/env bash
set -euo pipefail

stage=${1:?Usage: run.sh image|toolchain|qt|itk|vtk|app|package|check-package}
repository=$(cd "$(dirname "$0")/../.." && pwd)
app_source=${APP_SOURCE_DIR:-$repository}
root=${CI_ROOT:?Set CI_ROOT to an absolute build directory on the host}
[[ "$root" = /* ]] || { echo 'CI_ROOT must be absolute' >&2; exit 2; }
profile=${LINUX_PROFILE:-standard}
case "$profile" in standard|legacy) ;; *) echo "Unknown Linux profile: $profile" >&2; exit 2 ;; esac
profiles="$repository/scripts/linux/profiles.json"
almalinux=$(jq -er --arg profile "$profile" '.[$profile].almalinux' "$profiles")
qt_version=$(jq -er --arg profile "$profile" '.[$profile].qt' "$profiles")
glibc_max=$(jq -er --arg profile "$profile" '.[$profile].glibc' "$profiles")
image="segmentpuzzler-linux-ci-$profile"
if [[ "$stage" = image ]]; then
    exec docker build --platform linux/amd64 --build-arg "ALMALINUX_VERSION=$almalinux" \
        -t "$image" -f "$repository/scripts/linux/Dockerfile" "$repository/scripts/linux"
fi
mkdir -p "$root/logs"
echo "Linux profile=$profile AlmaLinux=$almalinux Qt=$qt_version glibc<=$glibc_max"
workdir=/source
case "$stage" in
    toolchain)
        command=(bash -e -o pipefail -c '
            { rpm -qa | LC_ALL=C sort; gcc --version; g++ --version; cmake --version; ninja --version; } \
                | tee /work/logs/native-toolchain.log
            python3.12 -m pip freeze
        ')
        ;;
    qt)
        workdir=/work/logs
        command=(python3.12 -m aqt install-qt linux desktop "$qt_version" linux_gcc_64 --outputdir /work/qt --modules qtimageformats)
        ;;
    itk|vtk) command=(bash scripts/ci/build-dependency.sh "$stage") ;;
    app) command=(bash scripts/ci/build-app.sh) ;;
    package|check-package) command=(bash "scripts/linux/$stage.sh") ;;
    *) echo "Unknown stage: $stage" >&2; exit 2 ;;
esac
exec docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --workdir "$workdir" \
    -v "$repository:/source:ro" -v "$root:/work" \
    -v "$app_source:/app-source:ro" -e APP_SOURCE_DIR=/app-source \
    -e HOME=/tmp -e "QT_ROOT_DIR=/work/qt/$qt_version/gcc_64" \
    -e "LINUX_PROFILE=$profile" -e "GLIBC_MAX=$glibc_max" \
    -e "BUILD_PARALLEL=${BUILD_PARALLEL:-3}" \
    "$image" "${command[@]}"
