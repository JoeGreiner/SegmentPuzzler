#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QVariant>

#ifndef _WIN32
#include <cstdlib>
#endif

#include <iostream>
#include <stdexcept>
#include <cstdint>
#include <string>

#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapCliIO.h"

namespace {

constexpr const char *kBoundaryUrl =
    "https://drive.google.com/uc?export=download&id=1YMFz84x8_E4OVh74ABn8j2pcpGXKtu8r";
constexpr const char *kBoundaryFileName = "BoundaryPrediction.tif";
constexpr float kThreshold = 127.0f;
constexpr int kThreadCount = 1;
constexpr const char *kExpectedSha256 =
    "6d1f1498d5c27fc58d3449a92d60b953e79a3811a326408a1c6e722eeece38c7";

bool downloadFile(const QString &url, const QString &outputFilePath, int maxRedirects = 5) {
    if (QFile::exists(outputFilePath)) {
        std::cout << "Using cached file: " << outputFilePath.toStdString() << "\n";
        return true;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};

    for (int redirect = 0; redirect < maxRedirects; ++redirect) {
        QNetworkReply *reply = manager.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            std::cerr << "Download error: " << reply->errorString().toStdString() << "\n";
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
            std::cerr << "No data received.\n";
            return false;
        }

        QFile file(outputFilePath);
        if (!file.open(QIODevice::WriteOnly)) {
            std::cerr << "Cannot write to: " << outputFilePath.toStdString() << "\n";
            return false;
        }
        file.write(data);
        file.close();
        return true;
    }

    std::cerr << "Too many redirects.\n";
    return false;
}

QString ensureBoundaryFile() {
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString filePath = QDir(tempDir).filePath(kBoundaryFileName);
    return downloadFile(QString::fromLatin1(kBoundaryUrl), filePath) ? filePath : QString{};
}

std::string computeLabelHash(dataType::SegmentsImageType::Pointer image) {
    if (image.IsNull()) {
        throw std::runtime_error("Watershed output image is null.");
    }

    const auto voxelCount = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *labels = image->GetBufferPointer();
    QByteArray stableBytes;
    stableBytes.resize(static_cast<int>(voxelCount * sizeof(std::uint32_t)));
    char *out = stableBytes.data();
    for (size_t index = 0; index < voxelCount; ++index) {
        const std::uint32_t value = static_cast<std::uint32_t>(labels[index]);
        out[index * 4 + 0] = static_cast<char>(value & 0xFFu);
        out[index * 4 + 1] = static_cast<char>((value >> 8) & 0xFFu);
        out[index * 4 + 2] = static_cast<char>((value >> 16) & 0xFFu);
        out[index * 4 + 3] = static_cast<char>((value >> 24) & 0xFFu);
    }
    const QByteArray digest = QCryptographicHash::hash(
        stableBytes,
        QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

} // namespace

int main(int argc, char **argv) {
#ifndef _WIN32
    setenv("ITK_AUTOLOAD_PATH", "", 1);
#endif

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("SegmentPuzzler");
    QCoreApplication::setOrganizationName("JoeGreiner");

    try {
        const QString boundaryPath = ensureBoundaryFile();
        if (boundaryPath.isEmpty()) {
            throw std::runtime_error("Failed to obtain sample boundary file.");
        }

        auto boundaryImage = distance_map_benchmark::loadScalarImageAsFloat(boundaryPath.toStdString());
        auto thresholded = distance_map_benchmark::thresholdScalarImage(boundaryImage, kThreshold);

        distance_map_benchmark::DistanceImageType::Pointer distanceMap;
        generateDistanceMap(thresholded, distanceMap, 0.0, DistanceMapAlgorithm::Maurer, kThreadCount);

        distance_map_benchmark::DistanceImageType::Pointer invertedDistanceMap;
        invertDistanceMap(distanceMap, invertedDistanceMap);

        auto seeds = distance_map_benchmark::extractSeedsFromDistanceImage(
            distanceMap, distance_map_benchmark::SeedExtractorKind::LocalMaxima);

        WatershedRunOptions options;
        options.algorithm = WatershedAlgorithm::FastMarkerWatershed;

        dataType::SegmentsImageType::Pointer watershedImage;
        runWatershed(invertedDistanceMap, seeds, watershedImage, options);

        const std::string actualHash = computeLabelHash(watershedImage);
        if (actualHash != kExpectedSha256) {
            std::cerr << "Fast-marker sample hash mismatch.\n"
                      << "Expected: " << kExpectedSha256 << "\n"
                      << "Actual:   " << actualHash << "\n";
            return 1;
        }

        std::cout << "Fast-marker sample hash: " << actualHash << "\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "test_fast_marker_reference failed: " << exception.what() << "\n";
        return 1;
    }
}
