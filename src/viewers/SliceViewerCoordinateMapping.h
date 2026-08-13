#ifndef SEGMENTPUZZLER_SLICEVIEWERCOORDINATEMAPPING_H
#define SEGMENTPUZZLER_SLICEVIEWERCOORDINATEMAPPING_H

#include <algorithm>
#include <cstdint>

namespace slice_viewer_geometry {

// Matches QPainter's nearest-neighbour mapping from a target pixel center to
// the source image pixel when drawImage scales one integer QRect to another.
inline int sourcePixelForPaintedPixel(int position, int sourceExtent, int targetExtent) noexcept {
    if (sourceExtent <= 0 || targetExtent <= 0) {
        return 0;
    }

    const auto position64 = static_cast<std::int64_t>(position);
    const auto sourceExtent64 = static_cast<std::int64_t>(sourceExtent);
    const auto targetExtent64 = static_cast<std::int64_t>(targetExtent);
    const auto sourcePosition =
            (((2 * position64 + 1) * sourceExtent64) - 1) / (2 * targetExtent64);

    return static_cast<int>(std::clamp<std::int64_t>(sourcePosition, 0, sourceExtent64 - 1));
}

inline double paintedPositionForSourcePixelCenter(double sourcePosition,
                                                  int sourceExtent,
                                                  int targetExtent) noexcept {
    if (sourceExtent <= 0 || targetExtent <= 0) {
        return 0.0;
    }
    const double clampedPosition = std::clamp(
        sourcePosition, 0.0, static_cast<double>(sourceExtent - 1));
    return (clampedPosition + 0.5)
        * static_cast<double>(targetExtent)
        / static_cast<double>(sourceExtent);
}

inline int paintedBoundaryForSourceBoundary(int sourceBoundary,
                                            int sourceExtent,
                                            int targetExtent) noexcept {
    if (sourceExtent <= 0 || targetExtent <= 0) {
        return 0;
    }
    const auto clampedBoundary = std::clamp<std::int64_t>(
        sourceBoundary, 0, static_cast<std::int64_t>(sourceExtent));
    const auto numerator = clampedBoundary * static_cast<std::int64_t>(targetExtent);
    return static_cast<int>(numerator / static_cast<std::int64_t>(sourceExtent));
}

} // namespace slice_viewer_geometry

#endif // SEGMENTPUZZLER_SLICEVIEWERCOORDINATEMAPPING_H
