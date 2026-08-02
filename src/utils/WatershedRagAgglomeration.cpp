#include "src/utils/WatershedRagAgglomeration.h"
#include "src/utils/RegionAdjacencyGraph.h"
#include "src/utils/RegionMerger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace segment_puzzler {
namespace {

using SegmentIdType = dataType::SegmentIdType;
using SegmentsImageType = dataType::SegmentsImageType;

std::mutex &agglomerationLogMutex() {
    static std::mutex mutex;
    return mutex;
}

AgglomerationLogSink &agglomerationLogSinkStorage() {
    static AgglomerationLogSink sink;
    return sink;
}

void emitAgglomerationLog(const std::string &message) {
    std::lock_guard<std::mutex> guard(agglomerationLogMutex());
    if (const auto &sink = agglomerationLogSinkStorage()) {
        sink(message);
        return;
    }
    std::cout << message << std::endl;
}

double currentTimeSeconds() {
#ifdef USE_OMP
    return omp_get_wtime();
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

double elapsedMilliseconds(double startTimeSeconds) {
    return (currentTimeSeconds() - startTimeSeconds) * 1000.0;
}

void updateBoundaryRange(double value, double &boundaryMin, double &boundaryMax) {
    if (!std::isfinite(value)) {
        return;
    }
    boundaryMin = std::min(boundaryMin, value);
    boundaryMax = std::max(boundaryMax, value);
}

void logElapsedMilliseconds(double elapsedMs, const char *description) {
    std::ostringstream stream;
    stream << description << ' ' << elapsedMs / 1000.0;
    emitAgglomerationLog(stream.str());
}

void logStepTime(double startTimeSeconds, const char *description) {
    logElapsedMilliseconds(elapsedMilliseconds(startTimeSeconds), description);
}

int effectiveThreadCount(const WatershedRagAgglomerationOptions &options) {
#ifdef USE_OMP
    if (options.threadCount > 0) {
        omp_set_num_threads(options.threadCount);
        return options.threadCount;
    }
    return std::max(1, omp_get_max_threads());
#else
    (void)options;
    return 1;
#endif
}

BoundaryNormalizationMode resolveBoundaryNormalizationMode(double minValue, double maxValue,
                                                           BoundaryNormalizationMode requestedMode) {
    if (requestedMode != BoundaryNormalizationMode::AutoDetect) {
        return requestedMode;
    }
    if (maxValue <= 1.0 && minValue >= 0.0) {
        return BoundaryNormalizationMode::ProbabilityZeroToOne;
    }
    if (maxValue <= 2.0 && minValue >= 0.0) {
        return BoundaryNormalizationMode::ProbabilityZeroToTwo;
    }
    if (maxValue <= 255.0 && minValue >= 0.0) {
        return BoundaryNormalizationMode::UInt8FullRange;
    }
    return BoundaryNormalizationMode::UInt16FullRange;
}

double normalizeBoundaryValue(float rawValue, BoundaryNormalizationMode mode) {
    double normalized = 0.0;
    switch (mode) {
        case BoundaryNormalizationMode::AutoDetect:
        case BoundaryNormalizationMode::ProbabilityZeroToOne:
            normalized = static_cast<double>(rawValue);
            break;
        case BoundaryNormalizationMode::ProbabilityZeroToTwo:
            normalized = static_cast<double>(rawValue) * 0.5;
            break;
        case BoundaryNormalizationMode::UInt8FullRange:
            normalized = static_cast<double>(rawValue) / 255.0;
            break;
        case BoundaryNormalizationMode::UInt16FullRange:
            normalized = static_cast<double>(rawValue) /
                         static_cast<double>(std::numeric_limits<uint16_t>::max());
            break;
    }
    return std::clamp(normalized, 0.0, 1.0);
}

bool sizeBiasCleanupEnabled(const WatershedRagAgglomerationOptions &options) {
    return options.sizeBiasStrategy == SizeBiasStrategy::Cleanup ||
           options.sizeBiasStrategy == SizeBiasStrategy::SoftBiasAndCleanup;
}

template <typename TImage>
typename TImage::Pointer allocateImageLike(typename TImage::Pointer reference) {
    auto image = TImage::New();
    image->SetRegions(reference->GetLargestPossibleRegion());
    image->SetSpacing(reference->GetSpacing());
    image->SetOrigin(reference->GetOrigin());
    image->SetDirection(reference->GetDirection());
    image->Allocate();
    return image;
}

void printStats(const WatershedRagAgglomerationStats &stats) {
    std::ostringstream stream;
    stream << "WatershedRagAgglomeration:"
           << " fragments=" << stats.inputFragmentCount
           << " edges=" << stats.ragEdgeCount
           << " merges=" << stats.mergeCount
           << " output_clusters=" << stats.outputClusterCount
           << " initial_small_clusters=" << stats.initialSmallClusterCount
           << " final_small_clusters=" << stats.finalSmallClusterCount
           << " cleanup_merges=" << stats.sizeBiasCleanupMergeCount
           << " final_cleanup_eligible_small_clusters=" << stats.finalCleanupEligibleSmallClusterCount
           << " policy=" << agglomerationExecutionPolicyName(stats.executionPolicyUsed)
           << " batches=" << stats.batchCount
           << " max_batch_pairs=" << stats.maxBatchPairs
           << " region_index_ms=" << stats.regionIndexBuildMs
           << " rag_ms=" << stats.ragBuildMs
           << " merge_queue_init_ms=" << stats.mergeQueueInitializationMs
           << " agglomeration_ms=" << stats.agglomerationMs
           << " batch_select_ms=" << stats.batchSelectionMs
           << " batch_region_merge_ms=" << stats.batchRegionMergeMs
           << " batch_merge_score_update_ms=" << stats.batchMergeScoreUpdateMs
           << " batch_apply_ms=" << stats.batchApplyMs
           << " outdated_merge_candidates=" << stats.outdatedMergeCandidateCount
           << " updated_rag_edges=" << stats.updatedRagEdgeCount
           << " projection_ms=" << stats.projectionMs
           << " boundary_min=" << stats.boundaryMin
           << " boundary_max=" << stats.boundaryMax
           << " boundary_scale=" << boundaryNormalizationModeName(stats.resolvedBoundaryNormalization);
    emitAgglomerationLog(stream.str());
}

template <typename IsActiveVoxel>
struct AgglomerationContext {
    SegmentsImageType::Pointer labels;
    BoundaryFloatImageType::Pointer boundary;
    BoundaryMaskImageType::Pointer thresholdMask;
    const WatershedRagAgglomerationOptions &options;
    IsActiveVoxel isActiveVoxel;
    WatershedRagAgglomerationStats stats;
    std::vector<rag::RegionId> regionIdByVoxel;
    rag::RegionAdjacencyGraph graph;
    BoundaryNormalizationMode resolvedBoundaryMode = BoundaryNormalizationMode::AutoDetect;
    size_t dimX = 0;
    size_t dimY = 0;
    size_t dimZ = 0;
    size_t voxelCount = 0;
    const SegmentIdType *labelBuffer = nullptr;
    const float *boundaryBuffer = nullptr;
    const unsigned char *thresholdMaskBuffer = nullptr;
};

template <typename IsActiveVoxel>
void computeBoundaryRange(AgglomerationContext<IsActiveVoxel> &ctx) {
    double boundaryMin = std::numeric_limits<double>::infinity();
    double boundaryMax = -std::numeric_limits<double>::infinity();
    const int threadCount = effectiveThreadCount(ctx.options);
#ifdef USE_OMP
    std::vector<double> threadMinimums(static_cast<size_t>(threadCount), boundaryMin);
    std::vector<double> threadMaximums(static_cast<size_t>(threadCount), boundaryMax);
#pragma omp parallel num_threads(threadCount)
    {
        double localMin = std::numeric_limits<double>::infinity();
        double localMax = -std::numeric_limits<double>::infinity();
#pragma omp for nowait
        for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
            updateBoundaryRange(static_cast<double>(ctx.boundaryBuffer[index]), localMin, localMax);
        }
        const int threadId = omp_get_thread_num();
        threadMinimums[static_cast<size_t>(threadId)] = localMin;
        threadMaximums[static_cast<size_t>(threadId)] = localMax;
    }
    for (int threadId = 0; threadId < threadCount; ++threadId) {
        boundaryMin = std::min(boundaryMin, threadMinimums[static_cast<size_t>(threadId)]);
        boundaryMax = std::max(boundaryMax, threadMaximums[static_cast<size_t>(threadId)]);
    }
#else
    for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
        updateBoundaryRange(static_cast<double>(ctx.boundaryBuffer[index]), boundaryMin, boundaryMax);
    }
#endif
    if (!std::isfinite(boundaryMin) || !std::isfinite(boundaryMax)) {
        boundaryMin = 0.0;
        boundaryMax = 0.0;
    }
    ctx.resolvedBoundaryMode =
        resolveBoundaryNormalizationMode(boundaryMin, boundaryMax, ctx.options.boundaryNormalization);
    ctx.stats.boundaryMin = boundaryMin;
    ctx.stats.boundaryMax = boundaryMax;
    ctx.stats.resolvedBoundaryNormalization = ctx.resolvedBoundaryMode;
}

