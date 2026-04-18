#include "FastMarkerWatershed3D.h"

#ifdef SEGMENT_PUZZLER_ENABLE_PERFETTO
#include "examples/benchmarks/BenchmarkPerfettoTracing.h"
#else
#define TRACE_EVENT(...)
#define TRACE_EVENT_BEGIN(...)
#define TRACE_EVENT_END(...)
#define TRACE_EVENT_INSTANT(...)
#define TRACE_COUNTER(...)
#define TRACE_EVENT_CATEGORY_ENABLED(...) false
#endif

#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace segment_puzzler {
namespace {

constexpr std::uint8_t kFar = 0;
constexpr std::uint8_t kInQueue = 1;
constexpr std::uint8_t kDone = 2;
constexpr int kMaxBucketCount = 65536;
constexpr std::size_t kFloodCounterSampleInterval = 4096;
constexpr double kSlowFloodLevelThresholdMs = 5.0;

double wallTimeSeconds() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

std::size_t flattenIndex(int x, int y, int z, const std::array<int, 3> &dims) {
    return static_cast<std::size_t>((z * dims[1] + y) * dims[0] + x);
}

// Per-bucket FIFO queues with lazy deletion for moves.
// Eliminates the doubly-linked list and its next/prev arrays.
// Each bucket is a flat vector with a read cursor: push appends to the end,
// pop reads from the cursor position. This is O(1) for both and cache-friendly
// (sequential reads). FIFO ordering within a bucket matches the original
// doubly-linked-list behaviour.
// When a voxel is moved to a lower bucket a new entry is pushed there and the
// old entry is left in place; stale entries are detected by
// state[idx] != kInQueue when popped.
struct BucketQueueSet {
    struct Bucket {
        std::vector<std::uint32_t> items;
        std::size_t readHead = 0;

        void push(std::uint32_t idx) { items.push_back(idx); }
        bool empty() const { return readHead >= items.size(); }
        std::uint32_t front() const { return items[readHead]; }
        void advance() { ++readHead; }
        void clear() { items.clear(); readHead = 0; }
    };

    std::vector<Bucket> buckets;
    int levelCount = 0;
    int currentLevel = 0;
    std::size_t queueDepth = 0;
    std::size_t maxQueueDepth = 0;
    std::size_t maxBucketOccupancy = 0;

    explicit BucketQueueSet(int levels)
        : buckets(static_cast<std::size_t>(std::max(1, levels))),
          levelCount(std::max(1, levels)) {}

    void push(int level, std::uint32_t idx) {
        Bucket &bucket = buckets[static_cast<std::size_t>(level)];
        bucket.push(idx);
        // Keeping this unconditional benchmarked better than gating queue stats in the hot path.
        ++queueDepth;
        maxQueueDepth = std::max(maxQueueDepth, queueDepth);
        maxBucketOccupancy = std::max(maxBucketOccupancy, bucket.items.size() - bucket.readHead);
        if (level < currentLevel) {
            currentLevel = level;
        }
    }

    bool pop(int &levelOut, std::uint32_t &idxOut) {
        while (currentLevel < levelCount && buckets[static_cast<std::size_t>(currentLevel)].empty()) {
            buckets[static_cast<std::size_t>(currentLevel)].clear();
            ++currentLevel;
        }
        if (currentLevel >= levelCount) {
            return false;
        }
        levelOut = currentLevel;
        idxOut = buckets[static_cast<std::size_t>(currentLevel)].front();
        buckets[static_cast<std::size_t>(currentLevel)].advance();
        if (queueDepth > 0) {
            --queueDepth;
        }
        return true;
    }

