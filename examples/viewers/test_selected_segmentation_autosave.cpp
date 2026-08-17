#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "src/qtUtils/ApplicationQuitEventFilter.h"
#include "src/qtUtils/SelectedSegmentationAutosave.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/itkSignal.h"

namespace {

bool check(bool condition, const std::string &message) {
    if (condition) {
        return true;
    }
    std::cerr << "Assertion failed: " << message << '\n';
    return false;
}

dataType::SegmentsImageType::Pointer makeSegmentation(dataType::SegmentIdType value) {
    auto image = dataType::SegmentsImageType::New();
    dataType::SegmentsImageType::IndexType start{};
    start.Fill(0);
    dataType::SegmentsImageType::SizeType size{{2, 2, 2}};
    image->SetRegions(dataType::SegmentsImageType::RegionType(start, size));
    image->Allocate();
    image->FillBuffer(value);
    return image;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 10000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(2);
    }
    QApplication::processEvents();
    return condition();
}

bool waitForRecovery(SelectedSegmentationAutosave &autosave,
                     TaskRunner &taskRunner) {
    return waitUntil([&]() {
        const QString recoveryPath = autosave.selectedSegmentationRecoveryPath();
        return !taskRunner.isBusy()
            && !autosave.isWriteInProgress()
            && !recoveryPath.isEmpty()
            && QFile::exists(recoveryPath);
    });
}

void selectSegmentation(const std::shared_ptr<GraphBase> &graphBase,
                        const dataType::SegmentsImageType::Pointer &image,
                        itkSignal<dataType::SegmentIdType> *signal,
                        SelectedSegmentationAutosave &autosave) {
    graphBase->pSelectedSegmentation = image;
    graphBase->pSelectedSegmentationSignal = signal;
    autosave.selectedSegmentationChanged();
}

class QuitEventReceiver final : public QObject {
public:
    bool receivedQuit = false;

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::Quit) {
            receivedQuit = true;
        }
        return QObject::event(event);
    }
};

bool testApplicationQuitEventFilter() {
    QuitEventReceiver receiver;
    bool approveQuit = false;
    int requestCount = 0;
    ApplicationQuitEventFilter filter([&]() {
        ++requestCount;
        return approveQuit;
    });
    receiver.installEventFilter(&filter);

    QEvent unrelatedEvent(QEvent::User);
    QCoreApplication::sendEvent(&receiver, &unrelatedEvent);
    if (!check(requestCount == 0, "the quit filter handled an unrelated event")) {
        return false;
    }

    QEvent rejectedQuit(QEvent::Quit);
    QCoreApplication::sendEvent(&receiver, &rejectedQuit);
    if (!check(requestCount == 1 && !receiver.receivedQuit && !rejectedQuit.isAccepted(),
               "a rejected quit event reached its receiver")) {
        return false;
    }

    approveQuit = true;
    QEvent approvedQuit(QEvent::Quit);
    QCoreApplication::sendEvent(&receiver, &approvedQuit);
    return check(requestCount == 2 && receiver.receivedQuit,
                 "an approved quit event did not reach its receiver");
}