template <typename IsActiveVoxel>
void buildRegionIndex(AgglomerationContext<IsActiveVoxel> &ctx) {
    double t = currentTimeSeconds();
    ctx.regionIdByVoxel.assign(ctx.voxelCount, rag::invalidRegionId);

    SegmentIdType maxLabel = 0;
    std::size_t activeNonZeroCount = 0;
    const int threadCount = effectiveThreadCount(ctx.options);
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount) reduction(max:maxLabel) reduction(+:activeNonZeroCount)
#endif
    for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
        if (!ctx.isActiveVoxel(static_cast<size_t>(index))) {
            continue;
        }
        const SegmentIdType label = ctx.labelBuffer[index];
        if (label == 0) {
            continue;
        }
        maxLabel = std::max(maxLabel, label);
        ++activeNonZeroCount;
    }

    const bool useDenseMap = activeNonZeroCount > 0 &&
                             static_cast<uint64_t>(maxLabel) <= static_cast<uint64_t>(activeNonZeroCount) * 4ull;

    if (useDenseMap) {
        std::vector<rag::RegionId> labelToRegionId(
            static_cast<size_t>(maxLabel) + 1,
            rag::invalidRegionId);
        for (size_t index = 0; index < ctx.voxelCount; ++index) {
            if (!ctx.isActiveVoxel(index)) {
                continue;
            }
            const SegmentIdType label = ctx.labelBuffer[index];
            // Label 0 is reserved for watershed/boundary/background voxels and is never turned into a fragment node.
            if (label == 0) {
                continue;
            }
            if (labelToRegionId[label] == rag::invalidRegionId) {
                labelToRegionId[label] = ctx.graph.addRegion();
            }
            const rag::RegionId regionId = labelToRegionId[label];
            ctx.regionIdByVoxel[index] = regionId;
            ++ctx.graph.region(regionId).voxelCount;
        }
    } else {
        std::unordered_map<SegmentIdType, rag::RegionId> labelToRegionId;
        labelToRegionId.reserve(activeNonZeroCount / 4 + 16);
        for (size_t index = 0; index < ctx.voxelCount; ++index) {
            if (!ctx.isActiveVoxel(index)) {
                continue;
            }
            const SegmentIdType label = ctx.labelBuffer[index];
            // Label 0 is reserved for watershed/boundary/background voxels and is never turned into a fragment node.
            if (label == 0) {
                continue;
            }
            auto insertion = labelToRegionId.emplace(label, static_cast<rag::RegionId>(ctx.graph.regionCount()));
            if (insertion.second) {
                ctx.graph.addRegion();
            }
            const rag::RegionId regionId = insertion.first->second;
            ctx.regionIdByVoxel[index] = regionId;
            ++ctx.graph.region(regionId).voxelCount;
        }
    }

    ctx.stats.inputFragmentCount = ctx.graph.regionCount();
    ctx.stats.regionIndexBuildMs = elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration build region index done:");
}

