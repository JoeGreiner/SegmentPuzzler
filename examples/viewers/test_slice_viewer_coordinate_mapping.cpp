#include "src/viewers/SliceViewerCoordinateMapping.h"

#include <climits>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expectMapping(const std::string &name,
                   int sourceExtent,
                   int targetExtent,
                   const std::vector<int> &expected) {
    for (int position = 0; position < static_cast<int>(expected.size()); ++position) {
        const int actual = slice_viewer_geometry::sourcePixelForPaintedPixel(
                position, sourceExtent, targetExtent);
        if (actual != expected[static_cast<std::size_t>(position)]) {
            std::cerr << name << ": position " << position << " mapped to " << actual
                      << ", expected " << expected[static_cast<std::size_t>(position)] << '\n';
            return false;
        }
    }
    return true;
}

bool expectValue(const std::string &name, int actual, int expected) {
    if (actual == expected) {
        return true;
    }
    std::cerr << name << ": got " << actual << ", expected " << expected << '\n';
    return false;
}

} // namespace

int main() {
    bool passed = true;

    passed &= expectMapping("identity", 5, 5, {0, 1, 2, 3, 4});
    passed &= expectMapping("upscale", 3, 5, {0, 0, 1, 2, 2});
    passed &= expectMapping("downscale", 5, 3, {0, 2, 4});
    passed &= expectMapping("rounded target extent", 4, 3, {0, 1, 3});
    passed &= expectMapping("strong downscale", 10, 2, {2, 7});
    passed &= expectValue(
            "rounded 100-to-110 mapping",
            slice_viewer_geometry::sourcePixelForPaintedPixel(33, 100, 110),
            30);

    passed &= expectValue(
            "negative target position clamps",
            slice_viewer_geometry::sourcePixelForPaintedPixel(-10, 5, 3),
            0);
    passed &= expectValue(
            "position past target clamps",
            slice_viewer_geometry::sourcePixelForPaintedPixel(10, 5, 3),
            4);
    passed &= expectValue(
            "empty source is safe",
            slice_viewer_geometry::sourcePixelForPaintedPixel(0, 0, 3),
            0);
    passed &= expectValue(
            "empty target is safe",
            slice_viewer_geometry::sourcePixelForPaintedPixel(0, 5, 0),
            0);
    passed &= expectValue(
            "64-bit intermediate",
            slice_viewer_geometry::sourcePixelForPaintedPixel(INT_MAX - 1, INT_MAX, INT_MAX),
            INT_MAX - 1);

    if (!passed) {
        return 1;
    }

    std::cout << "SliceViewer coordinate mapping tests passed\n";
    return 0;
}
