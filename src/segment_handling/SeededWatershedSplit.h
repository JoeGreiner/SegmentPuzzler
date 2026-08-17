#ifndef SEGMENTPUZZLER_SEEDEDWATERSHEDSPLIT_H
#define SEGMENTPUZZLER_SEEDEDWATERSHEDSPLIT_H

#include "src/file_definitions/dataTypes.h"
#include "src/utils/FastMarkerWatershed3D.h"
#include "src/utils/roi.h"

#include <itkImage.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace segment_puzzler {

using SeededSplitMaskImage = itk::Image<unsigned char, 3>;
using SeededSplitDistanceImage = itk::Image<float, 3>;
using SeededSplitMarkerImage = dataType::SegmentsImageType;
inline constexpr double kDefaultSeededSplitSmoothingSigmaPixels = 2.0;
inline constexpr double kDefaultSeededSplitCompactness =
    kDefaultFastMarkerWatershedCompactness;

struct SeededWatershedSplitSession {
    using IndexType = dataType::SegmentsImageType::IndexType;

    dataType::SegmentIdType sourceLabel = 0;
    std::uint64_t sourceModifiedTime = 0;
    std::size_t voxelCount = 0;
    std::size_t connectedComponentCount = 0;
    std::uint64_t maskHash = 0;
    std::uint64_t landscapeHash = 0;
    Roi sourceRoi;
    IndexType globalOffset{};
    SeededSplitMaskImage::Pointer mask;
    SeededSplitDistanceImage::Pointer distance;
    SeededSplitDistanceImage::Pointer landscape;
    float maximumDistance = 0.0f;
    double landscapeSmoothingSigmaPixels = 0.0;
    double maskAndConnectivityMs = 0.0;
    double distanceTransformMs = 0.0;
    double landscapeSmoothingMs = 0.0;
};

using SeededSplitSeedGroups =
    std::array<std::vector<SeededWatershedSplitSession::IndexType>, 2>;

struct SeededWatershedSplitOptions {
    double compactness = kDefaultSeededSplitCompactness;
    bool connectSeeds = false;
    bool allowDisconnectedParts = false;
};

struct SeededWatershedSplitResult {
    SeededSplitMarkerImage::Pointer markers;
    dataType::SegmentsImageType::Pointer partition;
    FastMarkerWatershedMetrics floodMetrics;
    std::uint64_t markerHash = 0;
    std::uint64_t partitionHash = 0;
    std::array<std::size_t, 2> markerVoxelCounts{0, 0};
    std::array<std::size_t, 2> connectionVoxelCounts{0, 0};
    std::array<std::size_t, 2> voxelCounts{0, 0};
    std::array<std::size_t, 2> connectedComponentCounts{0, 0};
    double compactness = 0.0;
    bool connectSeeds = false;
    bool disconnectedPartsAllowed = false;
    double markerConnectionMs = 0.0;
    double watershedMs = 0.0;
    std::string error;

    bool valid() const {
        return partition.IsNotNull() && error.empty();
    }

    bool hasDisconnectedParts() const {
        return connectedComponentCounts[0] > 1
               || connectedComponentCounts[1] > 1;
    }
};

SeededWatershedSplitSession prepareSeededWatershedSplit(
    dataType::SegmentsImageType::Pointer segments,
    dataType::SegmentIdType label);

void updateSeededWatershedSplitLandscape(
    SeededWatershedSplitSession &session,
    double smoothingSigmaPixels);

std::optional<SeededWatershedSplitSession::IndexType> seededSplitMaximumAlongWorldRay(
    const SeededWatershedSplitSession &session,
    const std::array<double, 3> &rayStartWorld,
    const std::array<double, 3> &rayEndWorld);

std::array<double, 3> seededSplitSeedWorldPoint(
    const SeededWatershedSplitSession &session,
    const SeededWatershedSplitSession::IndexType &localIndex);

SeededWatershedSplitResult computeSeededWatershedSplit(
    const SeededWatershedSplitSession &session,
    const SeededSplitSeedGroups &seedGroups,
    const SeededWatershedSplitOptions &options = {});

SeededWatershedSplitResult computeSeededWatershedSplit(
    const SeededWatershedSplitSession &session,
    const std::array<SeededWatershedSplitSession::IndexType, 2> &seedIndices,
    const SeededWatershedSplitOptions &options = {});

} // namespace segment_puzzler

#endif // SEGMENTPUZZLER_SEEDEDWATERSHEDSPLIT_H
