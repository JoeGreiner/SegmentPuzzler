#include "BlockwiseFastMarkerWatershed3D.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace segment_puzzler {
namespace {

using CostImage = FastMarkerWatershedCostImage;
using LabelImage = FastMarkerWatershedLabelImage;
using Region = CostImage::RegionType;

struct Block {
    Region core;
    Region outer;
};

double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::vector<Block> makeBlocks(const Region &imageRegion, int blockEdge, int halo, int color) {
    const auto imageIndex = imageRegion.GetIndex();
    const auto imageSize = imageRegion.GetSize();
    const auto blockCountX = (imageSize[0] + blockEdge - 1) / blockEdge;
    const auto blockCountY = (imageSize[1] + blockEdge - 1) / blockEdge;
    const auto blockCountZ = (imageSize[2] + blockEdge - 1) / blockEdge;

    std::vector<Block> blocks;
    blocks.reserve(static_cast<std::size_t>(blockCountX * blockCountY * blockCountZ / 2 + 1));
    for (std::size_t bz = 0; bz < blockCountZ; ++bz) {
        for (std::size_t by = 0; by < blockCountY; ++by) {
            for (std::size_t bx = 0; bx < blockCountX; ++bx) {
                if (static_cast<int>((bx + by + bz) % 2) != color) {
                    continue;
                }

                CostImage::IndexType coreIndex;
                CostImage::SizeType coreSize;
                CostImage::IndexType outerIndex;
                CostImage::SizeType outerSize;
                for (unsigned int axis = 0; axis < 3; ++axis) {
                    const std::size_t blockCoordinate = axis == 0 ? bx : (axis == 1 ? by : bz);
                    const auto offset = blockCoordinate * static_cast<std::size_t>(blockEdge);
                    coreIndex[axis] = imageIndex[axis] + static_cast<CostImage::IndexValueType>(offset);
                    coreSize[axis] = std::min<std::size_t>(blockEdge, imageSize[axis] - offset);

                    const auto imageBegin = imageIndex[axis];
                    const auto imageEnd = imageBegin + static_cast<CostImage::IndexValueType>(imageSize[axis]);
                    const auto coreEnd = coreIndex[axis] + static_cast<CostImage::IndexValueType>(coreSize[axis]);
                    outerIndex[axis] = std::max(
                        imageBegin, coreIndex[axis] - static_cast<CostImage::IndexValueType>(halo));
                    const auto outerEnd = std::min(
                        imageEnd, coreEnd + static_cast<CostImage::IndexValueType>(halo));
                    outerSize[axis] = static_cast<CostImage::SizeValueType>(outerEnd - outerIndex[axis]);
                }
                blocks.push_back({Region(coreIndex, coreSize), Region(outerIndex, outerSize)});
            }
        }
    }
    return blocks;
}

template <typename ImageType>
typename ImageType::Pointer allocateLocalImage(
    typename ImageType::Pointer source,
    const typename ImageType::RegionType &sourceRegion) {
    auto image = ImageType::New();
    typename ImageType::IndexType zeroIndex;
    zeroIndex.Fill(0);
    image->SetRegions(typename ImageType::RegionType(zeroIndex, sourceRegion.GetSize()));
    image->SetSpacing(source->GetSpacing());
    image->SetDirection(source->GetDirection());
    typename ImageType::PointType origin;
    source->TransformIndexToPhysicalPoint(sourceRegion.GetIndex(), origin);
    image->SetOrigin(origin);
    image->Allocate(true);
    return image;
}

std::size_t flatOffset(const CostImage::IndexType &index,
                       const CostImage::IndexType &imageIndex,
                       const CostImage::SizeType &imageSize) {
    const auto x = static_cast<std::size_t>(index[0] - imageIndex[0]);
    const auto y = static_cast<std::size_t>(index[1] - imageIndex[1]);
    const auto z = static_cast<std::size_t>(index[2] - imageIndex[2]);
    return (z * imageSize[1] + y) * imageSize[0] + x;
}

bool runBlock(const Block &block,
              CostImage::Pointer costImage,
              LabelImage::Pointer markers,
              LabelImage::Pointer priorLabels,
              LabelImage::Pointer output,
              const FastMarkerWatershedOptions &watershedOptions) {
    auto localCost = allocateLocalImage<CostImage>(costImage, block.outer);
    auto localMarkers = allocateLocalImage<LabelImage>(markers, block.outer);

    const auto imageRegion = costImage->GetLargestPossibleRegion();
    const auto imageIndex = imageRegion.GetIndex();
    const auto imageSize = imageRegion.GetSize();
    const auto outerIndex = block.outer.GetIndex();
    const auto outerSize = block.outer.GetSize();
    const float *cost = costImage->GetBufferPointer();
    const unsigned int *sourceMarkers = markers->GetBufferPointer();
    const unsigned int *priorLabelBuffer = priorLabels.IsNotNull() ? priorLabels->GetBufferPointer() : nullptr;
    float *localCostBuffer = localCost->GetBufferPointer();
    unsigned int *localMarkerBuffer = localMarkers->GetBufferPointer();

    bool hasMarker = false;
    std::size_t localOffset = 0;
    CostImage::IndexType sourceIndex;
    for (std::size_t z = 0; z < outerSize[2]; ++z) {
        sourceIndex[2] = outerIndex[2] + static_cast<CostImage::IndexValueType>(z);
        for (std::size_t y = 0; y < outerSize[1]; ++y) {
            sourceIndex[1] = outerIndex[1] + static_cast<CostImage::IndexValueType>(y);
            sourceIndex[0] = outerIndex[0];
            std::size_t sourceOffset = flatOffset(sourceIndex, imageIndex, imageSize);
            for (std::size_t x = 0; x < outerSize[0]; ++x, ++sourceOffset, ++localOffset) {
                localCostBuffer[localOffset] = cost[sourceOffset];
                unsigned int marker = sourceMarkers[sourceOffset];
                if (marker == 0 && priorLabelBuffer != nullptr) {
                    marker = priorLabelBuffer[sourceOffset];
                }
                localMarkerBuffer[localOffset] = marker;
                hasMarker = hasMarker || marker != 0;
            }
        }
    }

    if (!hasMarker) {
        return false;
    }

    auto localLabels = runFastMarkerWatershed3D(localCost, localMarkers, watershedOptions);
    const unsigned int *localLabelBuffer = localLabels->GetBufferPointer();
    unsigned int *outputBuffer = output->GetBufferPointer();
    const auto coreIndex = block.core.GetIndex();
    const auto coreSize = block.core.GetSize();
    const auto localCoreX = static_cast<std::size_t>(coreIndex[0] - outerIndex[0]);
    const auto localCoreY = static_cast<std::size_t>(coreIndex[1] - outerIndex[1]);
    const auto localCoreZ = static_cast<std::size_t>(coreIndex[2] - outerIndex[2]);

    CostImage::IndexType outputIndex;
    for (std::size_t z = 0; z < coreSize[2]; ++z) {
        outputIndex[2] = coreIndex[2] + static_cast<CostImage::IndexValueType>(z);
        for (std::size_t y = 0; y < coreSize[1]; ++y) {
            outputIndex[1] = coreIndex[1] + static_cast<CostImage::IndexValueType>(y);
            outputIndex[0] = coreIndex[0];
            const std::size_t outputOffset = flatOffset(outputIndex, imageIndex, imageSize);
            const std::size_t sourceOffset =
                ((localCoreZ + z) * outerSize[1] + localCoreY + y) * outerSize[0] + localCoreX;
            std::copy_n(localLabelBuffer + sourceOffset, coreSize[0], outputBuffer + outputOffset);
        }
    }
    return true;
}

struct PassResult {
    std::vector<Block> deferred;
    std::size_t completed = 0;
    BlockwiseFastMarkerWatershedPassMetrics metrics;
};

PassResult runPass(const std::vector<Block> &blocks,
                   CostImage::Pointer costImage,
                   LabelImage::Pointer markers,
                   LabelImage::Pointer priorLabels,
                   LabelImage::Pointer output,
                   const BlockwiseFastMarkerWatershedOptions &options) {
    const auto passStarted = std::chrono::steady_clock::now();
    std::exception_ptr firstException;
    std::mutex exceptionMutex;
    std::vector<unsigned char> completed(blocks.size(), 0);
    std::vector<double> blockTimesMs(blocks.size(), 0.0);
    std::vector<int> blockThreadIds(blocks.size(), 0);

#ifdef USE_OMP
#pragma omp parallel for schedule(dynamic, 1) num_threads(options.threadCount) if(blocks.size() > 1)
#endif
    for (long long i = 0; i < static_cast<long long>(blocks.size()); ++i) {
        const auto blockStarted = std::chrono::steady_clock::now();
#ifdef USE_OMP
        blockThreadIds[static_cast<std::size_t>(i)] = omp_get_thread_num();
#endif
        try {
            if (!runBlock(blocks[static_cast<std::size_t>(i)], costImage, markers, priorLabels,
                          output, options.watershed)) {
                blockTimesMs[static_cast<std::size_t>(i)] = elapsedMs(blockStarted);
                continue;
            }
            completed[static_cast<std::size_t>(i)] = 1;
        } catch (...) {
            std::lock_guard<std::mutex> guard(exceptionMutex);
            if (!firstException) {
                firstException = std::current_exception();
            }
        }
        blockTimesMs[static_cast<std::size_t>(i)] = elapsedMs(blockStarted);
    }

    if (firstException) {
        std::rethrow_exception(firstException);
    }

    PassResult result;
    result.metrics.scheduledBlockCount = blocks.size();
    result.deferred.reserve(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (completed[i] != 0) {
            ++result.completed;
        } else {
            result.deferred.push_back(blocks[i]);
        }
        result.metrics.summedBlockMs += blockTimesMs[i];
        result.metrics.maxBlockMs = std::max(result.metrics.maxBlockMs, blockTimesMs[i]);
    }
    result.metrics.completedBlockCount = result.completed;
    result.metrics.deferredBlockCount = result.deferred.size();
    result.metrics.elapsedMs = elapsedMs(passStarted);
    std::sort(blockThreadIds.begin(), blockThreadIds.end());
    result.metrics.threadsUsed = blocks.empty()
        ? 0
        : static_cast<int>(std::unique(blockThreadIds.begin(), blockThreadIds.end()) - blockThreadIds.begin());
    return result;
}

LabelImage::Pointer copyLabels(LabelImage::Pointer source) {
    auto copy = LabelImage::New();
    copy->CopyInformation(source);
    copy->SetRegions(source->GetLargestPossibleRegion());
    copy->Allocate();
    std::copy_n(source->GetBufferPointer(), source->GetLargestPossibleRegion().GetNumberOfPixels(),
                copy->GetBufferPointer());
    return copy;
}

} // namespace

