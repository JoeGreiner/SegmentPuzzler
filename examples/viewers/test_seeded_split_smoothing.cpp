#include "src/viewers/Segment3DViewerDialog.h"
#include "src/qtUtils/TaskRunner.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
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
        auto image = dataType::SegmentsImageType::New();
        image->SetRegions(dataType::SegmentsImageType::RegionType({{0, 0, 0}}, {{32, 24, 16}}));
        image->Allocate();
        image->FillBuffer(1);
        auto baseSession = segment_puzzler::prepareSeededWatershedSplit(
            image, 1, segment_puzzler::SplitLandscapePreparation::Deferred);
        const auto scene = Segment3DViewerDialog::prepareScene(image, {{1, 0xFFAAAAAA}}, baseSession.sourceRoi);
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
