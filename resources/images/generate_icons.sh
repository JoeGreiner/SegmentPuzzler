#!/bin/bash

# Check if input file is provided
# here: 256x256 icon file
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <icon.png>"
    exit 1
fi

ICON_FILE="$1"

if [ ! -f "$ICON_FILE" ]; then
    echo "Error: File '$ICON_FILE' not found."
    exit 1
fi

ICONSET_FOLDER="SegmentPuzzler.iconset"
mkdir -p "$ICONSET_FOLDER"

echo "Generating icon sizes..."
sips -z 16 16     "$ICON_FILE" --out "$ICONSET_FOLDER/icon_16x16.png"
sips -z 32 32     "$ICON_FILE" --out "$ICONSET_FOLDER/icon_32x32.png"
sips -z 64 64     "$ICON_FILE" --out "$ICONSET_FOLDER/icon_32x32@2x.png"
sips -z 128 128   "$ICON_FILE" --out "$ICONSET_FOLDER/icon_128x128.png"
sips -z 256 256   "$ICON_FILE" --out "$ICONSET_FOLDER/icon_128x128@2x.png"
sips -z 256 256   "$ICON_FILE" --out "$ICONSET_FOLDER/icon_256x256.png"

# Copy the original 256x256 icon as the high-res version
# Ideally one would have a 512x512 icon to use here, but we'll just use the original icon for simplicity
cp "$ICON_FILE" "$ICONSET_FOLDER/icon_256x256@2x.png"

echo "Converting to .icns..."
iconutil -c icns "$ICONSET_FOLDER"

if [ -f "SegmentPuzzler.icns" ]; then
    echo "Icon successfully generated: SegmentPuzzler.icns"
else
    echo "Error: Failed to generate .icns file."
    exit 1
fi

rm -r "$ICONSET_FOLDER"
echo "Done!"

