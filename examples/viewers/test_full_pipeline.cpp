// Automated full pipeline test / profiling run.
//
// Drives the WatershedControl through all steps, including
// segment feature computation and agglomeration with size bias.
//
// Build with BUILD_TESTING=ON.

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
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <itkCastImageFilter.h>
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
#include "src/qtUtils/SegmentTableDialog.h"
#include "src/utils/WatershedRagAgglomeration.h"
#include "src/utils/utils.h"

namespace {

void printUsage(const char *argv0) {
    std::cout << "Usage: " << argv0 << " [--threads N] [--boundary PATH]\n";
}

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
uint64_t imageHash(ImagePointer image) {
    if (image.IsNull()) {
        return 0;
    }
    const auto voxelCount = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *buffer = image->GetBufferPointer();
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < voxelCount; ++index) {
        hash ^= static_cast<uint64_t>(buffer[index]) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    }
    return hash;
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

struct LabelIsolationInfo {
    uint64_t voxelCount = 0;
    bool touchesOtherNonZeroLabel = false;
    dataType::SegmentIdType firstTouchingLabel = 0;
};

template <typename ImagePointer>
std::unordered_map<dataType::SegmentIdType, LabelIsolationInfo> collectLabelIsolationInfo(ImagePointer image) {
    std::unordered_map<dataType::SegmentIdType, LabelIsolationInfo> infoByLabel;
    if (image.IsNull()) {
        return infoByLabel;
    }

    const auto size = image->GetLargestPossibleRegion().GetSize();
    const size_t dimX = size[0];
    const size_t dimY = size[1];
    const size_t dimZ = size[2];
    const size_t planeXY = dimX * dimY;
    const auto *buffer = image->GetBufferPointer();

    for (size_t z = 0; z < dimZ; ++z) {
        for (size_t y = 0; y < dimY; ++y) {
            for (size_t x = 0; x < dimX; ++x) {
                const size_t index = x + y * dimX + z * planeXY;
                const dataType::SegmentIdType label = buffer[index];
                if (label == 0) {
                    continue;
                }

                auto &labelInfo = infoByLabel[label];
                ++labelInfo.voxelCount;

                const auto markTouch = [&](size_t neighborIndex) {
                    const dataType::SegmentIdType neighborLabel = buffer[neighborIndex];
                    if (neighborLabel == 0 || neighborLabel == label) {
                        return;
                    }
                    labelInfo.touchesOtherNonZeroLabel = true;
                    if (labelInfo.firstTouchingLabel == 0) {
                        labelInfo.firstTouchingLabel = neighborLabel;
                    }
                    auto &neighborInfo = infoByLabel[neighborLabel];
                    neighborInfo.touchesOtherNonZeroLabel = true;
                    if (neighborInfo.firstTouchingLabel == 0) {
                        neighborInfo.firstTouchingLabel = label;
                    }
                };

                if (x + 1 < dimX) {
                    markTouch(index + 1);
                }
                if (y + 1 < dimY) {
                    markTouch(index + dimX);
                }
                if (z + 1 < dimZ) {
                    markTouch(index + planeXY);
                }
            }
        }
    }

    return infoByLabel;
}

struct SmallDisplaySummary {
    std::size_t smallLabelCount = 0;
    std::size_t nonIsolatedSmallLabelCount = 0;
};

SmallDisplaySummary summarizeSmallDisplayLabels(dataType::SegmentsImageType::Pointer image, uint64_t threshold) {
    SmallDisplaySummary summary;
    const auto isolationInfoByLabel = collectLabelIsolationInfo(image);
    for (const auto &entry : isolationInfoByLabel) {
        if (entry.second.voxelCount >= threshold) {
            continue;
        }
        ++summary.smallLabelCount;
        if (entry.second.touchesOtherNonZeroLabel) {
            ++summary.nonIsolatedSmallLabelCount;
        }
    }
    return summary;
}

segment_puzzler::BoundaryFloatImageType::Pointer castBoundaryToFloat(dataType::BoundaryImageType::Pointer boundary) {
    using CastFilterType = itk::CastImageFilter<dataType::BoundaryImageType, segment_puzzler::BoundaryFloatImageType>;
    auto castFilter = CastFilterType::New();
    castFilter->SetInput(boundary);
    castFilter->Update();
    return castFilter->GetOutput();
}

template <typename TImage, typename TValue>
typename TImage::Pointer makeImage(const std::array<size_t, 3> &dims, std::initializer_list<TValue> values) {
    const size_t voxelCount = dims[0] * dims[1] * dims[2];
    if (values.size() != voxelCount) {
        throw std::runtime_error("synthetic image voxel count mismatch");
    }

    typename TImage::Pointer image = TImage::New();
    typename TImage::RegionType region;
    typename TImage::SizeType size;
    size[0] = dims[0];
    size[1] = dims[1];
    size[2] = dims[2];
    region.SetSize(size);
    image->SetRegions(region);
    image->Allocate();
    image->FillBuffer(0);

    auto *buffer = image->GetBufferPointer();
    size_t index = 0;
    for (const auto &value : values) {
        buffer[index++] = static_cast<typename TImage::PixelType>(value);
    }
    return image;
}

void runSyntheticSizeBiasChecks() {
    {
        const auto labels = makeImage<dataType::SegmentsImageType>({2, 2, 1}, {1, 2, 3, 3});
        const auto boundary = makeImage<segment_puzzler::BoundaryFloatImageType>({2, 2, 1}, {0.50f, 0.42f, 0.70f, 0.18f});
        segment_puzzler::WatershedRagAgglomerationOptions options;
        options.linkage = segment_puzzler::RagLinkage::Average;
        options.boundaryNormalization = segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne;
        options.boundaryEvidenceStrategy = segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean;
        options.executionPolicy = segment_puzzler::AgglomerationExecutionPolicy::Serial;
        options.tau = 0.5;

        auto baseline = segment_puzzler::runWatershedRagAgglomeration(labels, boundary, nullptr, options);
        if (baseline.stats.outputClusterCount != 2) {
            throw std::runtime_error("synthetic soft-bias baseline did not keep two clusters");
        }

        options.sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::SoftBias;
        options.sizeBiasThreshold = 2;
        options.sizeBiasStrength = 0.4;
        auto softBias = segment_puzzler::runWatershedRagAgglomeration(labels, boundary, nullptr, options);
        if (softBias.stats.outputClusterCount != 1) {
            throw std::runtime_error("synthetic soft bias did not change merge order toward the small cluster");
        }
    }

    {
        const auto labels = makeImage<dataType::SegmentsImageType>({4, 1, 1}, {1, 2, 2, 2});
        segment_puzzler::WatershedRagAgglomerationOptions options;
        options.linkage = segment_puzzler::RagLinkage::Average;
        options.boundaryNormalization = segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne;
        options.boundaryEvidenceStrategy = segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean;
        options.executionPolicy = segment_puzzler::AgglomerationExecutionPolicy::Serial;
        options.tau = 0.5;
        options.sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::Cleanup;
        options.sizeBiasThreshold = 2;
        options.sizeBiasProtection = 0.1;

        const auto mergeableBoundary =
            makeImage<segment_puzzler::BoundaryFloatImageType>({4, 1, 1}, {0.60f, 0.50f, 0.50f, 0.50f});
        auto cleanupMerged = segment_puzzler::runWatershedRagAgglomeration(labels, mergeableBoundary, nullptr, options);
        if (cleanupMerged.stats.outputClusterCount != 1 || cleanupMerged.stats.sizeBiasCleanupMergeCount != 1) {
            throw std::runtime_error("synthetic cleanup did not absorb the protected small cluster");
        }

        const auto blockedBoundary =
            makeImage<segment_puzzler::BoundaryFloatImageType>({4, 1, 1}, {0.80f, 0.60f, 0.60f, 0.60f});
        auto cleanupBlocked = segment_puzzler::runWatershedRagAgglomeration(labels, blockedBoundary, nullptr, options);
        if (cleanupBlocked.stats.outputClusterCount != 2 ||
            cleanupBlocked.stats.sizeBiasCleanupMergeCount != 0 ||
            cleanupBlocked.stats.finalCleanupEligibleSmallClusterCount != 0) {
            throw std::runtime_error("synthetic cleanup crossed the protection floor unexpectedly");
        }
    }
}

std::ptrdiff_t findDisplayOnlyBoundaryVoxel(dataType::SegmentsImageType::Pointer displayLabels,
                                            dataType::SegmentsImageType::Pointer canonicalLabels) {
    if (displayLabels.IsNull() || canonicalLabels.IsNull()) {
        return -1;
    }
    const auto voxelCount = displayLabels->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *displayBuffer = displayLabels->GetBufferPointer();
    const auto *canonicalBuffer = canonicalLabels->GetBufferPointer();
    for (size_t index = 0; index < voxelCount; ++index) {
        if (displayBuffer[index] == 0 && canonicalBuffer[index] != 0) {
            return static_cast<std::ptrdiff_t>(index);
        }
    }
    return -1;
}

QCheckBox *findCheckBoxByText(QWidget *parent, const QString &text) {
    const auto boxes = parent->findChildren<QCheckBox *>();
    for (auto *box : boxes) {
        if (box != nullptr && box->text() == text) {
            return box;
        }
    }
    return nullptr;
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

    int threadCount = QThread::idealThreadCount();
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
        if (argument == "--boundary" && i + 1 < argc) {
            boundaryPathOverride = QString::fromLocal8Bit(argv[i + 1]);
            ++i;
            continue;
        }
        std::cerr << "Unknown argument: " << argument << "\n";
        printUsage(argv[0]);
        return 1;
    }
    std::cout << "Thread count: " << threadCount << "\n";

