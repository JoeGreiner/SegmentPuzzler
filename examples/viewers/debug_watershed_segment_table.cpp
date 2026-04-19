#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <iostream>
#include <stdexcept>

#ifndef _WIN32
#include <cstdlib>
#endif

#include "src/controllers/MainWindowWatershedControl.h"
#include "src/controllers/watershedControl.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SegmentTableDialog.h"

namespace {

void printUsage(const char *argv0) {
    std::cout << "Usage: " << argv0 << " [--threads N] [--timeout-ms N] [--fast-marker] [--boundary PATH]\n";
}

bool downloadFile(const QString &url, const QString &outputFilePath, int maxRedirects = 5) {
    if (QFile::exists(outputFilePath)) {
        std::cout << "Using cached file: " << outputFilePath.toStdString() << "\n";
        return true;
    }

    std::cout << "Downloading " << outputFilePath.toStdString() << " ...\n";
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};

    for (int redirect = 0; redirect < maxRedirects; ++redirect) {
        QNetworkReply *reply = manager.get(request);

        QObject::connect(reply, &QNetworkReply::downloadProgress,
                         [](qint64 received, qint64 total) {
                             if (total > 0) {
                                 std::cout << "\r  " << received / 1024 << " / "
                                           << total / 1024 << " KB   " << std::flush;
                             }
                         });

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            std::cout << "\nDownload error: " << reply->errorString().toStdString() << "\n";
            reply->deleteLater();
            return false;
        }

        const QVariant redirection = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirection.isValid()) {
            QUrl newUrl = redirection.toUrl();
            if (newUrl.isRelative()) {
                newUrl = request.url().resolved(newUrl);
            }
            request.setUrl(newUrl);
            reply->deleteLater();
            continue;
        }

        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (data.isEmpty()) {
            std::cout << "\nNo data received.\n";
            return false;
        }

        QFile file(outputFilePath);
        if (!file.open(QIODevice::WriteOnly)) {
            std::cout << "\nCannot write to: " << outputFilePath.toStdString() << "\n";
            return false;
        }
        file.write(data);
        file.close();
        std::cout << "\nDownloaded to: " << outputFilePath.toStdString() << "\n";
        return true;
    }

    std::cout << "Too many redirects.\n";
    return false;
}

QString ensureBoundaryFile() {
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString filePath = QDir(tempDir).filePath("BoundaryPrediction.tif");
    const QString url = "https://drive.google.com/uc?export=download&id=1YMFz84x8_E4OVh74ABn8j2pcpGXKtu8r";
    return downloadFile(url, filePath) ? filePath : QString{};
}

class GuiDebugRunner : public QObject {
    Q_OBJECT
public:
    GuiDebugRunner(QApplication &appIn,
                   MainWindowWatershedControl *windowIn,
                   QString boundaryPathIn,
                   int threadCountIn,
                   bool useFastMarkerIn,
                   int timeoutMsIn)
        : app(appIn),
          window(windowIn),
          ws(windowIn->myWatershedControl),
          boundaryPath(std::move(boundaryPathIn)),
          threadCount(threadCountIn),
          useFastMarker(useFastMarkerIn),
          timeoutMs(timeoutMsIn) {}

public slots:
    void start() {
        if (!QTest::qWaitForWindowExposed(window, timeoutMs)) {
            fail("Watershed window was not exposed");
            return;
        }

        ws->setThreadCount(threadCount);
        if (useFastMarker) {
            ws->setWatershedAlgorithm(WatershedAlgorithm::FastMarkerWatershed);
        }

        std::cout << "\n=== Loading boundary file ===\n";
        ws->addBoundariesFromFile(boundaryPath);

        auto *thresholdButton = requireButton("thresholdBoundariesButton");
        auto *distanceButton = requireButton("calculateDistanceMapButton");
        auto *seedsButton = requireButton("calculateSeedsButton");
        auto *watershedButton = requireButton("runWatershedButton");
        auto *inspectButton = requireButton("inspectSegmentsButton");
        if (thresholdButton == nullptr || distanceButton == nullptr || seedsButton == nullptr
            || watershedButton == nullptr || inspectButton == nullptr) {
            return;
        }

        if (!waitUntil([thresholdButton]() { return thresholdButton->isEnabled(); }, "threshold enabled")) {
            return;
        }

        clickButton(thresholdButton, "Step 1: Threshold");
        if (!waitUntil([this, distanceButton]() { return !ws->taskRunner->isBusy() && distanceButton->isEnabled(); },
                       "distance map ready")) {
            return;
        }

        clickButton(distanceButton, "Step 2: Distance map");
        if (!waitUntil([this, seedsButton]() { return !ws->taskRunner->isBusy() && seedsButton->isEnabled(); },
                       "seed extraction ready")) {
            return;
        }

        clickButton(seedsButton, "Step 3: Seed extraction");
        if (!waitUntil([this, watershedButton]() { return !ws->taskRunner->isBusy() && watershedButton->isEnabled(); },
                       "watershed ready")) {
            return;
        }

        clickButton(watershedButton, "Step 4: Watershed");
        if (!waitUntil([this, inspectButton]() { return !ws->taskRunner->isBusy() && inspectButton->isEnabled(); },
                       "segment inspection ready")) {
            return;
        }

        clickButton(inspectButton, "Step 5: Inspect Segments");
        if (!waitUntil([this]() {
                return ws->findChild<SegmentTableDialog *>() != nullptr;
            }, "segment table created")) {
            return;
        }

        auto *dialog = ws->findChild<SegmentTableDialog *>();
        if (dialog == nullptr) {
            fail("Segment table dialog was not found");
            return;
        }

        if (ws->pAgglomertionPreviewSignal == nullptr || ws->pAgglomertionPreviewSignal->pImage.IsNull()) {
            fail("Agglomertion preview signal was not available for pre-agglomertion inspection");
            return;
        }
        if (ws->graphBase->pSelectedSegmentation != ws->pAgglomertionPreviewSignal->pImage
            || ws->graphBase->pSelectedSegmentationSignal != ws->pAgglomertionPreviewSignal) {
            fail("Inspect Segments did not use the agglomertion preview before a persisted agglomertion existed");
            return;
        }

        if (!QTest::qWaitForWindowExposed(dialog, timeoutMs)) {
            fail("Segment table dialog was not exposed");
            return;
        }

        QSignalSpy finishedSpy(dialog, SIGNAL(computeFinishedDebug()));
        if (!finishedSpy.isValid()) {
            fail("computeFinishedDebug signal is not available");
            return;
        }

        std::cout << "[WatershedGuiDebug] waiting for segment table compute to finish\n";
        if (!finishedSpy.wait(timeoutMs)) {
            fail("Timed out while waiting for segment table compute");
            return;
        }

        std::cout << "\n=== GUI debug run complete ===\n";
        QTimer::singleShot(250, &app, &QApplication::quit);
    }

private:
    QPushButton *requireButton(const char *objectName) {
        auto *button = ws->findChild<QPushButton *>(QString::fromLatin1(objectName));
        if (button == nullptr) {
            fail(QString("Button not found: %1").arg(QString::fromLatin1(objectName)));
        }
        return button;
    }