int automaticWatershedBlockEdge(const FastMarkerWatershedCostImage::SizeType &imageSize,
                                 int threadCount) {
    const double voxelCount = static_cast<double>(imageSize[0]) *
                              static_cast<double>(imageSize[1]) *
                              static_cast<double>(imageSize[2]);
    const double targetVoxels = voxelCount / (2.0 * std::max(1, threadCount));
    const int edge = static_cast<int>(std::cbrt(std::max(1.0, targetVoxels)));
    const int roundedDown = std::max(32, (edge / 32) * 32);
    return std::clamp(roundedDown, 128, 256);
}

FastMarkerWatershedLabelImage::Pointer runBlockwiseFastMarkerWatershed3D(
    FastMarkerWatershedCostImage::Pointer costImage,
    FastMarkerWatershedLabelImage::Pointer markers,
    const BlockwiseFastMarkerWatershedOptions &options,
    BlockwiseFastMarkerWatershedMetrics *metrics) {
    if (costImage.IsNull() || markers.IsNull()) {
        throw std::invalid_argument("Blockwise fast marker watershed requires cost and marker images.");
    }
    if (options.threadCount <= 0 || options.blockEdge < 0 || options.halo < 0) {
        throw std::invalid_argument("Thread count must be positive; block edge and halo must be non-negative.");
    }
    if (costImage->GetLargestPossibleRegion() != markers->GetLargestPossibleRegion()) {
        throw std::invalid_argument("Cost and marker image regions must match.");
    }

    BlockwiseFastMarkerWatershedMetrics localMetrics;
    const auto started = std::chrono::steady_clock::now();
    const auto imageRegion = costImage->GetLargestPossibleRegion();
    const int blockEdge = options.blockEdge > 0
        ? options.blockEdge
        : automaticWatershedBlockEdge(imageRegion.GetSize(), options.threadCount);
    localMetrics.blockEdge = blockEdge;

    const auto redBlocks = makeBlocks(imageRegion, blockEdge, options.halo, 0);
    const auto blackBlocks = makeBlocks(imageRegion, blockEdge, options.halo, 1);
    localMetrics.redBlockCount = redBlocks.size();
    localMetrics.blackBlockCount = blackBlocks.size();

    auto output = LabelImage::New();
    output->CopyInformation(markers);
    output->SetRegions(imageRegion);
    output->Allocate(true);

    const auto redStarted = std::chrono::steady_clock::now();
    PassResult redResult = runPass(redBlocks, costImage, markers, nullptr, output, options);
    redResult.metrics.red = true;
    localMetrics.passes.push_back(redResult.metrics);
    ++localMetrics.passCount;
    localMetrics.deferredBlockCount += redResult.deferred.size();
    localMetrics.redMs = elapsedMs(redStarted);

    const auto blackStarted = std::chrono::steady_clock::now();
    const auto blackSnapshotStarted = std::chrono::steady_clock::now();
    auto blackPriorLabels = copyLabels(output);
    const double blackSnapshotMs = elapsedMs(blackSnapshotStarted);
    PassResult blackResult = runPass(blackBlocks, costImage, markers, blackPriorLabels, output, options);
    blackResult.metrics.red = false;
    blackResult.metrics.snapshotMs = blackSnapshotMs;
    localMetrics.passes.push_back(blackResult.metrics);
    ++localMetrics.passCount;
    localMetrics.deferredBlockCount += blackResult.deferred.size();
    localMetrics.blackMs = elapsedMs(blackStarted);

    while (!redResult.deferred.empty() || !blackResult.deferred.empty()) {
        std::size_t completedThisRound = 0;
        if (!redResult.deferred.empty()) {
            const auto passStarted = std::chrono::steady_clock::now();
            const auto snapshotStarted = std::chrono::steady_clock::now();
            auto priorLabels = copyLabels(output);
            const double snapshotMs = elapsedMs(snapshotStarted);
            redResult = runPass(redResult.deferred, costImage, markers, priorLabels, output, options);
            redResult.metrics.red = true;
            redResult.metrics.snapshotMs = snapshotMs;
            localMetrics.passes.push_back(redResult.metrics);
            localMetrics.redMs += elapsedMs(passStarted);
            ++localMetrics.passCount;
            localMetrics.deferredBlockCount += redResult.deferred.size();
            completedThisRound += redResult.completed;
        }
        if (!blackResult.deferred.empty()) {
            const auto passStarted = std::chrono::steady_clock::now();
            const auto snapshotStarted = std::chrono::steady_clock::now();
            auto priorLabels = copyLabels(output);
            const double snapshotMs = elapsedMs(snapshotStarted);
            blackResult = runPass(blackResult.deferred, costImage, markers, priorLabels, output, options);
            blackResult.metrics.red = false;
            blackResult.metrics.snapshotMs = snapshotMs;
            localMetrics.passes.push_back(blackResult.metrics);
            localMetrics.blackMs += elapsedMs(passStarted);
            ++localMetrics.passCount;
            localMetrics.deferredBlockCount += blackResult.deferred.size();
            completedThisRound += blackResult.completed;
        }
        if (completedThisRound != 0) {
            continue;
        }
        localMetrics.usedGlobalFallback = true;
        output = runFastMarkerWatershed3D(costImage, markers, options.watershed);
        break;
    }

    localMetrics.elapsedMs = elapsedMs(started);
    if (metrics != nullptr) {
        *metrics = localMetrics;
    }
    return output;
}

} // namespace segment_puzzler