template <typename IsActiveVoxel>
void buildInitialRegionAdjacencyGraph(AgglomerationContext<IsActiveVoxel> &ctx) {
    double t = currentTimeSeconds();
    const int threadCount = effectiveThreadCount(ctx.options);
    const double spacingX = ctx.labels->GetSpacing()[0];
    const double spacingY = ctx.labels->GetSpacing()[1];
    const double spacingZ = ctx.labels->GetSpacing()[2];
    const double faceAreaX = ctx.options.usePhysicalFaceArea ? spacingY * spacingZ : 1.0;
    const double faceAreaY = ctx.options.usePhysicalFaceArea ? spacingX * spacingZ : 1.0;
    const double faceAreaZ = ctx.options.usePhysicalFaceArea ? spacingX * spacingY : 1.0;

    std::vector<std::unordered_map<uint64_t, rag::RagEdgeStats>> threadMaps(static_cast<size_t>(threadCount));
    for (auto &threadMap : threadMaps) {
        threadMap.reserve(std::max<std::size_t>(32, ctx.stats.inputFragmentCount / std::max(1, threadCount)));
    }

#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount) schedule(static)
#endif
    for (long long z = 0; z < static_cast<long long>(ctx.dimZ); ++z) {
#ifdef USE_OMP
        auto &ragStats = threadMaps[static_cast<size_t>(omp_get_thread_num())];
#else
        auto &ragStats = threadMaps[0];
#endif
        for (size_t y = 0; y < ctx.dimY; ++y) {
            for (size_t x = 0; x < ctx.dimX; ++x) {
                const size_t index = (static_cast<size_t>(z) * ctx.dimY * ctx.dimX) + (y * ctx.dimX) + x;
                if (!ctx.isActiveVoxel(index)) {
                    continue;
                }

                const auto accumulateFace = [&](size_t firstVoxelIndex,
                                                size_t secondVoxelIndex,
                                                double contactArea) {
                    if (!ctx.isActiveVoxel(secondVoxelIndex)) {
                        return;
                    }
                    const rag::RegionId firstRegionId = ctx.regionIdByVoxel[firstVoxelIndex];
                    const rag::RegionId secondRegionId = ctx.regionIdByVoxel[secondVoxelIndex];
                    if (firstRegionId == rag::invalidRegionId ||
                        secondRegionId == rag::invalidRegionId ||
                        firstRegionId == secondRegionId) {
                        return;
                    }
                    const double firstBoundary =
                        normalizeBoundaryValue(ctx.boundaryBuffer[firstVoxelIndex], ctx.resolvedBoundaryMode);
                    const double secondBoundary =
                        normalizeBoundaryValue(ctx.boundaryBuffer[secondVoxelIndex], ctx.resolvedBoundaryMode);
                    rag::RagEdgeStats &edgeStats =
                        ragStats[rag::makeRegionPairKey(firstRegionId, secondRegionId)];
                    const double boundaryArea = 0.5 * (firstBoundary + secondBoundary) * contactArea;
                    edgeStats.totalBoundarySum += boundaryArea;
                    edgeStats.totalContactArea += contactArea;
                    const bool isOpenFace = ctx.thresholdMaskBuffer == nullptr ||
                                            (ctx.thresholdMaskBuffer[firstVoxelIndex] == 0 &&
                                             ctx.thresholdMaskBuffer[secondVoxelIndex] == 0);
                    if (isOpenFace) {
                        edgeStats.openBoundarySum += boundaryArea;
                        edgeStats.openContactArea += contactArea;
                    }
                };

                if (x + 1 < ctx.dimX) {
                    accumulateFace(index, index + 1, faceAreaX);
                }
                if (y + 1 < ctx.dimY) {
                    accumulateFace(index, index + ctx.dimX, faceAreaY);
                }
                if (static_cast<size_t>(z) + 1 < ctx.dimZ) {
                    accumulateFace(index, index + (ctx.dimX * ctx.dimY), faceAreaZ);
                }
            }
        }
    }

    std::unordered_map<uint64_t, rag::RagEdgeStats> ragStats;
    ragStats.reserve(std::max<std::size_t>(32, ctx.stats.inputFragmentCount * 4));
    for (const auto &threadMap : threadMaps) {
        for (const auto &entry : threadMap) {
            ragStats[entry.first].add(entry.second);
        }
    }

    std::vector<uint64_t> keys;
    keys.reserve(ragStats.size());
    for (const auto &entry : ragStats) {
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());

    ctx.graph.reserveEdges(keys.size());
    for (uint64_t key : keys) {
        const auto regionIds = rag::unpackRegionPairKey(key);
        ctx.graph.addEdge(regionIds.first, regionIds.second, ragStats.at(key));
    }

    ctx.stats.ragEdgeCount = ctx.graph.storedEdgeCount();
    ctx.stats.ragBuildMs = elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration build rag done:");
}

