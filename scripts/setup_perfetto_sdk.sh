#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PERFETTO_VERSION="v53.0"
DEST_DIR="${1:-${REPO_ROOT}/thirdparty/perfetto-cpp-sdk-src}"
SOURCE_ARCHIVE_URL="https://codeload.github.com/google/perfetto/zip/refs/tags/${PERFETTO_VERSION}"
CURL_OPTS=(-fL --retry 3 --retry-delay 2 --retry-all-errors)

TMP_DIR="$(mktemp -d)"
ARCHIVE_PATH="${TMP_DIR}/perfetto-download.zip"
EXTRACT_DIR="${TMP_DIR}/extract"
trap 'rm -rf "${TMP_DIR}"' EXIT

mkdir -p "${EXTRACT_DIR}" "${DEST_DIR}"

echo "Downloading ${SOURCE_ARCHIVE_URL}"
curl "${CURL_OPTS[@]}" "${SOURCE_ARCHIVE_URL}" -o "${ARCHIVE_PATH}"

(
    cd "${EXTRACT_DIR}"
    cmake -E tar xf "${ARCHIVE_PATH}"
)

SDK_SOURCE_DIR="${EXTRACT_DIR}"
if [[ ! -f "${SDK_SOURCE_DIR}/perfetto.cc" || ! -f "${SDK_SOURCE_DIR}/perfetto.h" ]]; then
    PERFETTO_CC_PATH="$(find "${EXTRACT_DIR}" -type f -name perfetto.cc -print -quit)"
    PERFETTO_H_PATH="$(find "${EXTRACT_DIR}" -type f -name perfetto.h -print -quit)"
    if [[ -z "${PERFETTO_CC_PATH}" || -z "${PERFETTO_H_PATH}" ]]; then
        echo "Failed to locate perfetto.cc and perfetto.h in downloaded archive." >&2
        exit 1
    fi
    SDK_SOURCE_DIR="$(dirname "${PERFETTO_CC_PATH}")"
fi

install -m 0644 "${SDK_SOURCE_DIR}/perfetto.cc" "${DEST_DIR}/perfetto.cc"
install -m 0644 "${SDK_SOURCE_DIR}/perfetto.h" "${DEST_DIR}/perfetto.h"

echo "Perfetto SDK installed to ${DEST_DIR}"
