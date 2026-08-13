#ifndef SEGMENTPUZZLER_VOXELSPACING_H
#define SEGMENTPUZZLER_VOXELSPACING_H

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace voxel_geometry {

inline constexpr double kSpacingRelativeTolerance = 1e-4;
inline constexpr double kSpacingAbsoluteTolerance = 1e-4;

struct VoxelSpacing {
    double x = 1.0;
    double y = 1.0;
    double z = 1.0;

    double operator[](unsigned int axis) const {
        switch (axis) {
            case 0: return x;
            case 1: return y;
            case 2: return z;
            default: throw std::out_of_range("Voxel spacing axis must be 0, 1, or 2");
        }
    }
};

struct PlaneScale {
    double horizontal = 1.0;
    double vertical = 1.0;
};

struct PlaneAxes {
    unsigned int horizontal;
    unsigned int vertical;
};

inline bool isValid(const VoxelSpacing &spacing) noexcept {
    return std::isfinite(spacing.x) && spacing.x > 0.0
        && std::isfinite(spacing.y) && spacing.y > 0.0
        && std::isfinite(spacing.z) && spacing.z > 0.0;
}

inline bool nearlyEqual(double left,
                        double right,
                        double relativeTolerance = kSpacingRelativeTolerance,
                        double absoluteTolerance = kSpacingAbsoluteTolerance) noexcept {
    const double difference = std::abs(left - right);
    const double scale = std::max(std::abs(left), std::abs(right));
    return difference <= std::max(absoluteTolerance, relativeTolerance * scale);
}

inline bool nearlyEqual(const VoxelSpacing &left,
                        const VoxelSpacing &right,
                        double relativeTolerance = kSpacingRelativeTolerance,
                        double absoluteTolerance = kSpacingAbsoluteTolerance) noexcept {
    return nearlyEqual(left.x, right.x, relativeTolerance, absoluteTolerance)
        && nearlyEqual(left.y, right.y, relativeTolerance, absoluteTolerance)
        && nearlyEqual(left.z, right.z, relativeTolerance, absoluteTolerance);
}

template<typename SpacingType>
VoxelSpacing fromItkSpacing(const SpacingType &spacing) {
    return {static_cast<double>(spacing[0]),
            static_cast<double>(spacing[1]),
            static_cast<double>(spacing[2])};
}

template<typename SpacingType>
void assignToItkSpacing(const VoxelSpacing &source, SpacingType &target) {
    target[0] = source.x;
    target[1] = source.y;
    target[2] = source.z;
}

inline double normalizedAxisScale(const VoxelSpacing &spacing, unsigned int imageAxis) {
    if (!isValid(spacing)) {
        return 1.0;
    }
    return spacing[imageAxis] / spacing.x;
}

inline PlaneAxes planeAxes(unsigned int sliceAxis) {
    switch (sliceAxis) {
        case 0: return {2, 1}; // YZ view
        case 1: return {0, 2}; // XZ view
        case 2: return {0, 1}; // XY view
        default: throw std::out_of_range("Slice axis must be 0, 1, or 2");
    }
}

inline PlaneScale planeScale(const VoxelSpacing &spacing, unsigned int sliceAxis) {
    const auto axes = planeAxes(sliceAxis);
    return {normalizedAxisScale(spacing, axes.horizontal),
            normalizedAxisScale(spacing, axes.vertical)};
}

inline double sliceWidthInNormalizedUnits(int sourceWidth,
                                          const VoxelSpacing &spacing,
                                          unsigned int sliceAxis) {
    return static_cast<double>(std::max(0, sourceWidth))
        * planeScale(spacing, sliceAxis).horizontal;
}

inline double sliceHeightInNormalizedUnits(int sourceHeight,
                                           const VoxelSpacing &spacing,
                                           unsigned int sliceAxis) {
    return static_cast<double>(std::max(0, sourceHeight))
        * planeScale(spacing, sliceAxis).vertical;
}

} // namespace voxel_geometry

#endif // SEGMENTPUZZLER_VOXELSPACING_H
