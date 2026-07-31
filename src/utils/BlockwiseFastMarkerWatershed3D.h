#ifndef SEGMENTPUZZLER_BLOCKWISEFASTMARKERWATERSHED3D_H
#define SEGMENTPUZZLER_BLOCKWISEFASTMARKERWATERSHED3D_H

#include "FastMarkerWatershed3D.h"

#include <cstddef>
#include <vector>

namespace segment_puzzler {

struct BlockwiseFastMarkerWatershedOptions {
    int threadCount = 1;
    int blockEdge = 0;
    int halo = 16;
    FastMarkerWatershedOptions watershed;
};

struct BlockwiseFastMarkerWatershedPassMetrics {
    bool red = true;
    std::size_t scheduledBlockCount = 0;
    std::size_t completedBlockCount = 0;
    std::size_t deferredBlockCount = 0;
    int threadsUsed = 0;
    double snapshotMs = 0.0;
    double elapsedMs = 0.0;
    double summedBlockMs = 0.0;
    double maxBlockMs = 0.0;
};

struct BlockwiseFastMarkerWatershedMetrics {
    int blockEdge = 0;
    std::size_t redBlockCount = 0;
    std::size_t blackBlockCount = 0;
    std::size_t deferredBlockCount = 0;
    std::size_t passCount = 0;
    double redMs = 0.0;
    double blackMs = 0.0;
    double elapsedMs = 0.0;
    bool usedGlobalFallback = false;
    std::vector<BlockwiseFastMarkerWatershedPassMetrics> passes;
};

int automaticWatershedBlockEdge(
    const FastMarkerWatershedCostImage::SizeType &imageSize,
    int threadCount);

// Approximate blockwise watershed: deterministic for a fixed block layout, but
// the result may depend on blockEdge/halo and need not match
// runFastMarkerWatershed3D.
FastMarkerWatershedLabelImage::Pointer runBlockwiseFastMarkerWatershed3D(
    FastMarkerWatershedCostImage::Pointer costImage,
    FastMarkerWatershedLabelImage::Pointer markers,
    const BlockwiseFastMarkerWatershedOptions &options = {},
    BlockwiseFastMarkerWatershedMetrics *metrics = nullptr);

} // namespace segment_puzzler

#endif