    std::size_t currentDepth() const {
        return queueDepth;
    }
};

void sampleFloodCounters(const BucketQueueSet &queues,
                         int currentLevel,
                         const FastMarkerWatershedMetrics &metrics);

void enqueueOrMove(BucketQueueSet &queues,
                   std::vector<std::uint32_t> &owner,
                   std::vector<std::uint16_t> &bestLevel,
                   std::vector<std::uint8_t> &state,
                   std::uint32_t idx,
                   std::uint32_t label,
                   std::uint16_t enqueueLevel,
                   FastMarkerWatershedMetrics &metrics);

bool advanceToNextBucketLevel(BucketQueueSet &queues, int &levelOut) {
    while (queues.currentLevel < queues.levelCount) {
        auto &bucket = queues.buckets[static_cast<std::size_t>(queues.currentLevel)];
        if (bucket.readHead < bucket.items.size()) {
            levelOut = queues.currentLevel;
            return true;
        }
        bucket.clear();
        ++queues.currentLevel;
    }
    return false;
}

inline void enqueueOrMoveRaw(BucketQueueSet &queues,
                             std::uint32_t *owner,
                             std::uint16_t *bestLevel,
                             std::uint8_t *state,
                             std::uint32_t idx,
                             std::uint32_t label,
                             std::uint16_t enqueueLevel,
                             FastMarkerWatershedMetrics &metrics) {
    if (state[idx] == kFar) {
        state[idx] = kInQueue;
        bestLevel[idx] = enqueueLevel;
        owner[idx] = label;
        queues.push(static_cast<int>(enqueueLevel), idx);
        ++metrics.enqueuedVoxelCount;
        ++metrics.uniqueQueuedVoxelCount;
        return;
    }

    if (state[idx] == kInQueue && enqueueLevel < bestLevel[idx]) {
        bestLevel[idx] = enqueueLevel;
        owner[idx] = label;
        queues.push(static_cast<int>(enqueueLevel), idx);
        ++metrics.enqueuedVoxelCount;
        ++metrics.requeueCount;
    }
}

std::array<int, 6> buildNeighborStrides6(const std::array<int, 3> &paddedDims) {
    const int xStride = 1;
    const int yStride = paddedDims[0];
    const int zStride = paddedDims[0] * paddedDims[1];
    return {{-zStride, -yStride, -xStride, xStride, yStride, zStride}};
}

std::array<int, 26> buildNeighborStrides26(const std::array<int, 3> &paddedDims) {
    std::array<int, 26> offsets{};
    std::size_t index = 0;
    const int xStride = 1;
    const int yStride = paddedDims[0];
    const int zStride = paddedDims[0] * paddedDims[1];
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                offsets[index++] = dx * xStride + dy * yStride + dz * zStride;
            }
        }
    }
    return offsets;
}

template <std::size_t NeighborCount>
void expandSeedFrontier(const std::array<int, NeighborCount> &neighbors,
                        BucketQueueSet &queues,
                        std::vector<std::uint32_t> &owner,
                        std::vector<std::uint16_t> &bestLevel,
                        std::vector<std::uint8_t> &state,
                        const std::vector<std::uint16_t> &paddedCosts,
                        const std::vector<std::uint32_t> &seedIndices,
                        FastMarkerWatershedMetrics &metrics) {
    for (const std::uint32_t seedIdx : seedIndices) {
        const std::uint32_t seedLabel = owner[seedIdx];
        for (std::size_t neighborIndex = 0; neighborIndex < NeighborCount; ++neighborIndex) {
            ++metrics.neighborCheckCount;
            const auto nIdx = static_cast<std::size_t>(static_cast<int>(seedIdx) + neighbors[neighborIndex]);
            if (state[nIdx] == kDone) {
                continue;
            }
            const std::uint16_t enqueueLevel = paddedCosts[nIdx];
            enqueueOrMoveRaw(queues,
                             owner.data(), bestLevel.data(), state.data(),
                             static_cast<std::uint32_t>(nIdx), seedLabel,
                             enqueueLevel, metrics);
        }
    }
}

