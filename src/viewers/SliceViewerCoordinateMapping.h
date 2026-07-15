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

} // namespace slice_viewer_geometry

#endif // SEGMENTPUZZLER_SLICEVIEWERCOORDINATEMAPPING_H
