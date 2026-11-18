#include "src/image_storage/DenseImageStorage.h"
#include "src/image_storage/StreamVByteImageStorage.h"
#include "src/segment_handling/Graph.h"
#include "src/viewers/AnnotationSliceViewer.h"
#include "src/viewers/Segment3DViewerDialog.h"
#include "src/qtUtils/TaskRunner.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSurfaceFormat>
#include <QThreadPool>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <itkMultiThreaderBase.h>

#include <future>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitUntil(const std::function<bool()> &ready) {
    QElapsedTimer timeout;
    timeout.start();
    while (!ready() && timeout.elapsed() < 10000) {
        QEventLoop turn;
        QTimer::singleShot(5, &turn, &QEventLoop::quit);
        turn.exec();
    }
    return ready();
}

// Keep the smoothing worker queued while testing user actions deterministically.
struct WorkerGate {
    std::promise<void> release;
    QFuture<void> future = QtConcurrent::run([wait = release.get_future().share()]() { wait.wait(); });
    bool released = false;

    void open() {
        if (!released) {
            released = true;
            release.set_value();
        }
    }
    ~WorkerGate() {
        open();
        future.waitForFinished();
    }
};

using Session = segment_puzzler::SeededWatershedSplitSession;

void testSplitPreviewAfterMerge() {
    using Label = dataType::SegmentIdType;
    auto image = dataType::SegmentsImageType::New();
    image->SetRegions(dataType::SegmentsImageType::SizeType{{15, 7, 7}});
    image->Allocate(true);
    for (int z = 1; z <= 5; ++z) for (int y = 1; y <= 5; ++y) for (int x = 1; x <= 13; ++x)
        if (x <= 5 || x >= 9 || (y == 3 && z == 3)) image->SetPixel({x, y, z}, 1);
    image->SetPixel({14, 3, 3}, 2);
    auto base = std::make_shared<GraphBase>();
    base->pSelectedSegmentation = segment_puzzler::StreamVByteImageStorage<Label>::fromImage(*image);
    base->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<Label>::fromImage(image);
    Graph graph(base, false);
    base->pGraph = &graph;
    graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
    graph.constructFromVolume(base->pWorkingSegmentsImage);
    AnnotationSliceViewer viewer(base, nullptr);
    require(viewer.show3DSplitViewForLabel(1), "Could not open the split viewer");
    auto *dialog = viewer.findChild<Segment3DViewerDialog *>();
    require(dialog, "Missing split dialog");
    auto *size = dialog->findChild<QSpinBox *>("minimumErosionMarkerSizeSpinBox");
    auto *erosion = dialog->findChild<QPushButton *>("erosionSplitButton");
    QPushButton *apply = nullptr;
    for (auto *button : dialog->findChildren<QPushButton *>())
        if (button->text() == "Apply Split") apply = button;
    require(size && erosion && apply && waitUntil([&] { return erosion->isEnabled(); }), "Split tools did not become ready");
    size->setValue(1);
    erosion->click();
    require(apply->isEnabled(), "Expected a valid two-part preview");

    const auto owner = *graph.mergeEdges({graph.initialLabelPairToTwoSidedInitialEdge.at({1, 2})->numId}).begin();
    const auto next = graph.nextFreeId;
    const auto workingTime = base->pWorkingSegmentsImage->GetMTime();
    const auto selectedTime = base->pSelectedSegmentation->GetMTime();
    const auto edgeTime = base->pEdgesInitialSegmentsImage->GetMTime();
    bool askedToReopen = false;
    QTimer::singleShot(0, dialog, [&] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            askedToReopen = box->text().contains("reopen", Qt::CaseInsensitive);
            box->accept();
        }
    });
    apply->click();
    require(askedToReopen && graph.nextFreeId == next && graph.workingNodes.size() == 1 &&
            graph.workingNodes.at(owner)->subInitialNodes.size() == 2 &&
            base->pWorkingSegmentsImage->GetMTime() == workingTime && base->pSelectedSegmentation->GetMTime() == selectedTime &&
            base->pEdgesInitialSegmentsImage->GetMTime() == edgeTime, "Applying a stale preview changed the merged graph or images");
}

std::unique_ptr<Segment3DViewerDialog> makeDialog(const std::shared_ptr<Session> &session,
                                               const Segment3DViewerDialog::PreparedScene &scene) {
    Segment3DViewerDialog::SplitSessionConfig config;
    config.session = session;
    config.applySplit = [](const auto &) { return Segment3DViewerDialog::SplitApplyResult{}; };
    config.applyErosionSplit = [](const auto &) { return Segment3DViewerDialog::SplitApplyResult{}; };
    return std::make_unique<Segment3DViewerDialog>(scene, config);
}

} // namespace