bool testPersistedStateAndCleanup(const QTemporaryDir &temporaryDirectory) {
    auto graphBase = std::make_shared<GraphBase>();
    TaskRunner taskRunner;
    auto image = makeSegmentation(1);
    itkSignal<dataType::SegmentIdType> signal(image, false);
    signal.name = QStringLiteral("Loaded");
    signal.sourceFilePath = temporaryDirectory.filePath(QStringLiteral("loaded.nrrd"));
    graphBase->pSelectedSegmentation = image;
    graphBase->pSelectedSegmentationSignal = &signal;

    SelectedSegmentationAutosave autosave(graphBase, &taskRunner);
    autosave.applySettings({true, 120});
    if (!check(!autosave.hasUnsavedSegmentations(),
               "a newly loaded segmentation should start clean")) {
        return false;
    }

    image->SetPixel({0, 0, 0}, 2);
    image->Modified();
    if (!check(autosave.hasUnsavedSegmentations(),
               "modifying a loaded segmentation should make it unsaved")) {
        return false;
    }
    autosave.requestAutosave();
    if (!check(waitForRecovery(autosave, taskRunner),
               "modified loaded segmentation did not produce a recovery file")) {
        return false;
    }

    const QString recoveryPath = autosave.selectedSegmentationRecoveryPath();
    autosave.cleanup();
    if (!check(QFile::exists(recoveryPath) && autosave.hasUnsavedSegmentations(),
               "cleanup removed recovery data that was not regularly saved")) {
        return false;
    }
    autosave.applySettings({false, 120});
    if (!check(QFile::exists(recoveryPath),
               "disabling autosave removed unsaved recovery data")) {
        return false;
    }
    autosave.applySettings({true, 120});

    const quintptr imageIdentity = reinterpret_cast<quintptr>(image.GetPointer());
    const quintptr signalIdentity = reinterpret_cast<quintptr>(&signal);
    autosave.recordSelectedSegmentationSave(
        imageIdentity,
        signalIdentity,
        0,
        temporaryDirectory.filePath(QStringLiteral("stale-export.nrrd")));
    if (!check(QFile::exists(recoveryPath) && autosave.hasUnsavedSegmentations(),
               "a save older than the recovery deleted unsaved data")) {
        return false;
    }

    autosave.recordSelectedSegmentationSave(
        imageIdentity,
        signalIdentity,
        static_cast<quint64>(image->GetMTime()),
        temporaryDirectory.filePath(QStringLiteral("exported.nrrd")));
    return check(!autosave.hasUnsavedSegmentations() && !QFile::exists(recoveryPath),
                 "a successful regular save did not clear covered recovery data");
}

bool testRecoveryGenerationsAndSelectionSwitch(const QTemporaryDir &temporaryDirectory) {
    auto graphBase = std::make_shared<GraphBase>();
    TaskRunner taskRunner;
    auto loadedImage = makeSegmentation(3);
    itkSignal<dataType::SegmentIdType> loadedSignal(loadedImage, false);
    loadedSignal.name = QStringLiteral("Generation test");
    loadedSignal.sourceFilePath = temporaryDirectory.filePath(QStringLiteral("generation.nrrd"));
    graphBase->pSelectedSegmentation = loadedImage;
    graphBase->pSelectedSegmentationSignal = &loadedSignal;

    SelectedSegmentationAutosave autosave(graphBase, &taskRunner);
    autosave.applySettings({true, 120});
    loadedImage->SetPixel({0, 0, 0}, 4);
    loadedImage->Modified();
    autosave.requestAutosave();
    if (!check(waitForRecovery(autosave, taskRunner),
               "first recovery generation did not finish")) {
        return false;
    }
    const QString olderGeneration = autosave.selectedSegmentationRecoveryPath();

    loadedImage->SetPixel({0, 0, 0}, 5);
    loadedImage->Modified();
    autosave.requestAutosave();
    if (!check(waitUntil([&]() {
            return !taskRunner.isBusy()
                && !autosave.isWriteInProgress()
                && autosave.selectedSegmentationRecoveryPath() != olderGeneration;
        }), "a newer recovery generation did not replace the older generation")) {
        return false;
    }
    const QString newerGeneration = autosave.selectedSegmentationRecoveryPath();
    if (!check(QFile::exists(newerGeneration) && !QFile::exists(olderGeneration),
               "the newer recovery generation did not replace the older one")) {
        return false;
    }

    auto newImage = makeSegmentation(7);
    itkSignal<dataType::SegmentIdType> newSignal(newImage, false);
    newSignal.name = QStringLiteral("Unsourced");
    selectSegmentation(graphBase, newImage, &newSignal, autosave);
    if (!check(autosave.hasUnsavedSegmentations(),
               "a segmentation without a source file should start unsaved")) {
        return false;
    }
    autosave.cleanup();
    if (!check(QFile::exists(newerGeneration),
               "switching segmentations removed another unsaved recovery")) {
        return false;
    }

    autosave.applySettings({true, 120});
    autosave.requestAutosave();
    if (!check(waitForRecovery(autosave, taskRunner),
               "an unsourced segmentation did not use fallback recovery storage")) {
        return false;
    }
    const QString fallbackRecovery = autosave.selectedSegmentationRecoveryPath();
    const QString fallbackDirectory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                          .filePath(QStringLiteral("recovery"));
    if (!check(QFileInfo(fallbackRecovery).absolutePath()
                   == QFileInfo(fallbackDirectory).absoluteFilePath(),
               "unsourced recovery was not written to AppDataLocation/recovery")) {
        return false;
    }

    selectSegmentation(graphBase, loadedImage, &loadedSignal, autosave);
    if (!check(autosave.selectedSegmentationRecoveryPath() == newerGeneration
                   && QFile::exists(newerGeneration),
               "reselecting a segmentation did not restore its recovery state")) {
        return false;
    }
    return check(autosave.discardUnsavedRecoveries()
                     && !QFile::exists(fallbackRecovery)
                     && !QFile::exists(newerGeneration),
                 "explicit discard did not remove all unsaved recovery files");
}