template <std::size_t NeighborCount>
void runFloodLoop(const std::array<int, NeighborCount> &neighbors,
                  BucketQueueSet &queues,
                  std::vector<std::uint32_t> &owner,
                  std::vector<std::uint16_t> &bestLevel,
                  std::vector<std::uint8_t> &state,
                  const std::vector<std::uint16_t> &paddedCosts,
                  FastMarkerWatershedMetrics &localMetrics) {
    auto *const ownerData = owner.data();
    auto *const bestLevelData = bestLevel.data();
    auto *const stateData = state.data();
    const auto *const paddedCostsData = paddedCosts.data();

    // Gate trace emission here, not queue-state updates: the gated bookkeeping variant regressed.
    const bool queueCounterTracingEnabled = TRACE_EVENT_CATEGORY_ENABLED("watershed.queue") ||
                                            TRACE_EVENT_CATEGORY_ENABLED("watershed");
    const bool debugLevelTracingEnabled = TRACE_EVENT_CATEGORY_ENABLED("watershed.debug");
    const bool slowLevelTracingEnabled = TRACE_EVENT_CATEGORY_ENABLED("watershed.slow");
    const bool anyTracingEnabled = queueCounterTracingEnabled || debugLevelTracingEnabled || slowLevelTracingEnabled;

    int currentLevel = 0;
    if (!anyTracingEnabled) {
        while (advanceToNextBucketLevel(queues, currentLevel)) {
            auto &bucket = queues.buckets[static_cast<std::size_t>(currentLevel)];
            const auto currentLevelU16 = static_cast<std::uint16_t>(currentLevel);
            while (bucket.readHead < bucket.items.size()) {
                const std::uint32_t idx = bucket.items[bucket.readHead++];
                if (queues.queueDepth > 0) {
                    --queues.queueDepth;
                }
                ++localMetrics.popCount;
                if (stateData[idx] != kInQueue) {
                    ++localMetrics.stalePopCount;
                    continue;
                }

                stateData[idx] = kDone;
                ++localMetrics.finalizedVoxelCount;
                const std::uint32_t ownerLabel = ownerData[idx];
                for (std::size_t neighborIndex = 0; neighborIndex < NeighborCount; ++neighborIndex) {
                    ++localMetrics.neighborCheckCount;
                    const std::uint32_t nIdx = static_cast<std::uint32_t>(static_cast<int>(idx) + neighbors[neighborIndex]);
                    if (stateData[nIdx] == kDone) {
                        continue;
                    }

                    const std::uint16_t enqueueLevel = std::max<std::uint16_t>(currentLevelU16, paddedCostsData[nIdx]);
                    enqueueOrMoveRaw(queues, ownerData, bestLevelData, stateData,
                                     nIdx, ownerLabel, enqueueLevel, localMetrics);
                }
            }
            bucket.clear();
            ++queues.currentLevel;
        }
        return;
    }

    const bool levelTimingEnabled = debugLevelTracingEnabled || slowLevelTracingEnabled;
    bool floodLevelTraceOpen = false;
    int tracedFloodLevel = -1;
    double tracedFloodLevelStart = 0.0;
    std::size_t tracedFloodLevelQueueDepth = 0;
    std::size_t tracedFloodLevelFinalizedBefore = 0;
    std::size_t tracedFloodLevelStalePopsBefore = 0;
    auto beginFloodLevelTrace = [&](int level) {
        tracedFloodLevel = level;
        tracedFloodLevelStart = levelTimingEnabled ? wallTimeSeconds() : 0.0;
        tracedFloodLevelQueueDepth = queues.currentDepth();
        tracedFloodLevelFinalizedBefore = localMetrics.finalizedVoxelCount;
        tracedFloodLevelStalePopsBefore = localMetrics.stalePopCount;
        floodLevelTraceOpen = true;
        if (debugLevelTracingEnabled) {
            TRACE_EVENT_BEGIN("watershed.debug", "fast_marker_flood_level",
                              "level", level,
                              "queue_depth", static_cast<uint64_t>(tracedFloodLevelQueueDepth),
                              "finalized_before", static_cast<uint64_t>(tracedFloodLevelFinalizedBefore),
                              "stale_pops_before", static_cast<uint64_t>(tracedFloodLevelStalePopsBefore));
        }
    };
    auto endFloodLevelTrace = [&]() {
        if (!floodLevelTraceOpen) {
            return;
        }
        if (debugLevelTracingEnabled) {
            TRACE_EVENT_END("watershed.debug");
        }
        if (slowLevelTracingEnabled) {
            const double levelMs = (wallTimeSeconds() - tracedFloodLevelStart) * 1000.0;
            if (levelMs >= kSlowFloodLevelThresholdMs) {
                TRACE_EVENT_INSTANT("watershed.slow", "fast_marker_flood_level_slow",
                                    "level", tracedFloodLevel,
                                    "duration_ms", levelMs,
                                    "queue_depth", static_cast<uint64_t>(tracedFloodLevelQueueDepth),
                                    "finalized_before", static_cast<uint64_t>(tracedFloodLevelFinalizedBefore),
                                    "stale_pops_before", static_cast<uint64_t>(tracedFloodLevelStalePopsBefore));
            }
        }
        floodLevelTraceOpen = false;
    };

    while (advanceToNextBucketLevel(queues, currentLevel)) {
        auto &bucket = queues.buckets[static_cast<std::size_t>(currentLevel)];
        const auto currentLevelU16 = static_cast<std::uint16_t>(currentLevel);
        endFloodLevelTrace();
        beginFloodLevelTrace(currentLevel);
        if (queueCounterTracingEnabled) {
            sampleFloodCounters(queues, currentLevel, localMetrics);
        }

        while (bucket.readHead < bucket.items.size()) {
            const std::uint32_t idx = bucket.items[bucket.readHead++];
            if (queues.queueDepth > 0) {
                --queues.queueDepth;
            }
            ++localMetrics.popCount;
            if (stateData[idx] != kInQueue) {
                ++localMetrics.stalePopCount;
                if (queueCounterTracingEnabled && (localMetrics.popCount % kFloodCounterSampleInterval) == 0) {
                    sampleFloodCounters(queues, currentLevel, localMetrics);
                }
                continue;
            }

            stateData[idx] = kDone;
            ++localMetrics.finalizedVoxelCount;
            const std::uint32_t ownerLabel = ownerData[idx];
            for (std::size_t neighborIndex = 0; neighborIndex < NeighborCount; ++neighborIndex) {
                ++localMetrics.neighborCheckCount;
                const std::uint32_t nIdx = static_cast<std::uint32_t>(static_cast<int>(idx) + neighbors[neighborIndex]);
                if (stateData[nIdx] == kDone) {
                    continue;
                }

                const std::uint16_t enqueueLevel = std::max<std::uint16_t>(currentLevelU16, paddedCostsData[nIdx]);
                enqueueOrMoveRaw(queues, ownerData, bestLevelData, stateData,
                                 nIdx, ownerLabel, enqueueLevel, localMetrics);
            }

            if (queueCounterTracingEnabled && (localMetrics.popCount % kFloodCounterSampleInterval) == 0) {
                sampleFloodCounters(queues, currentLevel, localMetrics);
            }
        }
        bucket.clear();
        ++queues.currentLevel;
    }
    endFloodLevelTrace();
    if (queueCounterTracingEnabled) {
        sampleFloodCounters(queues, currentLevel, localMetrics);
    }
}