template <typename IsActiveVoxel>
void runRegionMerging(AgglomerationContext<IsActiveVoxel> &ctx,
                      rag::RegionMerger &regionMerger,
                      bool previewMode) {
    const bool useOmpBatching =
#ifdef USE_OMP
        ctx.options.sizeBiasStrategy == SizeBiasStrategy::Off &&
        !previewMode &&
        effectiveThreadCount(ctx.options) > 1 &&
        ctx.stats.ragEdgeCount >= ctx.options.parallelMergeEdgeThreshold &&
        ctx.options.executionPolicy != AgglomerationExecutionPolicy::Serial;
#else
        false;
#endif

    if (ctx.options.executionPolicy == AgglomerationExecutionPolicy::Serial || !useOmpBatching) {
        ctx.stats.executionPolicyUsed = AgglomerationExecutionPolicy::Serial;
        regionMerger.mergeGreedily();
        return;
    }

    ctx.stats.executionPolicyUsed = AgglomerationExecutionPolicy::OmpBatched;
    regionMerger.mergeInNonOverlappingBatches();
}

template <typename IsActiveVoxel>
void writeAgglomeratedLabels(AgglomerationContext<IsActiveVoxel> &ctx,
                             WatershedRagAgglomerationResult &result) {
    double t = currentTimeSeconds();
    result.agglomeratedLabels = allocateImageLike<SegmentsImageType>(ctx.labels);
    SegmentIdType *outputBuffer = result.agglomeratedLabels->GetBufferPointer();

    std::vector<rag::RegionId> activeRegionIds;
    activeRegionIds.reserve(ctx.graph.regionCount());
    for (rag::RegionId regionId = 0; regionId < static_cast<rag::RegionId>(ctx.graph.regionCount()); ++regionId) {
        if (ctx.graph.isActiveRegion(regionId)) {
            activeRegionIds.push_back(regionId);
        }
    }
    std::vector<SegmentIdType> outputLabelByRegionId(ctx.graph.regionCount(), 0);
    SegmentIdType nextLabel = 1;
    for (rag::RegionId regionId : activeRegionIds) {
        outputLabelByRegionId[static_cast<std::size_t>(regionId)] = nextLabel++;
    }

    const int threadCount = effectiveThreadCount(ctx.options);
    const rag::RegionAdjacencyGraph &readOnlyGraph = ctx.graph;
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount)
#endif
    for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
        if (!ctx.isActiveVoxel(static_cast<size_t>(index))) {
            outputBuffer[index] = 0;
            continue;
        }
        const rag::RegionId initialRegionId = ctx.regionIdByVoxel[static_cast<size_t>(index)];
        if (initialRegionId == rag::invalidRegionId) {
            outputBuffer[index] = 0;
            continue;
        }
        const rag::RegionId regionIdAfterMerges =
            readOnlyGraph.findRegionIdAfterAppliedMerges(initialRegionId);
        outputBuffer[index] = outputLabelByRegionId[static_cast<std::size_t>(regionIdAfterMerges)];
    }

    ctx.stats.outputClusterCount = activeRegionIds.size();
    ctx.stats.projectionMs = elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration projection done:");
}

