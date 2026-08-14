#!/bin/bash

set -euo pipefail

if [ "$#" -ne 0 ]; then
    echo "Usage: $0"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ICON_DOCUMENT="$SCRIPT_DIR/SegmentPuzzler.icon"
OUTPUT_PNG="$SCRIPT_DIR/icon.png"
OUTPUT_ICO="$SCRIPT_DIR/SegmentPuzzler.ico"

if [ ! -d "$ICON_DOCUMENT" ]; then
    echo "Error: Icon Composer document not found: $ICON_DOCUMENT"
    exit 1
fi

if ! command -v xcode-select >/dev/null 2>&1; then
    echo "Error: Xcode 26 or newer is required."
    exit 1
fi

DEVELOPER_DIR="$(xcode-select -p)"
ICON_COMPOSER_TOOL="${DEVELOPER_DIR%/Developer}/Applications/Icon Composer.app/Contents/Executables/ictool"
if [ ! -x "$ICON_COMPOSER_TOOL" ]; then
    echo "Error: Icon Composer's ictool was not found. Install Xcode 26 or newer."
    exit 1
fi

if ! python3 -c 'from PIL import Image' >/dev/null 2>&1; then
    echo "Error: Pillow is required. Install it with: python3 -m pip install Pillow"
    exit 1
fi

BUILD_FOLDER="$(mktemp -d "${TMPDIR:-/tmp}/SegmentPuzzler-icons.XXXXXX")"
TEMP_PNG="$BUILD_FOLDER/icon.png"
TEMP_ICO="$BUILD_FOLDER/SegmentPuzzler.ico"

cleanup() {
    rm -rf "$BUILD_FOLDER"
}
trap cleanup EXIT

echo "Rendering the Icon Composer document..."
"$ICON_COMPOSER_TOOL" "$ICON_DOCUMENT" \
    --export-image \
    --output-file "$TEMP_PNG" \
    --platform macOS \
    --rendition Default \
    --width 1024 \
    --height 1024 \
    --scale 1

echo "Generating the Windows icon..."
python3 -c '
import sys
from PIL import Image

source = Image.open(sys.argv[1]).convert("RGBA")
if source.size != (1024, 1024):
    raise SystemExit(f"Expected a 1024 x 1024 export, got {source.size[0]} x {source.size[1]}")
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
source.save(sys.argv[2], "ICO", sizes=sizes)
' "$TEMP_PNG" "$TEMP_ICO"

mv "$TEMP_PNG" "$OUTPUT_PNG"
mv "$TEMP_ICO" "$OUTPUT_ICO"

echo "Generated: $OUTPUT_PNG"
echo "Generated: $OUTPUT_ICO"
