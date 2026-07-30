#ifndef SEGMENTPUZZLER_SLICEVIEWERZOOMPOLICY_H
#define SEGMENTPUZZLER_SLICEVIEWERZOOMPOLICY_H

#include <algorithm>
#include <cmath>

namespace slice_viewer_zoom {

constexpr double kMinimumVisibleSliceExtent = 16.0;
constexpr double kMaximumVisibleSliceExtent = 8192.0;

struct Limits {
    double minimum;
    double maximum;
};

inline double fittedZoomForViewport(int viewportWidth,
                                    int viewportHeight,
                                    int imageWidth,
                                    int imageHeight) {
    if (viewportWidth <= 0 || viewportHeight <= 0 || imageWidth <= 0 || imageHeight <= 0) {
        return 1.0;
    }

    return std::min(static_cast<double>(viewportWidth) / imageWidth,
                    static_cast<double>(viewportHeight) / imageHeight);
}

inline Limits limitsForMaximumSliceExtent(int maximumSliceExtent) {
    const double extent = std::max(1, maximumSliceExtent);
    const double minimum = std::min(1.0, kMinimumVisibleSliceExtent / extent);
    const double maximum = std::max(minimum, kMaximumVisibleSliceExtent / extent);
    return {minimum, maximum};
}

inline double initialZoom(double fittedZoom,
                          bool isTwoDimensional,
                          int maximumSliceExtent) {
    const auto limits = limitsForMaximumSliceExtent(maximumSliceExtent);
    if (!std::isfinite(fittedZoom) || fittedZoom <= 0.0) {
        return std::clamp(1.0, limits.minimum, limits.maximum);
    }

    const double preferredZoom = isTwoDimensional ? std::min(1.0, fittedZoom)
                                                   : std::max(1.0, fittedZoom);
    return std::clamp(preferredZoom, limits.minimum, limits.maximum);
}

} // namespace slice_viewer_zoom

#endif // SEGMENTPUZZLER_SLICEVIEWERZOOMPOLICY_H