template <typename IsActiveVoxel>
WatershedRagAgglomerationResult runWatershedRagAgglomerationImpl(
    SegmentsImageType::Pointer labels,
    BoundaryFloatImageType::Pointer boundary,
    BoundaryMaskImageType::Pointer thresholdMask,
    const WatershedRagAgglomerationOptions &options,
    IsActiveVoxel isActiveVoxel,
    bool previewMode) {
    if (labels.IsNull() || boundary.IsNull()) {
        throw std::invalid_argument("Watershed RAG agglomeration requires labels and boundary images.");
    }
    if (labels->GetLargestPossibleRegion().GetSize() != boundary->GetLargestPossibleRegion().GetSize()) {
        throw std::invalid_argument("Watershed RAG agglomeration requires matching label and boundary shapes.");
    }
    if (thresholdMask.IsNotNull() &&
        labels->GetLargestPossibleRegion().GetSize() != thresholdMask->GetLargestPossibleRegion().GetSize()) {
        throw std::invalid_argument("Watershed RAG agglomeration requires matching label and threshold-mask shapes.");
    }
    if (options.boundaryEvidenceStrategy != BoundaryEvidenceStrategy::RawInterfaceMean && thresholdMask.IsNull()) {
        throw std::invalid_argument("Threshold-aware agglomeration strategies require a threshold mask.");
    }

    AgglomerationContext<IsActiveVoxel> ctx{labels, boundary, thresholdMask, options, isActiveVoxel};
    const auto size = labels->GetLargestPossibleRegion().GetSize();
    ctx.dimX = size[0];
    ctx.dimY = size[1];
    ctx.dimZ = size[2];
    ctx.voxelCount = ctx.dimX * ctx.dimY * ctx.dimZ;
    ctx.labelBuffer = labels->GetBufferPointer();
    ctx.boundaryBuffer = boundary->GetBufferPointer();
    ctx.thresholdMaskBuffer = thresholdMask.IsNotNull() ? thresholdMask->GetBufferPointer() : nullptr;

    emitAgglomerationLog("WatershedRagAgglomeration build region index");
    computeBoundaryRange(ctx);
    buildRegionIndex(ctx);

    emitAgglomerationLog("WatershedRagAgglomeration build rag");
    buildInitialRegionAdjacencyGraph(ctx);

    const int threadCount = effectiveThreadCount(ctx.options);
    rag::RegionMerger regionMerger(ctx.graph, ctx.options, ctx.stats, threadCount);
    ctx.stats.initialSmallClusterCount = regionMerger.countSmallRegions();

    emitAgglomerationLog("WatershedRagAgglomeration initialize merge queue");
    const double queueInitializationStart = currentTimeSeconds();
    regionMerger.initializeMergeQueue();
    ctx.stats.mergeQueueInitializationMs = elapsedMilliseconds(queueInitializationStart);
    logStepTime(queueInitializationStart, "WatershedRagAgglomeration initialize merge queue done:");

    emitAgglomerationLog("WatershedRagAgglomeration agglomeration");
    const double agglomerationStart = currentTimeSeconds();
    runRegionMerging(ctx, regionMerger, previewMode);
    regionMerger.mergeRemainingSmallRegions();
    ctx.stats.agglomerationMs += elapsedMilliseconds(agglomerationStart);
    logStepTime(agglomerationStart, "WatershedRagAgglomeration agglomeration done:");

    ctx.stats.finalSmallClusterCount = regionMerger.countSmallRegions();
    ctx.stats.finalCleanupEligibleSmallClusterCount =
        sizeBiasCleanupEnabled(ctx.options) ? regionMerger.countSmallRegionsEligibleForCleanup() : 0;

    WatershedRagAgglomerationResult result;
    result.stats = ctx.stats;
    emitAgglomerationLog("WatershedRagAgglomeration projection");
    writeAgglomeratedLabels(ctx, result);
    result.stats = ctx.stats;
    printStats(result.stats);
    return result;
}

} // namespace