struct CostRange {
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();

    bool hasFiniteSpread() const {
        return std::isfinite(minValue) && std::isfinite(maxValue) && maxValue > minValue;
    }
};

CostRange computeCostRange(segment_puzzler::FastMarkerWatershedCostImage::Pointer costImage) {
    const auto region = costImage->GetLargestPossibleRegion();
    CostRange range;
    itk::ImageRegionConstIterator<segment_puzzler::FastMarkerWatershedCostImage> it(costImage, region);
    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        const float value = it.Get();
        range.minValue = std::min(range.minValue, value);
        range.maxValue = std::max(range.maxValue, value);
    }

    return range;
}

std::uint16_t quantizeCostValue(float value, const CostRange &range, double scale) {
    if (!range.hasFiniteSpread()) {
        return 0;
    }

    const double normalized = (static_cast<double>(value) - static_cast<double>(range.minValue)) * scale;
    const double clamped = std::max(0.0, std::min(static_cast<double>(kMaxBucketCount - 1), normalized));
    return static_cast<std::uint16_t>(std::llround(clamped));
}

int compactQuantizedLevels(std::vector<std::uint16_t> &paddedCosts,
                           const std::array<int, 3> &dims,
                           const std::array<int, 3> &paddedDims,
                           const std::array<std::uint8_t, kMaxBucketCount> &usedLevels) {
    std::array<std::uint16_t, kMaxBucketCount> remap{};
    int compactLevelCount = 0;
    for (int level = 0; level < kMaxBucketCount; ++level) {
        if (usedLevels[static_cast<std::size_t>(level)] == 0) {
            continue;
        }
        remap[static_cast<std::size_t>(level)] = static_cast<std::uint16_t>(compactLevelCount++);
    }

    if (compactLevelCount <= 1 || compactLevelCount == kMaxBucketCount) {
        return std::max(1, compactLevelCount);
    }

    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                const std::size_t paddedIdx = flattenIndex(x + 1, y + 1, z + 1, paddedDims);
                paddedCosts[paddedIdx] = remap[paddedCosts[paddedIdx]];
            }
        }
    }

    return compactLevelCount;
}

