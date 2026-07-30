#include "src/viewers/SliceViewerZoomPolicy.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

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

    const double fitted2D =
            slice_viewer_zoom::fittedZoomForViewport(288, 174, 256, 128);
    passed &= expectNear("2D viewport fit", fitted2D, 1.125);
    passed &= expectNear("small 2D starts at native scale",
                         slice_viewer_zoom::initialZoom(fitted2D, true, 256),
                         1.0);
    passed &= expectNear("large 2D starts fitted",
                         slice_viewer_zoom::initialZoom(0.4, true, 2048),
                         0.4);
    passed &= expectNear("3D startup policy remains unchanged",
                         slice_viewer_zoom::initialZoom(0.4, false, 2048),
                         1.0);

    const auto largeImageLimits =
            slice_viewer_zoom::limitsForMaximumSliceExtent(2048);
    passed &= expectNear("large image minimum",
                         largeImageLimits.minimum,
                         16.0 / 2048.0);
    passed &= expectNear("large image maximum",
                         largeImageLimits.maximum,
                         8192.0 / 2048.0);

    const auto smallImageLimits =
            slice_viewer_zoom::limitsForMaximumSliceExtent(8);
    passed &= expectNear("small image minimum", smallImageLimits.minimum, 1.0);
    passed &= expectNear("small image maximum", smallImageLimits.maximum, 1024.0);

    const auto hugeImageLimits =
            slice_viewer_zoom::limitsForMaximumSliceExtent(16384);
    const double hugeImageInitial =
            slice_viewer_zoom::initialZoom(0.1, false, 16384);
    passed &= expectNear("huge image initial respects maximum",
                         hugeImageInitial,
                         hugeImageLimits.maximum);
    if (std::clamp(hugeImageInitial * 2.0,
                   hugeImageLimits.minimum,
                   hugeImageLimits.maximum) < hugeImageInitial) {
        std::cerr << "zooming in must not reduce the zoom\n";
        passed = false;
    }

    if (!passed) {
        return 1;
    }

    std::cout << "SliceViewer zoom policy tests passed\n";
    return 0;
}
