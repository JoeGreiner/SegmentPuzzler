// Automated pipeline test / profiling run for the full watershed GUI pipeline.
//
// Downloads the sample boundary prediction file (or uses the cached copy from
// a previous run), then drives the WatershedControl through all four pipeline
// steps with default settings, runs agglomertion, and exits cleanly.
//
// Build with BUILD_TESTING=ON.  Run from the build directory.

#include <QApplication>
#include <QCheckBox>
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
#include <array>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <vector>

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

template <typename ImagePointer>
bool imagesEqual(ImagePointer a, ImagePointer b) {
    if (a.IsNull() || b.IsNull()) {
        return false;
    }
    if (a->GetLargestPossibleRegion().GetSize() != b->GetLargestPossibleRegion().GetSize()) {
        return false;
    }

    const auto voxelCount = a->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *aBuffer = a->GetBufferPointer();
    const auto *bBuffer = b->GetBufferPointer();
    for (size_t index = 0; index < voxelCount; ++index) {
        if (aBuffer[index] != bBuffer[index]) {
            return false;
        }
    }
    return true;
}

template <typename ImagePointer>
bool nonZeroLabelsAreConnected(ImagePointer image, dataType::SegmentIdType *badLabelOut = nullptr) {
    if (image.IsNull()) {
        return false;
    }

    const auto size = image->GetLargestPossibleRegion().GetSize();
    const std::ptrdiff_t dimX = static_cast<std::ptrdiff_t>(size[0]);
    const std::ptrdiff_t dimY = static_cast<std::ptrdiff_t>(size[1]);
    const std::ptrdiff_t dimZ = static_cast<std::ptrdiff_t>(size[2]);
    const std::ptrdiff_t planeXY = dimX * dimY;
    const std::ptrdiff_t total = dimX * dimY * dimZ;
    const auto *buffer = image->GetBufferPointer();

    std::vector<unsigned char> visited(static_cast<size_t>(total), 0);
    std::unordered_map<dataType::SegmentIdType, bool> seenLabel;
    std::queue<std::ptrdiff_t> open;
    const std::array<std::ptrdiff_t, 6> offsetX{{1, -1, 0, 0, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetY{{0, 0, 1, -1, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetZ{{0, 0, 0, 0, 1, -1}};

    for (std::ptrdiff_t seed = 0; seed < total; ++seed) {
        const dataType::SegmentIdType label = buffer[seed];
        if (label == 0 || visited[seed] != 0) {
            visited[seed] = 1;
            continue;
        }

        if (seenLabel[label]) {
            if (badLabelOut != nullptr) {
                *badLabelOut = label;
            }
            return false;
        }
        seenLabel[label] = true;

        visited[seed] = 1;
        open.push(seed);
        while (!open.empty()) {
            const std::ptrdiff_t current = open.front();
            open.pop();

            const std::ptrdiff_t z = current / planeXY;
            const std::ptrdiff_t remainder = current % planeXY;
            const std::ptrdiff_t y = remainder / dimX;
            const std::ptrdiff_t x = remainder % dimX;

            for (size_t direction = 0; direction < offsetX.size(); ++direction) {
                const std::ptrdiff_t nx = x + offsetX[direction];
                const std::ptrdiff_t ny = y + offsetY[direction];
                const std::ptrdiff_t nz = z + offsetZ[direction];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ) {
                    continue;
                }

                const std::ptrdiff_t neighbor = nx + ny * dimX + nz * planeXY;
                if (visited[neighbor] != 0 || buffer[neighbor] != label) {
                    continue;
                }
                visited[neighbor] = 1;
                open.push(neighbor);
            }
        }
    }
    return true;
}

std::ptrdiff_t findMaskedBoundaryVoxel(dataType::SegmentsImageType::Pointer boundaryMaskedLabels,
                                            dataType::SegmentsImageType::Pointer boundaryFilledLabels) {
    if (boundaryMaskedLabels.IsNull() || boundaryFilledLabels.IsNull()) {
        return -1;
    }
    const auto voxelCount = boundaryMaskedLabels->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *boundaryMaskedBuffer = boundaryMaskedLabels->GetBufferPointer();
    const auto *boundaryFilledBuffer = boundaryFilledLabels->GetBufferPointer();
    for (size_t index = 0; index < voxelCount; ++index) {
        if (boundaryMaskedBuffer[index] == 0 && boundaryFilledBuffer[index] != 0) {
            return static_cast<std::ptrdiff_t>(index);
        }
    }
    return -1;
}

bool failTest(const std::string &message) {
    std::cerr << "Assertion failed: " << message << "\n";
    QApplication::exit(1);
    return true;
}

} // namespace

int main(int argc, char *argv[]) {
#ifndef _WIN32
    setenv("ITK_AUTOLOAD_PATH", "", 1);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("SegmentPuzzler");
    QApplication::setOrganizationName("JoeGreiner");

    bool useFastMarker = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--fast-marker") {
            useFastMarker = true;
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return 1;
        }
    }
    std::cout << "Watershed algorithm: " << (useFastMarker ? "fast-marker" : "GUI default") << "\n";

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
    QTimer::singleShot(0, [window, boundaryPath, useFastMarker]() {
        WatershedControl *ws = window->myWatershedControl;

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
                        if (ws->pWatershed.IsNull() || ws->pWatershedFragments.IsNull()) {
                            failTest("watershed outputs are missing");
                            return;
                        }
                        dataType::SegmentIdType badLabel = 0;
                        if (!nonZeroLabelsAreConnected(ws->pWatershed, &badLabel)) {
                            failTest("watershed boundary-masked label is disconnected: " + std::to_string(badLabel));
                            return;
                        }
                        if (!imagesEqual(ws->pWatershed, ws->graphBase->pWorkingSegmentsImage)) {
                            failTest("graph working image does not match watershed boundary-masked labels");
                            return;
                        }
                        const std::ptrdiff_t cutVoxelIndex = findMaskedBoundaryVoxel(ws->pWatershed, ws->pWatershedFragments);
                        if (cutVoxelIndex < 0) {
                            failTest("no watershed voxel was preserved in boundary-filled labels while zero-cut in boundary-masked labels");
                            return;
                        }

                        std::cout << "\n=== Step 5: Agglomertion ===\n";
                        ws->agglomertionAsync([ws, cutVoxelIndex]() {
                            if (ws->pAgglomertion.IsNull() || ws->pAgglomertionFragments.IsNull()) {
                                failTest("agglomeration outputs are missing");
                                return;
                            }
                            dataType::SegmentIdType badLabel = 0;
                            if (!nonZeroLabelsAreConnected(ws->pAgglomertion, &badLabel)) {
                                failTest("agglomeration boundary-masked label is disconnected: " + std::to_string(badLabel));
                                return;
                            }
                            if (!imagesEqual(ws->pAgglomertion, ws->graphBase->pWorkingSegmentsImage)) {
                                failTest("graph working image does not match agglomeration boundary-masked labels");
                                return;
                            }
                            const auto *agglomBoundaryFilled = ws->pAgglomertionFragments->GetBufferPointer();
                            const auto *agglomBoundaryMasked = ws->pAgglomertion->GetBufferPointer();
                            if (agglomBoundaryFilled[cutVoxelIndex] == 0) {
                                failTest("agglomeration boundary-filled labels kept a zero-cut watershed voxel inactive");
                                return;
                            }
                            if (agglomBoundaryMasked[cutVoxelIndex] != 0) {
                                failTest("agglomeration boundary-masked labels failed to keep the injected boundary cut");
                                return;
                            }
                            QTimer::singleShot(250, [ws, cutVoxelIndex]() {
                                if (ws->pAgglomertionPreviewSignal != nullptr &&
                                    ws->pAgglomertionPreviewSignal->getIsActive()) {
                                    failTest("final agglomeration scheduled a redundant live preview");
                                    return;
                                }
                                std::cout << "\n=== Explicit live preview refresh ===\n";
                                auto *livePreviewCheckBox =
                                    ws->findChild<QCheckBox *>("agglomertionPreviewCheckBox");
                                if (livePreviewCheckBox == nullptr) {
                                    failTest("live preview checkbox is missing");
                                    return;
                                }
                                livePreviewCheckBox->setChecked(false);
                                livePreviewCheckBox->setChecked(true);
                                QTimer::singleShot(500, [ws, livePreviewCheckBox, cutVoxelIndex]() {
                                    if (ws->pAgglomertionPreviewSignal == nullptr ||
                                        !ws->pAgglomertionPreviewSignal->getIsActive()) {
                                        failTest("explicit live preview refresh did not create an active preview");
                                        return;
                                    }
                                    if (ws->pAgglomertionPreviewSignal->pImage->GetBufferPointer()[cutVoxelIndex] != 0) {
                                        failTest("explicit live preview failed to inject a known boundary voxel");
                                        return;
                                    }
                                    auto *previewSignal = ws->pAgglomertionPreviewSignal;
                                    livePreviewCheckBox->setChecked(false);
                                    if (previewSignal->getIsActive() || previewSignal->pImage.IsNotNull()) {
                                        failTest("disabling live preview did not release its image");
                                        return;
                                    }

                                    livePreviewCheckBox->setChecked(true);
                                    QTimer::singleShot(500, [ws, previewSignal, cutVoxelIndex]() {
                                        if (ws->pAgglomertionPreviewSignal != previewSignal ||
                                            !previewSignal->getIsActive() || previewSignal->pImage.IsNull()) {
                                            failTest("live preview signal was not reused after releasing its image");
                                            return;
                                        }
                                        if (previewSignal->pImage->GetBufferPointer()[cutVoxelIndex] != 0) {
                                            failTest("recreated live preview failed to inject a known boundary voxel");
                                            return;
                                        }
                                        std::cout << "\n=== Pipeline complete ===\n";
                                        QApplication::quit();
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });

    return QApplication::exec();
}