int main(int argc, char **argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(2);
    try {
        if (app.arguments().contains("--stale-preview")) {
            testSplitPreviewAfterMerge();
            return 0;
        }
        auto image = dataType::SegmentsImageType::New();
        image->SetRegions(dataType::SegmentsImageType::RegionType({{0, 0, 0}}, {{32, 24, 16}}));
        image->Allocate();
        image->FillBuffer(1);
        auto baseSession = segment_puzzler::prepareSeededWatershedSplit(
            image, 1, segment_puzzler::SplitLandscapePreparation::Deferred);
        const auto scene = Segment3DViewerDialog::prepareScene(segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(image), {{1, 0xFFAAAAAA}}, baseSession.sourceRoi);
        auto reference = baseSession;
        segment_puzzler::updateSeededWatershedSplitLandscape(reference, 2.0);
        auto session = std::make_shared<Session>(baseSession);
        auto dialog = makeDialog(session, scene);
        auto *runner = dialog->findChild<TaskRunner *>();
        auto *slider = dialog->findChild<QSlider *>(QStringLiteral("smoothingSlider"));
        auto *erosion = dialog->findChild<QPushButton *>(QStringLiteral("erosionSplitButton"));
        require(runner && slider && erosion, "Missing smoothing controls");
        require(!runner->isBusy(), "Smoothing started before the first frame");
        {
            WorkerGate gate;
            dialog->show();
            require(waitUntil([&] { return runner->isBusy(); }), "Smoothing did not start after showing the dialog");
            require(session->landscape.IsNull(), "Pending smoothing changed the shared session");
            require(!erosion->isEnabled(), "Erosion was enabled before the landscape was ready");
            int enabledSeedButtons = 0;
            for (auto *button : dialog->findChildren<QPushButton *>()) {
                if (button->text().contains(QStringLiteral(" Seed")) && button->isEnabled()) {
                    ++enabledSeedButtons;
                }
            }
            require(enabledSeedButtons >= 2, "Seed placement was disabled during smoothing");
            bool responded = false;
            QTimer::singleShot(0, dialog.get(), [&] { responded = true; });
            require(waitUntil([&] { return responded; }), "The UI did not respond during smoothing");
            gate.open();
            require(waitUntil([&] { return !runner->isBusy() && erosion->isEnabled(); }), "Initial smoothing never completed");
            require(session->landscapeHash == reference.landscapeHash, "Initial background landscape differs from synchronous preparation");
        }
        {
            WorkerGate gate;
            slider->setValue(10);
            require(waitUntil([&] { return runner->isBusy(); }), "Slider smoothing did not start");
            const auto originalHash = session->landscapeHash;
            slider->setValue(15);
            bool discardedStale = false;
            auto connection = QObject::connect(runner, &TaskRunner::busyChanged, dialog.get(), [&](bool busy) {
                if (!busy && session->landscapeSmoothingSigmaPixels == 2.0) {
                    discardedStale = session->landscapeHash == originalHash && !erosion->isEnabled();
                }
            });
            gate.open();
            require(waitUntil([&] { return !runner->isBusy() && session->landscapeSmoothingSigmaPixels == 1.5; }),
                    "The latest requested smoothing was not computed");
            QObject::disconnect(connection);
            require(discardedStale, "An obsolete smoothing result was applied");
            segment_puzzler::updateSeededWatershedSplitLandscape(reference, 1.5);
            require(session->landscapeHash == reference.landscapeHash, "Slider landscape differs from synchronous smoothing");
        }
        {
            WorkerGate gate;
            const auto originalHash = session->landscapeHash;
            slider->setValue(5);
            require(waitUntil([&] { return runner->isBusy(); }), "Smoothing did not start before slider revert");
            slider->setValue(15);
            gate.open();
            require(waitUntil([&] { return !runner->isBusy() && erosion->isEnabled(); }), "Reverting smoothing left the tools disabled");
            require(session->landscapeHash == originalHash, "Reverting the slider replaced the correct landscape");
        }
        dialog.reset();
        {
            WorkerGate gate;
            auto closingSession = std::make_shared<Session>(baseSession);
            auto closingDialog = makeDialog(closingSession, scene);
            auto *closingRunner = closingDialog->findChild<TaskRunner *>();
            closingDialog->show();
            require(waitUntil([&] { return closingRunner->isBusy(); }), "Close test did not queue smoothing");
            QPointer<Segment3DViewerDialog> guardedDialog(closingDialog.get());
            closingDialog.reset();
            require(guardedDialog.isNull(), "The dialog was not destroyed");
            gate.open();
            require(QThreadPool::globalInstance()->waitForDone(10000), "Smoothing did not finish after closing");
            QApplication::processEvents();
            require(closingSession->landscape.IsNull(), "A closed dialog committed smoothing");
        }
        std::cout << "PASS: first-frame deferral, UI readiness, matching landscapes, stale requests, slider revert and close during smoothing\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
