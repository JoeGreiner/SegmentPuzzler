#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/cmake-build-debug}"
TARGET_NAME="SegmentPuzzler"
APP_BUNDLE="${BUILD_DIR}/${TARGET_NAME}.app"
PARALLEL_JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-8}"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<EOF
Usage: $(basename "$0") [app args...]

Configures ${TARGET_NAME} in Debug mode, builds it in ${BUILD_DIR},
and launches the macOS app bundle normally.
EOF
    exit 0
fi

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD_DIR}" --target "${TARGET_NAME}" --parallel "${PARALLEL_JOBS}"

if [[ ! -d "${APP_BUNDLE}" ]]; then
    echo "Expected app bundle not found: ${APP_BUNDLE}" >&2
    exit 1
fi

if [[ "$#" -eq 0 ]]; then
    open "${APP_BUNDLE}"
else
    open "${APP_BUNDLE}" --args "$@"
fi
