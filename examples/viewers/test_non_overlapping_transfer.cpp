#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>

#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "src/controllers/SignalControl.h"
#include "src/qtUtils/NonOverlappingTransferSettings.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/viewers/OrthoViewer.h"

namespace {
namespace transfer_settings = segment_puzzler::non_overlapping_transfer;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void saveScreenshot(QWidget *widget, const QString &name) {
    const QString directory = qEnvironmentVariable("SEGMENT_PUZZLER_TEST_SCREENSHOT_DIR");
    if (!directory.isEmpty()) {
        require(widget->grab().save(QDir(directory).filePath(name + ".png")), "Could not save screenshot");
    }
}

template<typename Open, typename Inspect>
void inspectDialog(Open open, Inspect inspect) {
    std::exception_ptr error;
    QTimer::singleShot(0, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        try {
            require(dialog != nullptr, "Expected a modal dialog");
            inspect(dialog);
        } catch (...) {
            error = std::current_exception();
            if (dialog) dialog->reject();
        }
    });
    open();
    if (error) std::rethrow_exception(error);
}

void waitForTask(TaskRunner &runner) {
    if (!runner.isBusy()) return;
    QEventLoop loop;
    QObject::connect(&runner, &TaskRunner::busyChanged, &loop, [&](bool busy) {
        if (!busy) loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    require(!runner.isBusy(), "Task did not finish");
}

void processEventsFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void key(QWidget *viewer, QEvent::Type type, int code, bool autoRepeat = false) {
    QKeyEvent event(type, code, Qt::NoModifier, {}, autoRepeat);
    QApplication::sendEvent(viewer, &event);
}

void click(AnnotationSliceViewer *viewer, int x, int y) {
    const QPointF center = viewer->widgetPositionForSlicePixel(x, y);
    const QPoint position(static_cast<int>(center.x()), static_cast<int>(center.y()));
    require(viewer->slicePixelFromWidgetPoint(position) == QPoint(x, y), "Test click must target the intended voxel");
    QMouseEvent press(QEvent::MouseButtonPress, position, position,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewer, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, position,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewer, &release);
}

void moveMouse(AnnotationSliceViewer *viewer, int x, int y) {
    const QPointF center = viewer->widgetPositionForSlicePixel(x, y);
    const QPoint position(static_cast<int>(center.x()), static_cast<int>(center.y()));
    QMouseEvent event(QEvent::MouseMove, position, position,
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewer, &event);
}

void testStoredSettings() {
    auto settings = transfer_settings::loadSettings();
    require(settings.maximumOverlapFraction == 0.0 && settings.preserveExistingSegmentation,
            "Missing settings must default to zero overlap and preservation");
    transfer_settings::saveSettings({0.375, false});
    settings = transfer_settings::loadSettings();
    require(settings.maximumOverlapFraction == 0.375 && !settings.preserveExistingSegmentation,
            "Both settings must survive a new QSettings instance");
    QSettings stored;
    for (const QVariant &invalid : {QVariant(-0.1), QVariant(1.1),
            QVariant(std::numeric_limits<double>::quiet_NaN()),
            QVariant(std::numeric_limits<double>::infinity()), QVariant("invalid")}) {
        stored.setValue(transfer_settings::kMaximumOverlapKey, invalid);
        stored.setValue(transfer_settings::kPreserveSegmentationKey, "invalid");
        settings = transfer_settings::loadSettings();
        require(settings.maximumOverlapFraction == 0.0 && settings.preserveExistingSegmentation,
                "Invalid persisted settings must fall back to defaults");
    }
    stored.clear();
}

void testDialogsAndShortcut(QApplication &app) {
    auto base = std::make_shared<GraphBase>();
    Graph graph(base, false);
    base->pGraph = &graph;
    graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
    TaskRunner runner;
    OrthoViewer viewer(base, &runner);
    SignalControl control(base, &viewer, &runner, nullptr, false);
    QMenu menu;
    control.populateSegmentationMenu(&menu);
    QAction *all = nullptr;
    QAction *volume = nullptr;
    for (auto *action : control.findChildren<QAction *>()) {
        if (action->text() == "Transfer Non-Overlapping Supervoxels…") all = action;
        if (action->text() == "Transfer Non-Overlapping Supervoxels by Volume…") volume = action;
    }
    require(all && volume && !all->isEnabled() && !volume->isEnabled(),
            "Bulk actions must exist and be disabled without images");

    for (bool save : {true, false}) {
        inspectDialog([&]() { control.showNonOverlappingTransferSettings(); }, [&](QDialog *dialog) {
            auto *overlap = dialog->findChild<QDoubleSpinBox *>();
            auto *preserve = dialog->findChild<QCheckBox *>();
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            require(dialog->windowTitle() == "Non-Overlapping Transfer Settings" &&
                    !dialog->findChild<QSpinBox *>() && buttons->button(QDialogButtonBox::Save),
                    "Settings must use the shared dialog without a volume field");
            require(overlap->value() == (save ? 0.0 : 50.0) && preserve->isChecked(),
                    "Settings dialog must read the saved defaults");
            require(overlap->minimum() == 0 && overlap->maximum() == 100 && overlap->decimals() == 1,
                    "Overlap input must accept 0–100% with one decimal");
            overlap->setValue(save ? 50.0 : 75.0);
            preserve->setChecked(save);
            if (save) saveScreenshot(dialog, "non-overlapping-settings");
            buttons->button(save ? QDialogButtonBox::Save : QDialogButtonBox::Cancel)->click();
        });
        const auto settings = transfer_settings::loadSettings();
        require(settings.maximumOverlapFraction == 0.5 && settings.preserveExistingSegmentation,
                "Save must persist defaults; Cancel must leave them unchanged");
    }

    auto working = dataType::SegmentsImageType::New();
    working->SetRegions(dataType::SegmentsImageType::RegionType({{0, 0, 0}}, {{5, 3, 1}}));
    working->Allocate(true);
    working->SetPixel({1, 1, 0}, 1);
    working->SetPixel({2, 1, 0}, 1);
    working->SetPixel({3, 1, 0}, 2);
    base->pWorkingSegmentsImage = working;
    graph.constructFromVolume(working);
    itkSignal<dataType::SegmentIdType> workingSignal(working);
    control.segmentsGraph = &workingSignal;
    auto selected = dataType::SegmentsImageType::New();
    selected->SetRegions(working->GetLargestPossibleRegion());
    selected->Allocate(true);
    selected->SetPixel({1, 1, 0}, 10);
    base->pSelectedSegmentation = selected;
    base->selectedSegmentationMaxSegmentId = 10;
    itkSignal<dataType::SegmentIdType> selectedSignal(selected);
    selectedSignal.setCategoricalColorMode();
    base->pSelectedSegmentationSignal = &selectedSignal;
    viewer.addSignal(&selectedSignal);
    viewer.setViewToMiddleOfStack();
    viewer.resize(1200, 800);
    QApplication::processEvents();
    runner.busyChanged(false);
    require(all->isEnabled() && volume->isEnabled(), "Bulk actions must be enabled with both images");

    const QImage beforeBulk = viewer.xy->signalList.front()->getAddressSliceQImage()->copy();
    for (int step = 0; step < 4; ++step) {
        const bool withVolume = step == 1 || step == 2;
        inspectDialog([&]() { (withVolume ? volume : all)->trigger(); }, [&](QDialog *dialog) {
            auto *overlap = dialog->findChild<QDoubleSpinBox *>();
            auto *preserve = dialog->findChild<QCheckBox *>();
            auto *volumeInput = dialog->findChild<QSpinBox *>();
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            require(overlap->value() == 50.0 && preserve->isChecked(),
                    "Bulk dialogs must reopen with shared defaults");
            require((volumeInput != nullptr) == withVolume &&
                    buttons->button(QDialogButtonBox::Ok)->text() == "Transfer", "Bulk dialog fields mismatch");
            if (volumeInput) {
                require(volumeInput->value() == 50000 && volumeInput->minimum() == 0 &&
                        volumeInput->maximum() == 1000000, "Volume must retain its independent defaults");
                volumeInput->setValue(2);
            }
            if (step < 2) saveScreenshot(dialog, withVolume ? "non-overlapping-volume" : "non-overlapping-all");
            overlap->setValue(100.0);
            preserve->setChecked(false);
            buttons->button(step == 1 || step == 3 ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
        });
        waitForTask(runner);
        require(all->isEnabled() && volume->isEnabled(), "Completed tasks must restore bulk actions");
        require(step == 3 || base->selectedSegmentationMaxSegmentId == 10,
                "Cancel and the exclusive volume boundary must leave labels unchanged and restore actions");
        const auto defaults = transfer_settings::loadSettings();
        require(defaults.maximumOverlapFraction == 0.5 && defaults.preserveExistingSegmentation,
                "Bulk overrides must not be saved as defaults");
    }
    require(selected->GetPixel({1, 1, 0}) == 11 && selected->GetPixel({2, 1, 0}) == 11 &&
            selected->GetPixel({3, 1, 0}) == 12 &&
            *viewer.xy->signalList.front()->getAddressSliceQImage() != beforeBulk,
            "Bulk transfer must apply dialog overrides and refresh the rendered segmentation");
    selected->FillBuffer(0);
    selected->SetPixel({1, 1, 0}, 10);
    selected->Modified();
    base->selectedSegmentationMaxSegmentId = 10;
    viewer.refreshViewers();

    QStatusBar statusBar;
    std::vector<std::pair<QString, int>> statusMessages;
    QObject::connect(&viewer, &OrthoViewer::sendStatusMessage, &statusBar,
                     [&](const QString &message, int timeoutMs) {
        statusMessages.emplace_back(message, timeoutMs);
        statusBar.showMessage(message, timeoutMs);
    });
    QWidget *eChip = nullptr;
    for (auto *widget : viewer.findChildren<QWidget *>()) {
        if (widget->toolTip().startsWith("Hold E and click")) eChip = widget;
    }
    require(eChip && eChip->isVisible(), "Bottom-right card must include an E shortcut with its tooltip");
    const auto inactiveChip = eChip->grab().toImage();
    key(viewer.xy, QEvent::KeyPress, Qt::Key_E);
    for (auto *slice : viewer.viewerList) {
        require(slice->activeTool == SliceViewer::ToolMode::NonOverlappingTransfer,
                "Holding E must activate the same tool in all linked viewers");
    }
    require(selected->GetPixel({2, 1, 0}) == 0 && eChip->grab().toImage() != inactiveChip,
            "E alone must highlight its card without transferring");
    const auto activeChip = eChip->grab().toImage();
    click(viewer.xy, 0, 1);
    require(statusMessages.empty() && eChip->grab().toImage() == activeChip,
            "Background clicks must remain silent and leave the normal E highlight");
    saveScreenshot(&viewer, "non-overlapping-hotkeys");
    transfer_settings::saveSettings({0.49, true});
    click(viewer.xy, 2, 1);
    require(selected->GetPixel({2, 1, 0}) == 0 && base->selectedSegmentationMaxSegmentId == 10,
            "Click must use the latest overlap setting and reject excess overlap");
    require(statusMessages.size() == 1 && statusMessages.back().first == "Not transferred: overlap exceeds 49.0%." &&
            statusMessages.back().second == 4000, "Overlap rejection must show the English limit for four seconds");
    const auto rejectedChip = eChip->grab().toImage();
    require(rejectedChip != activeChip && rejectedChip != inactiveChip,
            "Rejection must use a distinct E highlight");
    saveScreenshot(eChip->parentWidget(), "non-overlapping-rejected");
    moveMouse(viewer.xy, 2, 1);
    viewer.xz->setSliceIndex(viewer.xz->getSliceIndex());
    require(statusMessages.size() == 1 && statusBar.currentMessage() == statusMessages.back().first,
            "Coordinate updates must not immediately replace rejection feedback");
    const QString diagnostic = "Could not resolve the selected segment in the working graph.";
    viewer.zy->sendStatusMessage(diagnostic);
    require(statusMessages.back() == std::make_pair(diagnostic, 0) && statusBar.currentMessage() == diagnostic,
            "Other tools must be able to display diagnostics immediately after a transfer rejection");
    moveMouse(viewer.xy, 2, 1);
    require(statusBar.currentMessage().startsWith("x:"),
            "Replacing rejection feedback with an ordinary message must clear the obsolete coordinate hold");
    const auto extraStatusMessages = statusMessages.size() - 1;
    processEventsFor(450);
    click(viewer.xy, 2, 1);
    processEventsFor(450);
    require(eChip->grab().toImage() == rejectedChip && statusMessages.size() == 2 + extraStatusMessages,
            "Repeated rejections must restart the flash without an older timer clearing it");
    moveMouse(viewer.xy, 2, 1);
    require(statusBar.currentMessage() == "Not transferred: overlap exceeds 49.0%.",
            "A new timed rejection must protect its text from coordinate updates again");
    processEventsFor(350);
    require(eChip->grab().toImage() == activeChip, "Flash expiry must restore blue while E is held");
    transfer_settings::saveSettings({0.5, true});
    const QImage beforeClick = viewer.xy->signalList.front()->getAddressSliceQImage()->copy();
    click(viewer.xy, 2, 1);
    require(selected->GetPixel({1, 1, 0}) == 10 && selected->GetPixel({2, 1, 0}) == 11 &&
            selected->GetPixel({3, 1, 0}) == 0, "E+click must preserve occupied voxels and transfer only the clicked node");
    require(*viewer.xy->signalList.front()->getAddressSliceQImage() != beforeClick,
            "E+click must refresh the rendered segmentation");
    require(statusMessages.size() == 2 + extraStatusMessages, "Successful transfers must not emit rejection feedback");
    key(viewer.xy, QEvent::KeyRelease, Qt::Key_E, true);
    key(viewer.xy, QEvent::KeyPress, Qt::Key_E, true);
    click(viewer.xy, 3, 1);
    require(selected->GetPixel({3, 1, 0}) == 12 &&
            viewer.xy->activeTool == SliceViewer::ToolMode::NonOverlappingTransfer,
            "E must survive auto-repeat and permit repeated clicks including single voxels");
    click(viewer.xy, 3, 1);
    require(base->selectedSegmentationMaxSegmentId == 12, "Rejected repeat clicks must not consume labels");
    require(statusMessages.size() == 3 + extraStatusMessages && statusMessages.back().first == "Not transferred: no empty voxels." &&
            statusMessages.back().second == 4000, "Preserved full nodes must report the English no-empty-voxels reason");
    transfer_settings::saveSettings({1.0, false});
    click(viewer.xy, 2, 1);
    require(selected->GetPixel({1, 1, 0}) == 13 && selected->GetPixel({2, 1, 0}) == 13,
            "E+click must read the latest preserve setting and support full overwrite");
    key(viewer.xz, QEvent::KeyRelease, Qt::Key_E);
    require(viewer.xy->activeTool == SliceViewer::ToolMode::None && eChip->grab().toImage() == rejectedChip,
            "Releasing E must clear the tool while the brief rejection flash finishes");
    processEventsFor(800);
    require(viewer.xy->activeTool == SliceViewer::ToolMode::None && eChip->grab().toImage() == inactiveChip,
            "Releasing E in a linked viewer must clear the tool and its highlight");

    key(viewer.xy, QEvent::KeyPress, Qt::Key_A);
    require(viewer.xy->isOverlayOnlyMode() && viewer.xy->activeTool == SliceViewer::ToolMode::None,
            "A must retain its annotations-only behavior");
    key(viewer.xy, QEvent::KeyRelease, Qt::Key_A);
    require(!viewer.xy->isOverlayOnlyMode(), "Releasing A must restore the display");
    key(viewer.xy, QEvent::KeyPress, Qt::Key_S);
    click(viewer.xy, 2, 1);
    require(selected->GetPixel({1, 1, 0}) == 14 && selected->GetPixel({2, 1, 0}) == 14,
            "S must retain its unconditional full-node transfer");
    key(viewer.xy, QEvent::KeyRelease, Qt::Key_S);
    key(viewer.xy, QEvent::KeyPress, Qt::Key_E);
    app.applicationStateChanged(Qt::ApplicationInactive);
    require(viewer.xy->activeTool == SliceViewer::ToolMode::None,
            "Application deactivation must clear E like the other transient tools");

    std::promise<void> finish;
    auto finished = finish.get_future().share();
    runner.runInBackground("Busy input test", [finished]() { finished.wait(); }, []() {});
    key(viewer.xy, QEvent::KeyPress, Qt::Key_E);
    click(viewer.xy, 2, 1);
    const bool blocked = !all->isEnabled() && !volume->isEnabled() &&
        viewer.xy->activeTool == SliceViewer::ToolMode::None && base->selectedSegmentationMaxSegmentId == 14;
    finish.set_value();
    waitForTask(runner);
    require(blocked && all->isEnabled() && volume->isEnabled(),
            "Busy state must block shortcut and bulk actions, then restore availability");
    base->pSelectedSegmentation = nullptr;
    runner.busyChanged(false);
    key(viewer.xy, QEvent::KeyPress, Qt::Key_E);
    click(viewer.xy, 2, 1);
    require(!all->isEnabled() && !volume->isEnabled() && base->selectedSegmentationMaxSegmentId == 14,
            "Missing segmentation must disable bulk actions and make E+click a no-op");
    require(statusMessages.size() == 3 + extraStatusMessages, "Unavailable operations and other hotkeys must not emit rejection feedback");
    processEventsFor(4300);
    require(statusBar.currentMessage().isEmpty(), "Rejection status must expire automatically");
    moveMouse(viewer.xy, 2, 1);
    require(statusMessages.back().second == 0 && statusBar.currentMessage().startsWith("x:"),
            "Coordinate updates must resume after feedback expires");
    control.segmentsGraph = nullptr;
    base->pSelectedSegmentationSignal = nullptr;
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("SegmentPuzzlerTests");
    QCoreApplication::setApplicationName("NonOverlappingTransfer");
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    QFile style(":qdarkstyle/dark/style.qss");
    if (style.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(style.readAll()));
    try {
        testStoredSettings();
        testDialogsAndShortcut(app);
    } catch (const std::exception &error) {
        std::cerr << "Assertion failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Non-overlapping transfer settings, dialogs, shortcut and availability tests passed.\n";
    return 0;
}
