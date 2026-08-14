`SegmentPuzzler.icon` is the single source for the Linux, macOS,
and Windows application icons. Its `Assets/SegmentPuzzler-heart.svg` file
contains the editable artwork. Edit that SVG with a vector editor and use
Apple Icon Composer for the background, scale, shadow, and platform styling.

After saving the `.icon` document, run `resources/images/generate_icons_macos.sh`
from the repository root to update the Linux/Qt PNG and Windows ICO. Do not
edit the generated PNG or ICO directly.
