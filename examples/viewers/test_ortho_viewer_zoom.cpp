#include "src/viewers/OrthoViewer.h"
#include "src/viewers/SliceViewerZoomPolicy.h"
#include "src/viewers/itkSignal.h"
#include "src/qtUtils/TaskRunner.h"

#include <QApplication>
#include <QMainWindow>
#include <QScrollBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <memory>

namespace {

class RenderComparisonSignal : public itkSignal<unsigned short> {
public:
    using itkSignal::itkSignal;
    bool renderVisible = true;
    int visibleCalls = 0;
    std::atomic<int> volumeReads{0};
    bool supportsVisibleLabelRendering() const override {
        return renderVisible && itkSignal::supportsVisibleLabelRendering();
    }
    QImage calculateVisibleLabelImage(unsigned int index, unsigned int axis,
                                     const std::vector<int> &columns, const std::vector<int> &rows) override {
        ++visibleCalls;
        ++volumeReads;
        return itkSignal::calculateVisibleLabelImage(index, axis, columns, rows);
    }
    QImage calculateSliceQImage(unsigned int index, unsigned int axis,
                                std::vector<quint32> *buffer, bool reuseRawSlice = false) override {
        ++volumeReads;
        return itkSignal::calculateSliceQImage(index, axis, buffer, reuseRawSlice);
    }
    QString getNumberOfXYZAsString(int x, int y, int z) override {
        ++volumeReads;
        return itkSignal::getNumberOfXYZAsString(x, y, z);
    }
};

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

bool testRenderingDuringTasks() {
    auto image = itk::Image<unsigned short, 3>::New();
    image->SetRegions(itk::Size<3>{{1024, 769, 3}});
    image->Allocate();
    for (std::size_t i = 0; i < image->GetLargestPossibleRegion().GetNumberOfPixels(); ++i)
        image->GetBufferPointer()[i] = static_cast<unsigned short>(i / 37 % 7 + 1);
    RenderComparisonSignal signal(image, false);
    signal.setCategoricalColorMode();
    signal.setAlpha(255);
    TaskRunner runner;
    OrthoViewer viewer(std::make_shared<GraphBase>(), &runner);
    viewer.resize(600, 500);
    viewer.addSignal(&signal);
    viewer.xy->setZoom(2);
    processDeferredLayout();
    viewer.scrollAreaXY->horizontalScrollBar()->setValue(31);
    viewer.scrollAreaXY->verticalScrollBar()->setValue(19);
    processDeferredLayout();
    const auto captureSlice = [](SliceViewer *slice) {
        return slice->grab(slice->visibleRegion().boundingRect()).toImage();
    };
    bool passed = true;
    for (int scenario = 0; scenario < 3; ++scenario) {
        const bool readOnly = scenario == 1;
        const bool fail = scenario == 2;
        std::vector<QImage> previousFrames;
        for (auto *slice : viewer.viewerList) previousFrames.push_back(captureSlice(slice));
        std::promise<void> release;
        const auto released = release.get_future().share();
        bool committed = false, timedOut = false;
        QEventLoop finished;
        QObject::connect(&runner, &TaskRunner::busyChanged, &finished, [&](bool busy) {
            if (!busy) finished.quit();
        });
        QTimer closeError;
        QObject::connect(&closeError, &QTimer::timeout, [&] {
            if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) dialog->accept();
        });
        if (fail) closeError.start(10);
        const auto compute = [&, released] {
            released.wait();
            if (fail) throw std::runtime_error("Expected viewer test failure");
            if (!readOnly) image->FillBuffer(2);
        };
        const auto commit = [&] {
            committed = true;
            if (!readOnly) viewer.refreshViewers();
        };
        if (scenario == 0) runner.runWithLabel("Viewer rendering test", compute, commit);
        else runner.runInBackground("Viewer rendering test", compute, commit, {}, readOnly);
        const int reads = signal.volumeReads;
        processDeferredLayout();
        if (!readOnly) {
            for (std::size_t i = 0; i < viewer.viewerList.size(); ++i) {
                if (captureSlice(viewer.viewerList[i]) != previousFrames[i]) {
                    std::cerr << "Viewer lost its last frame during task " << scenario << " on view " << i << '\n';
                    passed = false;
                }
            }
        }
        viewer.resize(650 + scenario * 50, 550 + scenario * 50);
        processDeferredLayout();
        viewer.scrollAreaXY->viewport()->grab();
        if (!readOnly) {
            for (std::size_t i = 0; i < viewer.viewerList.size(); ++i) {
                const auto actual = captureSlice(viewer.viewerList[i]);
                const QRect overlap(QPoint(), previousFrames[i].size().boundedTo(actual.size()));
                if (overlap.isEmpty() || actual.copy(overlap) != previousFrames[i].copy(overlap)) {
                    std::cerr << "Viewer lost its frozen frame after resizing on view " << i << '\n';
                    passed = false;
                }
            }
        }
        const QPoint point(30, 40);
        QMouseEvent move(QEvent::MouseMove, point, viewer.xy->mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.xy, &move);
        if (!viewer.updatesEnabled() || viewer.isEnabled() != readOnly ||
            (readOnly ? signal.volumeReads == reads : signal.volumeReads != reads)) {
            std::cerr << "Incorrect volume access or viewer state during task " << scenario << '\n';
            passed = false;
        }
        release.set_value();
        QTimer::singleShot(10000, &finished, [&] { timedOut = true; finished.quit(); });
        finished.exec();
        processDeferredLayout();
        if (timedOut || runner.isBusy() || !viewer.updatesEnabled() || !viewer.isEnabled() || committed == fail) {
            std::cerr << "Viewer did not resume after task " << scenario << '\n';
            passed = false;
        }
        const auto actual = viewer.scrollAreaXY->viewport()->grab().toImage();
        signal.renderVisible = false;
        viewer.refreshViewers();
        processDeferredLayout();
        if (actual != viewer.scrollAreaXY->viewport()->grab().toImage()) {
            std::cerr << "Viewer did not show committed pixels after task " << scenario << '\n';
            passed = false;
        }
        signal.renderVisible = true;
        viewer.refreshViewers();
        processDeferredLayout();
    }
    return passed;
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
    RenderComparisonSignal signal(image, false);
    signal.setCategoricalColorMode();