void setAgglomerationLogSink(AgglomerationLogSink sink) {
    std::lock_guard<std::mutex> guard(agglomerationLogMutex());
    agglomerationLogSinkStorage() = std::move(sink);
}

const char *ragLinkageName(RagLinkage linkage) {
    switch (linkage) {
        case RagLinkage::Average:
            return "average";
        case RagLinkage::Sum:
            return "sum";
    }
    return "average";
}

const char *boundaryNormalizationModeName(BoundaryNormalizationMode mode) {
    switch (mode) {
        case BoundaryNormalizationMode::AutoDetect:
            return "auto-detect";
        case BoundaryNormalizationMode::ProbabilityZeroToOne:
            return "probability-0-1";
        case BoundaryNormalizationMode::ProbabilityZeroToTwo:
            return "probability-0-2";
        case BoundaryNormalizationMode::UInt8FullRange:
            return "uint8-full-range";
        case BoundaryNormalizationMode::UInt16FullRange:
            return "uint16-full-range";
    }
    return "uint16-full-range";
}

const char *boundaryEvidenceStrategyName(BoundaryEvidenceStrategy strategy) {
    switch (strategy) {
        case BoundaryEvidenceStrategy::RawInterfaceMean:
            return "raw-mean";
        case BoundaryEvidenceStrategy::OpenInterfaceMean:
            return "open-mean";
        case BoundaryEvidenceStrategy::OpenFractionWeighted:
            return "open-fraction-weighted";
    }
    return "open-fraction-weighted";
}