    const QString boundaryPath = boundaryPathOverride.isEmpty() ? ensureBoundaryFile() : boundaryPathOverride;
    if (!boundaryPathOverride.isEmpty() && !QFile::exists(boundaryPath)) {
        std::cerr << "Boundary file does not exist: " << boundaryPath.toStdString() << "\n";
        return 1;
    }
    if (boundaryPath.isEmpty()) {
        std::cerr << "Failed to obtain boundary prediction file. Aborting.\n";
        return 1;
    }

    try {
        runSyntheticSizeBiasChecks();
        std::cout << "Synthetic size-bias checks passed.\n";
    } catch (const std::exception &exception) {
        std::cerr << "Synthetic size-bias checks failed: " << exception.what() << "\n";
        return 1;
    }

    auto *window = new MainWindowWatershedControl();
    window->show();

    QTimer::singleShot(0, [window, boundaryPath, threadCount]() {
        WatershedControl *ws = window->myWatershedControl;
        ws->setThreadCount(threadCount);

        std::cout << "\n=== Loading boundary file ===\n";
        ws->addBoundariesFromFile(boundaryPath);

        std::cout << "\n=== Step 1: Threshold ===\n";
        double tStart = utils::tic("Step 1: Threshold");
        ws->thresholdBoundariesAsync([ws, tStart]() {
            utils::toc(tStart, "Step 1: Threshold done:");

            std::cout << "\n=== Step 2: Distance map ===\n";
            double t2 = utils::tic("Step 2: Distance map");
            ws->calculateDistanceMapAsync([ws, t2]() {
                utils::toc(t2, "Step 2: Distance map done:");

                std::cout << "\n=== Step 3: Seed extraction ===\n";
                double t3 = utils::tic("Step 3: Seed extraction");
                ws->extractSeedsAsync([ws, t3]() {
                    utils::toc(t3, "Step 3: Seed extraction done:");

                    std::cout << "\n=== Step 4: Watershed ===\n";
                    double t4 = utils::tic("Step 4: Watershed");
                    ws->watershedAsync([ws, t4]() {
                        utils::toc(t4, "Step 4: Watershed done:");
                        if (ws->pWatershed.IsNull() || ws->pWatershedFragments.IsNull()) {
                            failTest("watershed outputs are missing");
                            return;
                        }
                        dataType::SegmentIdType badLabel = 0;
                        if (!nonZeroLabelsAreConnected(ws->pWatershed, &badLabel)) {
                            failTest("watershed display label is disconnected: " + std::to_string(badLabel));
                            return;
                        }
                        if (!imagesEqual(ws->pWatershed, ws->graphBase->pWorkingSegmentsImage)) {
                            failTest("graph working image does not match watershed display labels");
                            return;
                        }
                        const std::ptrdiff_t cutVoxelIndex = findDisplayOnlyBoundaryVoxel(ws->pWatershed, ws->pWatershedFragments);
                        if (cutVoxelIndex < 0) {
                            failTest("no watershed voxel was preserved in canonical labels while zero-cut in display labels");
                            return;
                        }

                        // Step 5: Segment Table Computation
                        std::cout << "\n=== Step 5: Segment Table Computation ===\n";
                        auto fragments = ws->pWatershedFragments;
                        if (fragments) {
                            SegmentTableDialog::FeatureFlags flags;
                            flags.volume            = true;
                            flags.physicalSize      = true;
                            flags.pixelsOnBorder    = true;
                            flags.perimeterOnBorder = true;
                            flags.centroid          = true;
                            flags.bbox              = true;
                            flags.elongation        = true;
                            flags.flatness          = true;
                            flags.roundness         = true;
                            flags.equivSphRadius    = true;
                            flags.equivSphPerimeter = true;
                            flags.equivEllipsoid    = true;
                            flags.principalMoments  = true;
                            flags.perimeter         = true;
                            flags.orientedBBox      = true;
                            
                            double t5 = utils::tic("Step 5: Segment Table Computation");
                            auto result = SegmentTableDialog::computeFeatures(fragments, flags);
                            utils::toc(t5, "Step 5: Segment Table Computation done:");
                            std::cout << "Computed features for " << result.rows.size() << " segments.\n";
                        }

                        const uint64_t sizeBiasThreshold = 5000;
                        auto boundaryFloat = castBoundaryToFloat(ws->pBoundaries);
                        segment_puzzler::WatershedRagAgglomerationOptions sweepOptions;
                        sweepOptions.linkage = segment_puzzler::RagLinkage::Average;
                        sweepOptions.boundaryNormalization = segment_puzzler::BoundaryNormalizationMode::AutoDetect;
                        sweepOptions.boundaryEvidenceStrategy = segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted;
                        sweepOptions.executionPolicy = segment_puzzler::AgglomerationExecutionPolicy::Serial;
                        sweepOptions.tau = 1.0;
                        sweepOptions.sizeBiasThreshold = sizeBiasThreshold;
                        sweepOptions.sizeBiasStrength = 0.3;
                        sweepOptions.sizeBiasProtection = 0.0;
                        sweepOptions.sizeBiasRespectMask = true;

                        auto runSweep = [&](segment_puzzler::SizeBiasStrategy strategy) {
                            auto options = sweepOptions;
                            options.sizeBiasStrategy = strategy;
                            auto result = segment_puzzler::runWatershedRagAgglomeration(
                                ws->pWatershedFragments,
                                boundaryFloat,
                                ws->pThresholdedMembrane,
                                options);
                            return std::make_pair(result.stats, summarizeSmallDisplayLabels(result.agglomeratedLabels, sizeBiasThreshold));
                        };

                        std::cout << "\n=== Step 6a: Direct Agglomeration Sweeps ===\n";
                        const auto offSweep = runSweep(segment_puzzler::SizeBiasStrategy::Off);
                        const auto softSweep = runSweep(segment_puzzler::SizeBiasStrategy::SoftBias);
                        const auto cleanupSweep = runSweep(segment_puzzler::SizeBiasStrategy::Cleanup);
                        const auto combinedSweep = runSweep(segment_puzzler::SizeBiasStrategy::SoftBiasAndCleanup);

                        std::cout << "Sweep small-label counts: off=" << offSweep.second.smallLabelCount
                                  << " soft=" << softSweep.second.smallLabelCount
                                  << " cleanup=" << cleanupSweep.second.smallLabelCount
                                  << " combined=" << combinedSweep.second.smallLabelCount << "\n";
                        if (softSweep.second.smallLabelCount > offSweep.second.smallLabelCount) {
                            failTest("soft-bias increased the number of sub-threshold display clusters");
                            return;
                        }
                        if (cleanupSweep.second.smallLabelCount > offSweep.second.smallLabelCount) {
                            failTest("cleanup increased the number of sub-threshold display clusters");
                            return;
                        }
                        if (combinedSweep.second.smallLabelCount > offSweep.second.smallLabelCount) {
                            failTest("soft-bias-and-cleanup increased the number of sub-threshold display clusters");
                            return;
                        }
                        if (cleanupSweep.first.finalCleanupEligibleSmallClusterCount != 0) {
                            failTest("cleanup left sub-threshold clusters with an eligible cleanup edge");
                            return;
                        }
                        if (combinedSweep.first.finalCleanupEligibleSmallClusterCount != 0) {
                            failTest("soft-bias-and-cleanup left sub-threshold clusters with an eligible cleanup edge");
                            return;
                        }
                        if (cleanupSweep.second.nonIsolatedSmallLabelCount != 0) {
                            failTest("cleanup left non-isolated sub-threshold display clusters on the sample");
                            return;
                        }
                        if (combinedSweep.second.nonIsolatedSmallLabelCount != 0) {
                            failTest("soft-bias-and-cleanup left non-isolated sub-threshold display clusters on the sample");
                            return;
                        }

                        // Step 6b: Agglomeration with Size Bias through the UI
                        std::cout << "\n=== Step 6b: Agglomeration with Size Bias ===\n";
                        
                        // Access private widgets via findChild
                        auto* sizeBiasCB = ws->findChild<QCheckBox*>("agglomertionSizeBiasCheckBox");
                        if (sizeBiasCB) {
                            sizeBiasCB->setChecked(true);
                            std::cout << "UI: Enabled Size Bias check box\n";
                        } else {
                            std::cerr << "UI Error: Could not find agglomertionSizeBiasCheckBox\n";
                        }
                        
                        auto* sizeBiasStrategyCombo = ws->findChild<QComboBox*>("agglomertionSizeBiasStrategyComboBox");
                        if (sizeBiasStrategyCombo) {
                            int idx = sizeBiasStrategyCombo->findData(static_cast<int>(segment_puzzler::SizeBiasStrategy::SoftBiasAndCleanup));
                            if (idx != -1) {
                                sizeBiasStrategyCombo->setCurrentIndex(idx);
                                std::cout << "UI: Set Size Bias Strategy to SoftBiasAndCleanup\n";
                            }
                        }
                        
                        auto* sizeBiasThresholdSlider = ws->findChild<QSlider*>("agglomertionSizeBiasThresholdSlider");
                        if (sizeBiasThresholdSlider) {
                            sizeBiasThresholdSlider->setValue(5000);
                            std::cout << "UI: Set Size Bias Threshold to 5000\n";
                        }
                        
                        auto* sizeBiasThresholdSpinBox = ws->findChild<QSpinBox*>("agglomertionSizeBiasThresholdSpinBox");
                        if (sizeBiasThresholdSpinBox) {
                            sizeBiasThresholdSpinBox->setValue(5000);
                            std::cout << "UI: Set Size Bias Threshold SpinBox to 5000\n";
                        }
                        
                        auto* sizeBiasStrengthSlider = ws->findChild<QSlider*>("agglomertionSizeBiasStrengthSlider");
                        if (sizeBiasStrengthSlider) {
                            sizeBiasStrengthSlider->setValue(30); // 0.3
                            std::cout << "UI: Set Size Bias Strength to 0.3\n";
                        }
                        
                        auto* sizeBiasStrengthSpinBox = ws->findChild<QSpinBox*>("agglomertionSizeBiasStrengthSpinBox");
                        if (sizeBiasStrengthSpinBox) {
                             sizeBiasStrengthSpinBox->setValue(30);
                             std::cout << "UI: Set Size Bias Strength SpinBox to 30\n";
                        }

                        auto* sizeBiasProtectionSlider = ws->findChild<QSlider*>("agglomertionSizeBiasProtectionSlider");
                        if (sizeBiasProtectionSlider) {
                            sizeBiasProtectionSlider->setValue(0); // 0.0
                            std::cout << "UI: Set Size Bias Protection to 0.0\n";
                        }
                        
                        auto* sizeBiasProtectionSpinBox = ws->findChild<QSpinBox*>("agglomertionSizeBiasProtectionSpinBox");
                        if (sizeBiasProtectionSpinBox) {
                            sizeBiasProtectionSpinBox->setValue(0);
                            std::cout << "UI: Set Size Bias Protection SpinBox to 0\n";
                        }

                        auto* biasSlider = ws->findChild<QSlider*>("agglomertionBiasSlider");
                        if (biasSlider) {
                            biasSlider->setValue(100); // tau = 1.0
                            std::cout << "UI: Set Bias Slider to 100 (tau=1.0)\n";
                        }

                        double t6 = utils::tic("Step 6b: Agglomeration with Size Bias");
                        ws->agglomertionAsync([ws, t6, cutVoxelIndex]() {
                            utils::toc(t6, "Step 6b: Agglomeration with Size Bias done:");
                            if (ws->pAgglomertion.IsNull() || ws->pAgglomertionFragments.IsNull()) {
                                failTest("agglomeration outputs are missing");
                                return;
                            }
                            dataType::SegmentIdType badLabel = 0;
                            if (!nonZeroLabelsAreConnected(ws->pAgglomertion, &badLabel)) {
                                failTest("agglomeration display label is disconnected: " + std::to_string(badLabel));
                                return;
                            }
                            if (!imagesEqual(ws->pAgglomertion, ws->graphBase->pWorkingSegmentsImage)) {
                                failTest("graph working image does not match agglomeration display labels");
                                return;
                            }
                            const auto *agglomCanonical = ws->pAgglomertionFragments->GetBufferPointer();
                            const auto *agglomDisplay = ws->pAgglomertion->GetBufferPointer();
                            if (agglomCanonical[cutVoxelIndex] == 0) {
                                failTest("agglomeration canonical labels kept a zero-cut watershed voxel inactive");
                                return;
                            }
                            if (agglomDisplay[cutVoxelIndex] != 0) {
                                failTest("agglomeration display labels failed to keep the injected boundary cut");
                                return;
                            }

                            const uint64_t canonicalHash = imageHash(ws->pAgglomertionFragments);
                            auto *livePreviewCheckBox = findCheckBoxByText(ws, "Live Preview");
                            auto *injectBoundariesCheckBox = findCheckBoxByText(ws, "Inject Boundaries");
                            if (livePreviewCheckBox == nullptr || injectBoundariesCheckBox == nullptr) {
                                failTest("failed to locate agglomeration preview checkboxes");
                                return;
                            }

                            livePreviewCheckBox->setChecked(true);
                            injectBoundariesCheckBox->setChecked(false);

                            QTimer::singleShot(250, [ws, canonicalHash, injectBoundariesCheckBox]() {
                                if (imageHash(ws->pAgglomertionFragments) != canonicalHash) {
                                    failTest("canonical agglomeration labels changed after disabling Inject Boundaries");
                                    return;
                                }
                                if (ws->pAgglomertionPreviewSignal == nullptr || ws->pAgglomertionPreviewSignal->pImage.IsNull()) {
                                    failTest("agglomeration preview signal was not created");
                                    return;
                                }
                                const uint64_t previewWithoutBoundariesHash = imageHash(ws->pAgglomertionPreviewSignal->pImage);
                                injectBoundariesCheckBox->setChecked(true);

                                QTimer::singleShot(250, [ws, canonicalHash, previewWithoutBoundariesHash]() {
                                    if (imageHash(ws->pAgglomertionFragments) != canonicalHash) {
                                        failTest("canonical agglomeration labels changed after enabling Inject Boundaries");
                                        return;
                                    }
                                    if (ws->pAgglomertionPreviewSignal == nullptr || ws->pAgglomertionPreviewSignal->pImage.IsNull()) {
                                        failTest("agglomeration preview signal disappeared after enabling Inject Boundaries");
                                        return;
                                    }
                                    const uint64_t previewWithBoundariesHash = imageHash(ws->pAgglomertionPreviewSignal->pImage);
                                    if (previewWithBoundariesHash == previewWithoutBoundariesHash) {
                                        failTest("Inject Boundaries toggle did not change the preview image");
                                        return;
                                    }

                                    auto *replaceCheckBox = ws->findChild<QCheckBox *>("agglomertionReplaceCheckBox");
                                    auto *finalOutputCombo = ws->findChild<QComboBox *>("finalOutputInputComboBox");
                                    auto *replaceBiasSlider = ws->findChild<QSlider *>("agglomertionBiasSlider");
                                    if (replaceCheckBox == nullptr || finalOutputCombo == nullptr || replaceBiasSlider == nullptr) {
                                        failTest("failed to locate agglomeration replace controls");
                                        return;
                                    }
                                    if (!replaceCheckBox->isChecked()) {
                                        failTest("replace agglomeration checkbox is not enabled by default");
                                        return;
                                    }
                                    const int agglomCountBeforeReplace = finalOutputCombo->count();
                                    const int comboIndexBeforeReplace = finalOutputCombo->currentIndex();
                                    const int signalIndexBeforeReplace = finalOutputCombo->currentData().toInt();
                                    auto *signalBeforeReplace =
                                        (signalIndexBeforeReplace >= 0 &&
                                         signalIndexBeforeReplace < static_cast<int>(ws->allSignalList.size()))
                                            ? ws->allSignalList[static_cast<size_t>(signalIndexBeforeReplace)]
                                            : nullptr;
                                    const QString outputLabelBeforeReplace = finalOutputCombo->currentText();
                                    replaceCheckBox->setChecked(true);
                                    replaceBiasSlider->setValue(100);

                                    std::cout << "\n=== Step 6c: Agglomeration Replace ===\n";
                                    double t6b = utils::tic("Step 6c: Agglomeration Replace");
                                    ws->agglomertionAsync([ws, finalOutputCombo, agglomCountBeforeReplace,
                                                           comboIndexBeforeReplace, signalIndexBeforeReplace,
                                                           signalBeforeReplace, outputLabelBeforeReplace, t6b]() {
                                        utils::toc(t6b, "Step 6c: Agglomeration Replace done:");
                                        if (finalOutputCombo->count() != agglomCountBeforeReplace) {
                                            failTest("replace agglomeration created a new output instead of reusing the selected one");
                                            return;
                                        }
                                        if (finalOutputCombo->currentIndex() != comboIndexBeforeReplace) {
                                            failTest("replace agglomeration changed the selected output index");
                                            return;
                                        }
                                        if (finalOutputCombo->currentData().toInt() != signalIndexBeforeReplace) {
                                            failTest("replace agglomeration changed the selected signal instead of reusing it");
                                            return;
                                        }
                                        if (signalIndexBeforeReplace < 0 ||
                                            signalIndexBeforeReplace >= static_cast<int>(ws->allSignalList.size()) ||
                                            ws->allSignalList[static_cast<size_t>(signalIndexBeforeReplace)] != signalBeforeReplace) {
                                            failTest("replace agglomeration did not reuse the existing output signal object");
                                            return;
                                        }
                                        if (finalOutputCombo->currentText() != outputLabelBeforeReplace) {
                                            failTest("replace agglomeration unexpectedly changed the reused output label");
                                            return;
                                        }

                                        // Step 7: Post-Agglomeration Table Computation
                                        std::cout << "\n=== Step 7: Post-Agglomeration Table Computation ===\n";
                                        auto agglom = ws->pAgglomertion;
                                        if (agglom == nullptr) {
                                            failTest("no agglomeration result available for table computation");
                                            return;
                                        }

                                        SegmentTableDialog::FeatureFlags flags;
                                        flags.volume            = true;
                                        flags.physicalSize      = true;
                                        flags.pixelsOnBorder    = true;
                                        flags.perimeterOnBorder = true;
                                        flags.centroid          = true;
                                        flags.bbox              = true;
                                        flags.elongation        = true;
                                        flags.flatness          = true;
                                        flags.roundness         = true;
                                        flags.equivSphRadius    = true;
                                        flags.equivSphPerimeter = true;
                                        flags.equivEllipsoid    = true;
                                        flags.principalMoments  = true;
                                        flags.perimeter         = true;
                                        flags.orientedBBox      = true;

                                        double t7 = utils::tic("Step 7: Post-Agglomeration Table Computation");
                                        auto result = SegmentTableDialog::computeFeatures(agglom, flags);
                                        utils::toc(t7, "Step 7: Post-Agglomeration Table Computation done:");
                                        std::cout << "Computed features for " << result.rows.size() << " clusters.\n";

                                        auto dims = agglom->GetLargestPossibleRegion().GetSize();
                                        std::cout << "Volume dimensions: [" << dims[0] << ", " << dims[1] << ", " << dims[2] << "]\n";

                                        const uint64_t threshold = 5000;
                                        const auto isolationInfoByLabel = collectLabelIsolationInfo(agglom);
                                        int smallCount = 0;
                                        std::vector<dataType::SegmentIdType> nonIsolatedSmallLabels;
                                        std::cout << "\n--- Details of clusters smaller than " << threshold << " voxels ---\n";
                                        for (const auto &row : result.rows) {
                                            const auto isolationIt = isolationInfoByLabel.find(row.label);
                                            const bool isIsolated =
                                                isolationIt == isolationInfoByLabel.end() ||
                                                !isolationIt->second.touchesOtherNonZeroLabel;
                                            if (row.isIsolated != isIsolated) {
                                                failTest("segment feature table isolation flag disagrees with image-based isolation for label "
                                                         + std::to_string(row.label));
                                                return;
                                            }

                                            if (row.volume < threshold) {
                                                smallCount++;
                                                std::cout << "Cluster ID=" << row.label
                                                          << " volume=" << row.volume
                                                          << " centroid=(" << row.centroidX << ", " << row.centroidY << ", " << row.centroidZ << ")"
                                                          << " borderPixels=" << row.pixelsOnBorder
                                                          << " roundness=" << row.roundness
                                                          << " bbox=[" << row.bboxW << "x" << row.bboxH << "x" << row.bboxD << "]"
                                                          << " isolated=" << (isIsolated ? "yes" : "no");
                                                if (!isIsolated && isolationIt != isolationInfoByLabel.end()) {
                                                    std::cout << " touchingLabel=" << isolationIt->second.firstTouchingLabel;
                                                    nonIsolatedSmallLabels.push_back(row.label);
                                                }
                                                std::cout << "\n";
                                            }
                                        }
                                        std::cout << "Total number of clusters smaller than " << threshold << " voxels: " << smallCount << "\n";
                                        if (!nonIsolatedSmallLabels.empty()) {
                                            std::cout << "Non-isolated sub-threshold clusters remaining in the final display image:";
                                            for (dataType::SegmentIdType label : nonIsolatedSmallLabels) {
                                                std::cout << " " << label;
                                            }
                                            std::cout << "\n";
                                        } else if (smallCount > 0) {
                                            std::cout << "All sub-threshold clusters are isolated in the final display image.\n";
                                        }

                                        std::cout << "\n=== Full Pipeline complete ===\n";
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