    QMainWindow window;
    auto *orthoViewer = new OrthoViewer(graphBase, nullptr);
    window.setCentralWidget(orthoViewer);
    window.resize(468, 722);
    window.show();
    processDeferredLayout();

    orthoViewer->addSignal(&signal);
    orthoViewer->setViewToMiddleOfStack();
    processDeferredLayout();

    bool passed = testRenderingDuringTasks();
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

    for (std::size_t i = 0; i < region.GetNumberOfPixels(); ++i)
        image->GetBufferPointer()[i] = static_cast<unsigned short>(i * 7919);
    signal.setAlpha(150);
    for (double zoom : {0.37, 2.0, 8.0}) {
        orthoViewer->xy->setZoom(zoom);
        processDeferredLayout();
        orthoViewer->scrollAreaXY->horizontalScrollBar()->setValue(31);
        orthoViewer->scrollAreaXY->verticalScrollBar()->setValue(19);
        signal.renderVisible = false;
        orthoViewer->refreshViewers(&signal);
        processDeferredLayout();
        const auto reference = orthoViewer->scrollAreaXY->viewport()->grab().toImage();
        signal.renderVisible = true;
        orthoViewer->refreshViewers(&signal);
        processDeferredLayout();
        if (orthoViewer->scrollAreaXY->viewport()->grab().toImage() != reference) {
            std::cerr << "Visible widget rendering differs at zoom " << zoom << '\n';
            passed = false;
        }
    }
    if (signal.visibleCalls == 0) {
        std::cerr << "Widget did not use visible label rendering\n";
        passed = false;
    }

    if (!passed) {
        return 1;
    }

    std::cout << "OrthoViewer zoom tests passed\n";
    return 0;
}