const char *agglomerationExecutionPolicyName(AgglomerationExecutionPolicy policy) {
    switch (policy) {
        case AgglomerationExecutionPolicy::Auto:
            return "auto";
        case AgglomerationExecutionPolicy::Serial:
            return "serial";
        case AgglomerationExecutionPolicy::OmpBatched:
            return "omp-batched";
    }
    return "serial";
}

const char *sizeBiasStrategyName(SizeBiasStrategy strategy) {
    switch (strategy) {
        case SizeBiasStrategy::Off:
            return "off";
        case SizeBiasStrategy::SoftBias:
            return "soft-bias";
        case SizeBiasStrategy::Cleanup:
            return "cleanup";
        case SizeBiasStrategy::SoftBiasAndCleanup:
            return "soft-bias-and-cleanup";
    }
    return "off";
}

WatershedRagAgglomerationResult runWatershedRagAgglomeration(
    dataType::SegmentsImageType::Pointer labels,
    BoundaryFloatImageType::Pointer boundary,
    BoundaryMaskImageType::Pointer thresholdMask,
    const WatershedRagAgglomerationOptions &options) {
    const dataType::SegmentIdType *pLabels = labels->GetBufferPointer();
    const unsigned char *pMask = thresholdMask.IsNotNull() ? thresholdMask->GetBufferPointer() : nullptr;
    return runWatershedRagAgglomerationImpl(
        labels,
        boundary,
        thresholdMask,
        options,
        [pLabels, pMask](size_t index) {
            return pLabels[index] != 0 && (pMask == nullptr || pMask[index] == 0);
        },
        false);
}

WatershedRagAgglomerationResult runWatershedRagAgglomerationPreview(
    dataType::SegmentsImageType::Pointer labels,
    BoundaryFloatImageType::Pointer boundary,
    const OrthoPlanePreviewSelection &previewSelection,
    BoundaryMaskImageType::Pointer thresholdMask,
    const WatershedRagAgglomerationOptions &options) {
    if (labels.IsNull()) {
        throw std::invalid_argument("Watershed RAG preview requires labels.");
    }

    const auto size = labels->GetLargestPossibleRegion().GetSize();
    const std::array<int, 3> clampedSlices = {{
        std::clamp(previewSelection.sliceIndices[0], 0, static_cast<int>(size[0]) - 1),
        std::clamp(previewSelection.sliceIndices[1], 0, static_cast<int>(size[1]) - 1),
        std::clamp(previewSelection.sliceIndices[2], 0, static_cast<int>(size[2]) - 1)
    }};
    const size_t dimX = size[0];
    const size_t dimY = size[1];
    const size_t planeSizeXY = dimX * dimY;

    WatershedRagAgglomerationOptions previewOptions = options;
    previewOptions.executionPolicy = AgglomerationExecutionPolicy::Serial;
    const dataType::SegmentIdType *pLabels = labels->GetBufferPointer();
    const unsigned char *pMask = thresholdMask.IsNotNull() ? thresholdMask->GetBufferPointer() : nullptr;
    return runWatershedRagAgglomerationImpl(
        labels,
        boundary,
        thresholdMask,
        previewOptions,
        [clampedSlices, dimX, planeSizeXY, pLabels, pMask](size_t linearIndex) {
            if (pLabels[linearIndex] == 0) return false;
            if (pMask != nullptr && pMask[linearIndex] != 0) return false;
            const size_t z = linearIndex / planeSizeXY;
            const size_t rem = linearIndex % planeSizeXY;
            const size_t y = rem / dimX;
            const size_t x = rem % dimX;
            return static_cast<int>(x) == clampedSlices[0] ||
                   static_cast<int>(y) == clampedSlices[1] ||
                   static_cast<int>(z) == clampedSlices[2];
        },
        true);
}

} // namespace segment_puzzler