bool testInactiveSegmentationAutosave(const QTemporaryDir &temporaryDirectory) {
    auto graphBase = std::make_shared<GraphBase>();
    TaskRunner taskRunner;
    auto inactiveImage = makeSegmentation(13);
    itkSignal<dataType::SegmentIdType> inactiveSignal(inactiveImage, false);
    inactiveSignal.name = QStringLiteral("Inactive/") + QChar(0x7f)
        + QStringLiteral("dirty");
    inactiveSignal.sourceFilePath = temporaryDirectory.filePath(
        QStringLiteral("inactive.nrrd"));
    graphBase->pSelectedSegmentation = inactiveImage;
    graphBase->pSelectedSegmentationSignal = &inactiveSignal;

    SelectedSegmentationAutosave autosave(graphBase, &taskRunner);
    autosave.applySettings({true, 120});
    inactiveImage->SetPixel({0, 0, 0}, 14);
    inactiveImage->Modified();

    auto selectedImage = makeSegmentation(15);
    itkSignal<dataType::SegmentIdType> selectedSignal(selectedImage, false);
    selectedSignal.name = QStringLiteral("Selected clean");
    selectedSignal.sourceFilePath = temporaryDirectory.filePath(
        QStringLiteral("selected.nrrd"));
    selectSegmentation(graphBase, selectedImage, &selectedSignal, autosave);
    autosave.selectedSegmentationChanged();

    autosave.requestAutosave();
    if (!check(waitUntil([&]() {
            return !taskRunner.isBusy()
                && !autosave.isWriteInProgress()
                && autosave.hasUnsavedRecovery();
        }), "an inactive dirty segmentation was not autosaved")) {
        return false;
    }

    selectSegmentation(graphBase, inactiveImage, &inactiveSignal, autosave);
    const QString recoveryPath = autosave.selectedSegmentationRecoveryPath();
    if (!check(QFile::exists(recoveryPath)
                   && QFileInfo(recoveryPath).fileName().startsWith(
                       QStringLiteral("Inactive__dirty-recovery-")),
               "inactive recovery did not use the shared filename sanitizer")) {
        return false;
    }
    return check(autosave.discardUnsavedRecoveries() && !QFile::exists(recoveryPath),
                 "discard did not remove the inactive recovery");
}

