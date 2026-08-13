#include "src/viewers/SliceViewerCoordinateMapping.h"
#include "src/viewers/VoxelSpacing.h"

#include <climits>
#include <cmath>
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

bool expectNear(const std::string &name, double actual, double expected) {
    if (std::abs(actual - expected) <= 1e-12) {
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

    passed &= expectNear(
            "source center to target",
            slice_viewer_geometry::paintedPositionForSourcePixelCenter(4.0, 10, 40),
            18.0);
    passed &= expectValue(
            "source boundary to target",
            slice_viewer_geometry::paintedBoundaryForSourceBoundary(5, 10, 40),
            20);

    const voxel_geometry::VoxelSpacing anisotropic{1.0, 1.0, 4.0};
    passed &= expectValue(
        "absolute spacing tolerance",
        voxel_geometry::nearlyEqual(1.0, 1.00005),
        1);
    passed &= expectValue(
        "absolute spacing difference",
        voxel_geometry::nearlyEqual(1.0, 1.0002),
        0);
    passed &= expectValue(
        "relative spacing tolerance",
        voxel_geometry::nearlyEqual(1000.0, 1000.05),
        1);
    passed &= expectValue(
        "relative spacing difference",
        voxel_geometry::nearlyEqual(1000.0, 1000.2),
        0);
    const auto yzAxes = voxel_geometry::planeAxes(0);
    const auto xzAxes = voxel_geometry::planeAxes(1);
    const auto xyAxes = voxel_geometry::planeAxes(2);
    passed &= expectValue("YZ horizontal axis", static_cast<int>(yzAxes.horizontal), 2);
    passed &= expectValue("YZ vertical axis", static_cast<int>(yzAxes.vertical), 1);
    passed &= expectValue("XZ horizontal axis", static_cast<int>(xzAxes.horizontal), 0);
    passed &= expectValue("XZ vertical axis", static_cast<int>(xzAxes.vertical), 2);
    passed &= expectValue("XY horizontal axis", static_cast<int>(xyAxes.horizontal), 0);
    passed &= expectValue("XY vertical axis", static_cast<int>(xyAxes.vertical), 1);
    const auto xyScale = voxel_geometry::planeScale(anisotropic, 2);
    const auto xzScale = voxel_geometry::planeScale(anisotropic, 1);
    const auto yzScale = voxel_geometry::planeScale(anisotropic, 0);
    passed &= expectNear("XY horizontal spacing scale", xyScale.horizontal, 1.0);
    passed &= expectNear("XY vertical spacing scale", xyScale.vertical, 1.0);
    passed &= expectNear("XZ horizontal spacing scale", xzScale.horizontal, 1.0);
    passed &= expectNear("XZ vertical spacing scale", xzScale.vertical, 4.0);
    passed &= expectNear("YZ horizontal spacing scale", yzScale.horizontal, 4.0);
    passed &= expectNear("YZ vertical spacing scale", yzScale.vertical, 1.0);
    passed &= expectNear(
            "XZ normalized width",
            voxel_geometry::sliceWidthInNormalizedUnits(20, anisotropic, 1),
            20.0);
    passed &= expectNear(
            "XZ normalized height",
            voxel_geometry::sliceHeightInNormalizedUnits(7, anisotropic, 1),
            28.0);

    if (!passed) {
        return 1;
    }

    std::cout << "SliceViewer coordinate mapping tests passed\n";
    return 0;
}
