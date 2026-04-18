#ifndef SEGMENTPUZZLER_FASTMARKERWATERSHED3D_H
#define SEGMENTPUZZLER_FASTMARKERWATERSHED3D_H

#include <itkImage.h>

#include <cstddef>

namespace segment_puzzler {

struct FastMarkerWatershedOptions {
    bool fullyConnected = false;
    bool markWatershedLine = false;
};

struct FastMarkerWatershedMetrics {
    double quantizeMs = 0.0;
    double initMs = 0.0;
    double floodMs = 0.0;
    double elapsedMs = 0.0;
    double seedFrontierMs = 0.0;
    double floodScanMs = 0.0;
    double floodPropagateMs = 0.0;
    double writebackMs = 0.0;
    std::size_t bucketBytes = 0;
    std::size_t scratchBytes = 0;
    std::size_t stalePopCount = 0;
    std::size_t popCount = 0;
    std::size_t requeueCount = 0;
    std::size_t uniqueQueuedVoxelCount = 0;
    std::size_t maxQueueDepth = 0;
    std::size_t maxBucketOccupancy = 0;
    std::size_t seedCount = 0;
    std::size_t finalizedVoxelCount = 0;
    std::size_t enqueuedVoxelCount = 0;
    std::size_t neighborCheckCount = 0;

    double stalePopRatio() const {
        return popCount == 0 ? 0.0 : static_cast<double>(stalePopCount) / static_cast<double>(popCount);
    }

    double avgEnqueuesPerVoxel() const {
        return uniqueQueuedVoxelCount == 0
            ? 0.0
            : static_cast<double>(enqueuedVoxelCount) / static_cast<double>(uniqueQueuedVoxelCount);
    }
};

using FastMarkerWatershedCostImage = itk::Image<float, 3>;
using FastMarkerWatershedLabelImage = itk::Image<unsigned int, 3>;

FastMarkerWatershedLabelImage::Pointer runFastMarkerWatershed3D(
    FastMarkerWatershedCostImage::Pointer costImage,
    FastMarkerWatershedLabelImage::Pointer markers,
    const FastMarkerWatershedOptions &options = {},
    FastMarkerWatershedMetrics *metrics = nullptr);

} // namespace segment_puzzler

#endif