// Keep Perfetto queue counters gated here instead of branching queue bookkeeping in push/pop.
void sampleFloodCounters(const BucketQueueSet &queues,
                         int currentLevel,
                         const FastMarkerWatershedMetrics &metrics) {
    TRACE_COUNTER("watershed.queue", "queue_depth", static_cast<uint64_t>(queues.currentDepth()));
    TRACE_COUNTER("watershed.queue", "current_level", currentLevel);
    TRACE_COUNTER("watershed", "finalized_voxels_total", static_cast<uint64_t>(metrics.finalizedVoxelCount));
    TRACE_COUNTER("watershed", "stale_pops_total", static_cast<uint64_t>(metrics.stalePopCount));
    TRACE_COUNTER("watershed", "neighbor_checks_total", static_cast<uint64_t>(metrics.neighborCheckCount));
}

void enqueueOrMove(BucketQueueSet &queues,
                   std::vector<std::uint32_t> &owner,
                   std::vector<std::uint16_t> &bestLevel,
                   std::vector<std::uint8_t> &state,
                   std::uint32_t idx,
                   std::uint32_t label,
                   std::uint16_t enqueueLevel,
                   FastMarkerWatershedMetrics &metrics) {
    enqueueOrMoveRaw(queues, owner.data(), bestLevel.data(), state.data(),
                     idx, label, enqueueLevel, metrics);
}

} // namespace

