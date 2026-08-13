#include "src/viewers/OrthoViewer.h"
#include "src/viewers/SliceViewerZoomPolicy.h"
#include "src/viewers/itkSignal.h"

#include <QApplication>
#include <QMainWindow>

#include <cmath>
#include <iostream>
#include <memory>

namespace {

bool expectNear(const char *name, double actual, double expected) {
    if (std::abs(actual - expected) <= 1e-9) {
        return true;
    }
    std::cerr << name << ": got " << actual << ", expected " << expected << '\n';
    return false;
}

void processDeferredLayout() {
    QApplication::sendPostedEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents();
    QApplication::processEvents();
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);

    using Image = itk::Image<unsigned short, 3>;
    auto image = Image::New();
    Image::SizeType size = {{256, 128, 1}};
    Image::RegionType region;
    region.SetSize(size);
    image->SetRegions(region);
    Image::SpacingType fileSpacing;
    fileSpacing[0] = 0.25;
    fileSpacing[1] = 0.25;
    fileSpacing[2] = 1.0;
    image->SetSpacing(fileSpacing);
    image->Allocate();
    image->FillBuffer(1);

    auto graphBase = std::make_shared<GraphBase>();
    itkSignal<unsigned short> signal(image, false);
    signal.setLUTCategorical();

    QMainWindow window;
    auto *orthoViewer = new OrthoViewer(graphBase, nullptr);
    window.setCentralWidget(orthoViewer);
    window.resize(468, 722);
    window.show();
    processDeferredLayout();

    orthoViewer->addSignal(&signal);
    orthoViewer->setViewToMiddleOfStack();
    processDeferredLayout();

    bool passed = true;
    const auto loadedSpacing = orthoViewer->getVoxelSpacing();
    passed &= expectNear("loaded X spacing", loadedSpacing.x, 0.25);
    passed &= expectNear("loaded Y spacing", loadedSpacing.y, 0.25);
    passed &= expectNear("loaded Z spacing", loadedSpacing.z, 1.0);
    const double initialZoom = orthoViewer->xy->zoomFactor;
    passed &= expectNear("linked XZ zoom", orthoViewer->xz->zoomFactor, initialZoom);
    passed &= expectNear("linked YZ zoom", orthoViewer->zy->zoomFactor, initialZoom);
    passed &= expectNear("2D initial zoom", initialZoom, 1.0);
    if (orthoViewer->xy->width() > orthoViewer->scrollAreaXY->viewport()->width() ||
        orthoViewer->xy->height() > orthoViewer->scrollAreaXY->viewport()->height()) {
        std::cerr << "2D initial image does not fit the XY viewport\n";
        passed = false;
    }

    orthoViewer->setVoxelSpacing({1.0, 1.0, 4.0});
    processDeferredLayout();
    if (orthoViewer->xy->width() != 256 || orthoViewer->xy->height() != 128) {
        std::cerr << "XY view changed size for X/Y-isotropic spacing\n";
        passed = false;
    }
    if (orthoViewer->xz->width() != 256 || orthoViewer->xz->height() != 4) {
        std::cerr << "XZ view did not apply the fourfold Z spacing\n";
        passed = false;
    }
    if (orthoViewer->zy->width() != 4 || orthoViewer->zy->height() != 128) {
        std::cerr << "YZ view did not apply the fourfold Z spacing\n";
        passed = false;
    }

    const auto limits = slice_viewer_zoom::limitsForMaximumSliceExtent(256);
    orthoViewer->xy->modifyZoomInAllViewers(1e-9);
    passed &= expectNear("minimum XY zoom", orthoViewer->xy->zoomFactor, limits.minimum);
    passed &= expectNear("minimum XZ zoom", orthoViewer->xz->zoomFactor, limits.minimum);
    passed &= expectNear("minimum YZ zoom", orthoViewer->zy->zoomFactor, limits.minimum);

    orthoViewer->xy->modifyZoomInAllViewers(0.5);
    passed &= expectNear("minimum zoom is stable", orthoViewer->xy->zoomFactor, limits.minimum);

    if (!passed) {
        return 1;
    }

    std::cout << "OrthoViewer zoom tests passed\n";
    return 0;
}
