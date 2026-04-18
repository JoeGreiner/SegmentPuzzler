// Automated pipeline test / profiling run for the full watershed GUI pipeline.
//
// Downloads the sample boundary prediction file (or uses the cached copy from
// a previous run), then drives the WatershedControl through all four pipeline
// steps with default settings, runs agglomertion, and exits cleanly.
//
// Build with BUILD_TESTING=ON.  Run from the build directory.

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
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

namespace {

// Downloads url to outputFilePath, following up to maxRedirects redirects.
// Returns true on success.  If the file already exists it is reused.
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

        QVariant redirection = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirection.isValid()) {
            QUrl newUrl = redirection.toUrl();
            if (newUrl.isRelative()) {
                newUrl = request.url().resolved(newUrl);
            }
            request.setUrl(newUrl);
            reply->deleteLater();
            continue;
        }

        QByteArray data = reply->readAll();
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

// Returns path to the boundary prediction file, downloading if needed.
QString ensureBoundaryFile() {
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString filePath = QDir(tempDir).filePath("BoundaryPrediction.tif");
    const QString url = "https://drive.google.com/uc?export=download&id=1YMFz84x8_E4OVh74ABn8j2pcpGXKtu8r";
    return downloadFile(url, filePath) ? filePath : QString{};
}

} // namespace

int main(int argc, char *argv[]) {
#ifndef _WIN32
    setenv("ITK_AUTOLOAD_PATH", "", 1);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("SegmentPuzzler");
    QApplication::setOrganizationName("JoeGreiner");

    // Parse --threads N (default: all logical cores) and --fast-marker
    int threadCount = QThread::idealThreadCount();
    bool useFastMarker = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--threads" && i + 1 < argc) {
            try {
                threadCount = std::stoi(argv[i + 1]);
            } catch (const std::exception &) {
                std::cerr << "Invalid --threads value: " << argv[i + 1] << "\n";
                return 1;
            }
        }
        if (std::string(argv[i]) == "--fast-marker") {
            useFastMarker = true;
        }
    }
    std::cout << "Thread count: " << threadCount << "\n";
    std::cout << "Watershed algorithm: " << (useFastMarker ? "fast-marker" : "morphological") << "\n";

    // Apply dark stylesheet if available
    QFile styleFile(":qdarkstyle/dark/style.qss");
    if (styleFile.exists()) {
        styleFile.open(QFile::ReadOnly | QFile::Text);
        app.setStyleSheet(QTextStream(&styleFile).readAll());
    }

    const QString boundaryPath = ensureBoundaryFile();
    if (boundaryPath.isEmpty()) {
        std::cerr << "Failed to obtain boundary prediction file. Aborting.\n";
        return 1;
    }

    auto *window = new MainWindowWatershedControl();
    window->show();

    // Kick off the pipeline once the event loop is running.
    QTimer::singleShot(0, [window, boundaryPath, threadCount, useFastMarker]() {
        WatershedControl *ws = window->myWatershedControl;
        ws->setThreadCount(threadCount);
        if (useFastMarker) {
            ws->setWatershedAlgorithm(WatershedAlgorithm::FastMarkerWatershed);
        }

        std::cout << "\n=== Loading boundary file ===\n";
        ws->addBoundariesFromFile(boundaryPath);

        std::cout << "\n=== Step 1: Threshold ===\n";
        ws->thresholdBoundariesAsync([ws]() {
            std::cout << "\n=== Step 2: Distance map ===\n";
            ws->calculateDistanceMapAsync([ws]() {
                std::cout << "\n=== Step 3: Seed extraction ===\n";
                ws->extractSeedsAsync([ws]() {
                    std::cout << "\n=== Step 4: Watershed ===\n";
                    ws->watershedAsync([ws]() {
                        std::cout << "\n=== Step 5: Agglomertion ===\n";
                        ws->agglomertionAsync([]() {
                            std::cout << "\n=== Pipeline complete ===\n";
                            QApplication::quit();
                        });
                    });
                });
            });
        });
    });

    return QApplication::exec();
}