    void clickButton(QPushButton *button, const char *label) {
        std::cout << "\n=== " << label << " ===\n";
        std::cout << "[WatershedGuiDebug] clicking " << button->objectName().toStdString()
                  << " text=\"" << button->text().toStdString() << "\"\n";
        QTest::mouseClick(button, Qt::LeftButton);
    }

    bool waitUntil(const std::function<bool()> &predicate, const char *label) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (predicate()) {
                std::cout << "[WatershedGuiDebug] reached " << label
                          << " after " << timer.elapsed() << " ms\n";
                return true;
            }
            app.processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(25);
        }
        fail(QString("Timed out waiting for %1").arg(QString::fromLatin1(label)));
        return false;
    }

    void fail(const QString &message) {
        std::cerr << "[WatershedGuiDebug] ERROR: " << message.toStdString() << "\n";
        QTimer::singleShot(0, [&]() { app.exit(1); });
    }

    QApplication &app;
    MainWindowWatershedControl *window;
    WatershedControl *ws;
    QString boundaryPath;
    int threadCount;
    bool useFastMarker;
    int timeoutMs;
};

} // namespace

int main(int argc, char *argv[]) {
#ifndef _WIN32
    setenv("ITK_AUTOLOAD_PATH", "", 1);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("SegmentPuzzler");
    QApplication::setOrganizationName("JoeGreiner");

    int threadCount = QThread::idealThreadCount();
    bool useFastMarker = false;
    int timeoutMs = 300000;
    QString boundaryPathOverride;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if (argument == "--threads" && i + 1 < argc) {
            try {
                threadCount = std::stoi(argv[i + 1]);
            } catch (const std::exception &) {
                std::cerr << "Invalid --threads value: " << argv[i + 1] << "\n";
                return 1;
            }
            ++i;
            continue;
        }
        if (argument == "--timeout-ms" && i + 1 < argc) {
            try {
                timeoutMs = std::stoi(argv[i + 1]);
            } catch (const std::exception &) {
                std::cerr << "Invalid --timeout-ms value: " << argv[i + 1] << "\n";
                return 1;
            }
            ++i;
            continue;
        }
        if (argument == "--boundary" && i + 1 < argc) {
            boundaryPathOverride = QString::fromLocal8Bit(argv[i + 1]);
            ++i;
            continue;
        }
        if (argument == "--fast-marker") {
            useFastMarker = true;
            continue;
        }
        std::cerr << "Unknown argument: " << argument << "\n";
        printUsage(argv[0]);
        return 1;
    }

    std::cout << "Thread count: " << threadCount << "\n";
    std::cout << "Watershed algorithm: " << (useFastMarker ? "fast-marker" : "morphological") << "\n";
    std::cout << "Timeout: " << timeoutMs << " ms\n";

    QFile styleFile(":qdarkstyle/dark/style.qss");
    if (styleFile.exists()) {
        styleFile.open(QFile::ReadOnly | QFile::Text);
        app.setStyleSheet(QTextStream(&styleFile).readAll());
    }

    const QString boundaryPath = boundaryPathOverride.isEmpty() ? ensureBoundaryFile() : boundaryPathOverride;
    if (!boundaryPathOverride.isEmpty() && !QFile::exists(boundaryPath)) {
        std::cerr << "Boundary file does not exist: " << boundaryPath.toStdString() << "\n";
        return 1;
    }
    if (boundaryPath.isEmpty()) {
        std::cerr << "Failed to obtain boundary prediction file. Aborting.\n";
        return 1;
    }

    auto *window = new MainWindowWatershedControl();
    window->show();
    window->raise();
    window->activateWindow();

    auto *runner = new GuiDebugRunner(app, window, boundaryPath, threadCount, useFastMarker, timeoutMs);
    QTimer::singleShot(0, runner, &GuiDebugRunner::start);

    return QApplication::exec();
}

#include "debug_watershed_segment_table.moc"