FastMarkerWatershedLabelImage::Pointer runFastMarkerWatershed3D(
    FastMarkerWatershedCostImage::Pointer costImage,
    FastMarkerWatershedLabelImage::Pointer markers,
    const FastMarkerWatershedOptions &options,
    FastMarkerWatershedMetrics *metrics) {
    TRACE_EVENT("watershed", "fast_marker_watershed_backend",
                "fully_connected", options.fullyConnected,
                "watershed_lines", options.markWatershedLine);
    if (TRACE_EVENT_CATEGORY_ENABLED("watershed.parallel")) {
        TRACE_EVENT_INSTANT("watershed.parallel", "fast_marker_parallel_mode",
                            "mode", "sequential",
                            "phase", "flood");
    }
    if (costImage.IsNull() || markers.IsNull()) {
        throw std::runtime_error("Fast marker watershed requires both a cost image and markers.");
    }
    if (options.markWatershedLine) {
        throw std::runtime_error("Fast marker watershed does not support watershed lines yet.");
    }
    if (costImage->GetLargestPossibleRegion().GetSize() != markers->GetLargestPossibleRegion().GetSize()) {
        throw std::runtime_error("Fast marker watershed requires matching cost/marker dimensions.");
    }

    FastMarkerWatershedMetrics localMetrics;
    const double totalStart = wallTimeSeconds();

    const auto region = costImage->GetLargestPossibleRegion();
    const auto size = region.GetSize();
    const std::array<int, 3> dims = {
        static_cast<int>(size[0]),
        static_cast<int>(size[1]),
        static_cast<int>(size[2])
    };
    const std::size_t voxelCount = region.GetNumberOfPixels();
    const std::array<int, 3> paddedDims = {dims[0] + 2, dims[1] + 2, dims[2] + 2};
    const std::size_t paddedVoxelCount = static_cast<std::size_t>(paddedDims[0]) *
                                         static_cast<std::size_t>(paddedDims[1]) *
                                         static_cast<std::size_t>(paddedDims[2]);
    const double quantizeStart = wallTimeSeconds();
    CostRange costRange;
    {
        TRACE_EVENT("watershed", "fast_marker_quantize_costs", "voxels", static_cast<uint64_t>(voxelCount));
        costRange = computeCostRange(costImage);
    }
    const double quantizeEnd = wallTimeSeconds();
    localMetrics.quantizeMs = (quantizeEnd - quantizeStart) * 1000.0;

    auto output = FastMarkerWatershedLabelImage::New();
    output->SetRegions(region);
    output->SetSpacing(markers->GetSpacing());
    output->SetOrigin(markers->GetOrigin());
    output->SetDirection(markers->GetDirection());
    output->Allocate(true);

    std::vector<std::uint16_t> paddedCosts(paddedVoxelCount, 0);
    // owner holds both the queued label and (after finalization) the final label.
    // A separate labels array is not needed: when a voxel is finalized, owner[idx]
    // already holds the correct label and is used for propagation.
    std::vector<std::uint32_t> owner(paddedVoxelCount, 0);
    std::vector<std::uint16_t> bestLevel(paddedVoxelCount, std::numeric_limits<std::uint16_t>::max());
    std::vector<std::uint8_t> state(paddedVoxelCount, kDone);
    std::vector<std::uint32_t> seedIndices;
    const int neighborCount = options.fullyConnected ? 26 : 6;
    std::array<std::uint8_t, kMaxBucketCount> usedLevels{};

    const double initStart = wallTimeSeconds();
    const double quantizeScale = costRange.hasFiniteSpread()
        ? static_cast<double>(kMaxBucketCount - 1) / static_cast<double>(costRange.maxValue - costRange.minValue)
        : 0.0;
    itk::ImageRegionConstIterator<FastMarkerWatershedCostImage> costIt(costImage, region);
    itk::ImageRegionConstIterator<FastMarkerWatershedLabelImage> markerIt(markers, region);
    seedIndices.reserve(1024);
    {
        TRACE_EVENT("watershed", "fast_marker_initialize_markers",
                    "voxels", static_cast<uint64_t>(voxelCount),
                    "neighbor_count", static_cast<uint64_t>(neighborCount));
        costIt.GoToBegin();
        markerIt.GoToBegin();
        for (int z = 0; z < dims[2]; ++z) {
            for (int y = 0; y < dims[1]; ++y) {
                for (int x = 0; x < dims[0]; ++x, ++costIt, ++markerIt) {
                    const std::size_t paddedIdx = flattenIndex(x + 1, y + 1, z + 1, paddedDims);
                    const std::uint16_t level = quantizeCostValue(costIt.Get(), costRange, quantizeScale);
                    paddedCosts[paddedIdx] = level;
                    usedLevels[static_cast<std::size_t>(level)] = 1;
                    state[paddedIdx] = kFar;
                    const std::uint32_t label = markerIt.Get();
                    if (label > 0) {
                        owner[paddedIdx] = label;
                        state[paddedIdx] = kDone;
                        seedIndices.push_back(static_cast<std::uint32_t>(paddedIdx));
                        ++localMetrics.finalizedVoxelCount;
                    }
                }
            }
        }
    }
    const int compactBucketCount = compactQuantizedLevels(paddedCosts, dims, paddedDims, usedLevels);
    BucketQueueSet queues(compactBucketCount);
    localMetrics.seedCount = seedIndices.size();

    const double seedFrontierStart = wallTimeSeconds();
    // Densifying the actually-used quantized levels preserves order while
    // cutting down the empty-bucket scans in the flood loop.
    {
        TRACE_EVENT("watershed", "fast_marker_seed_frontier");
        if (options.fullyConnected) {
            const auto neighbors26 = buildNeighborStrides26(paddedDims);
            expandSeedFrontier<26>(neighbors26, queues, owner, bestLevel, state, paddedCosts, seedIndices, localMetrics);
        } else {
            const auto neighbors6 = buildNeighborStrides6(paddedDims);
            expandSeedFrontier<6>(neighbors6, queues, owner, bestLevel, state, paddedCosts, seedIndices, localMetrics);
        }
    }
    const double seedFrontierEnd = wallTimeSeconds();
    localMetrics.seedFrontierMs = (seedFrontierEnd - seedFrontierStart) * 1000.0;
    const double initEnd = wallTimeSeconds();
    localMetrics.initMs = (initEnd - initStart) * 1000.0;

    const double floodStart = wallTimeSeconds();
    {
        TRACE_EVENT("watershed", "fast_marker_flood");
        if (options.fullyConnected) {
            const auto neighbors26 = buildNeighborStrides26(paddedDims);
            runFloodLoop<26>(neighbors26, queues, owner, bestLevel, state, paddedCosts, localMetrics);
        } else {
            const auto neighbors6 = buildNeighborStrides6(paddedDims);
            runFloodLoop<6>(neighbors6, queues, owner, bestLevel, state, paddedCosts, localMetrics);
        }
    }
    const double floodEnd = wallTimeSeconds();
    localMetrics.floodMs = (floodEnd - floodStart) * 1000.0;

    localMetrics.floodScanMs = 0.0;
    localMetrics.floodPropagateMs = 0.0;
    localMetrics.maxQueueDepth = queues.maxQueueDepth;
    localMetrics.maxBucketOccupancy = queues.maxBucketOccupancy;
    TRACE_EVENT("watershed", "fast_marker_flood_summary",
                "flood_ms", localMetrics.floodMs,
                "scan_ms", localMetrics.floodScanMs,
                "propagate_ms", localMetrics.floodPropagateMs,
                "stale_pops", static_cast<uint64_t>(localMetrics.stalePopCount),
                "pop_count", static_cast<uint64_t>(localMetrics.popCount),
                "requeues", static_cast<uint64_t>(localMetrics.requeueCount),
                "unique_queued_voxels", static_cast<uint64_t>(localMetrics.uniqueQueuedVoxelCount),
                "max_queue_depth", static_cast<uint64_t>(localMetrics.maxQueueDepth),
                "max_bucket_occupancy", static_cast<uint64_t>(localMetrics.maxBucketOccupancy),
                "seed_count", static_cast<uint64_t>(localMetrics.seedCount),
                "stale_pop_ratio", localMetrics.stalePopRatio(),
                "avg_enqueues_per_voxel", localMetrics.avgEnqueuesPerVoxel(),
                "finalized_voxels", static_cast<uint64_t>(localMetrics.finalizedVoxelCount),
                "enqueues", static_cast<uint64_t>(localMetrics.enqueuedVoxelCount),
                "neighbor_checks", static_cast<uint64_t>(localMetrics.neighborCheckCount));

    const double writebackStart = wallTimeSeconds();
    itk::ImageRegionIterator<FastMarkerWatershedLabelImage> outIt(output, region);
    {
        TRACE_EVENT("watershed", "fast_marker_writeback", "voxels", static_cast<uint64_t>(voxelCount));
        outIt.GoToBegin();
        for (int z = 0; z < dims[2]; ++z) {
            for (int y = 0; y < dims[1]; ++y) {
                for (int x = 0; x < dims[0]; ++x, ++outIt) {
                    outIt.Set(owner[flattenIndex(x + 1, y + 1, z + 1, paddedDims)]);
                }
            }
        }
    }
    const double writebackEnd = wallTimeSeconds();
    localMetrics.writebackMs = (writebackEnd - writebackStart) * 1000.0;

    localMetrics.elapsedMs = (wallTimeSeconds() - totalStart) * 1000.0;
    localMetrics.bucketBytes = static_cast<std::size_t>(queues.levelCount) * sizeof(std::vector<std::uint32_t>);
    localMetrics.scratchBytes =
        paddedCosts.size() * sizeof(std::uint16_t) +
        owner.size() * sizeof(std::uint32_t) +
        bestLevel.size() * sizeof(std::uint16_t) +
        state.size() * sizeof(std::uint8_t);

    if (metrics != nullptr) {
        *metrics = localMetrics;
    }
    return output;
}

} // namespace segment_puzzler