bool testDiscardDeletionFailure(const QTemporaryDir &temporaryDirectory) {
    auto graphBase = std::make_shared<GraphBase>();
    TaskRunner taskRunner;
    auto image = makeSegmentation(17);
    itkSignal<dataType::SegmentIdType> signal(image, false);
    signal.name = QStringLiteral("Deletion failure");
    signal.sourceFilePath = temporaryDirectory.filePath(
        QStringLiteral("deletion-failure.nrrd"));
    graphBase->pSelectedSegmentation = image;
    graphBase->pSelectedSegmentationSignal = &signal;

    SelectedSegmentationAutosave autosave(graphBase, &taskRunner);
    autosave.applySettings({true, 120});
    image->SetPixel({0, 0, 0}, 18);
    image->Modified();
    autosave.requestAutosave();
    if (!check(waitForRecovery(autosave, taskRunner),
               "deletion failure setup did not produce a recovery")) {
        return false;
    }

    const QString recoveryPath = autosave.selectedSegmentationRecoveryPath();
    if (!check(QFile::remove(recoveryPath) && QDir().mkpath(recoveryPath),
               "could not replace the recovery file with a directory")) {
        return false;
    }
    if (!check(!autosave.discardUnsavedRecoveries()
                   && autosave.selectedSegmentationRecoveryPath() == recoveryPath,
               "a failed recovery deletion was reported as successful")) {
        return false;
    }
    if (!check(QDir(recoveryPath).removeRecursively(),
               "could not remove the recovery placeholder directory")) {
        return false;
    }
    return check(autosave.discardUnsavedRecoveries()
                     && autosave.selectedSegmentationRecoveryPath().isEmpty(),
                 "discard did not succeed after the deletion error was resolved");
}

bool testDestructorProtection() {
    auto graphBase = std::make_shared<GraphBase>();
    TaskRunner taskRunner;
    QString completedRecovery;
    {
        auto image = makeSegmentation(9);
        itkSignal<dataType::SegmentIdType> signal(image, false);
        signal.name = QStringLiteral("Destructor protected");
        graphBase->pSelectedSegmentation = image;
        graphBase->pSelectedSegmentationSignal = &signal;
        SelectedSegmentationAutosave autosave(graphBase, &taskRunner);
        autosave.applySettings({true, 120});
        autosave.requestAutosave();
        if (!check(waitForRecovery(autosave, taskRunner),
                   "destructor protection setup did not finish")) {
            return false;
        }
        completedRecovery = autosave.selectedSegmentationRecoveryPath();
    }
    if (!check(QFile::exists(completedRecovery),
               "the autosave destructor removed unsaved recovery data")) {
        return false;
    }
    QFile::remove(completedRecovery);

    auto inFlightImage = makeSegmentation(11);
    itkSignal<dataType::SegmentIdType> inFlightSignal(inFlightImage, false);
    const QString inFlightName = QStringLiteral("In-flight-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    inFlightSignal.name = inFlightName;
    graphBase->pSelectedSegmentation = inFlightImage;
    graphBase->pSelectedSegmentationSignal = &inFlightSignal;
    auto inFlightAutosave = std::make_unique<SelectedSegmentationAutosave>(
        graphBase, &taskRunner);
    inFlightAutosave->applySettings({true, 120});
    inFlightAutosave->requestAutosave();
    if (!check(inFlightAutosave->isWriteInProgress(),
               "in-flight destructor protection setup did not start")) {
        return false;
    }
    inFlightAutosave.reset();
    if (!check(waitUntil([&]() { return !taskRunner.isBusy(); }),
               "in-flight autosave did not finish after manager destruction")) {
        return false;
    }

    const QDir recoveryDirectory(QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                     .filePath(QStringLiteral("recovery")));
    const QStringList recoveries = recoveryDirectory.entryList(
        {QStringLiteral("%1-recovery-*.nrrd").arg(inFlightName)}, QDir::Files);
    if (!check(recoveries.size() == 1,
               "destroying the manager during a write did not preserve its recovery")) {
        return false;
    }
    QFile::remove(recoveryDirectory.filePath(recoveries.front()));
    return true;
}

} // namespace

int main(int argc, char *argv[]) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SegmentPuzzlerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SelectedSegmentationAutosave"));
    QSettings().clear();

    QTemporaryDir temporaryDirectory;
    const bool passed = check(temporaryDirectory.isValid(),
                              "temporary directory could not be created")
        && testApplicationQuitEventFilter()
        && testPersistedStateAndCleanup(temporaryDirectory)
        && testInactiveSegmentationAutosave(temporaryDirectory)
        && testDiscardDeletionFailure(temporaryDirectory)
        && testRecoveryGenerationsAndSelectionSwitch(temporaryDirectory)
        && testDestructorProtection();

    QSettings().clear();
    if (!passed) {
        return 1;
    }
    std::cout << "Selected segmentation autosave lifecycle tests passed\n";
    return 0;
}
