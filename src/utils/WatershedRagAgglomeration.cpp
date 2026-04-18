#include "src/utils/WatershedRagAgglomeration.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace segment_puzzler {
namespace {

using SegmentIdType = dataType::SegmentIdType;
using SegmentsImageType = dataType::SegmentsImageType;
using NeighborMap = std::unordered_map<int, int>;

struct EdgeStats {
    double totalBoundarySum = 0.0;
    double totalSupport = 0.0;
    double openBoundarySum = 0.0;
    double openSupport = 0.0;
};

struct EdgeAgg {
    int a = -1;
    int b = -1;
    double totalBoundarySum = 0.0;
    double totalSupport = 0.0;
    double openBoundarySum = 0.0;
    double openSupport = 0.0;
    double signedSum = 0.0;
    double support = 0.0;
    double score = 0.0;
    uint32_t version = 0;
    bool alive = true;
};

struct Cluster {
    int parent = -1;
    uint32_t rank = 0;
    uint64_t voxelCount = 0;
    bool alive = true;
    NeighborMap neighbors;
};

struct HeapItem {
    double score = 0.0;
    int edgeId = -1;
    uint32_t version = 0;
};

struct HeapCompare {
    bool operator()(const HeapItem &lhs, const HeapItem &rhs) const {
        if (lhs.score != rhs.score) {
            return lhs.score < rhs.score;
        }
        return lhs.edgeId > rhs.edgeId;
    }
};

struct SelectedMerge {
    int edgeId = -1;
    uint32_t version = 0;
    int rootA = -1;
    int rootB = -1;
    int winner = -1;
    int loser = -1;
};

struct ReductionEdge {
    uint64_t key = 0;
    double totalBoundarySum = 0.0;
    double totalSupport = 0.0;
    double openBoundarySum = 0.0;
    double openSupport = 0.0;
    double signedSum = 0.0;
    double support = 0.0;
};

using EdgeHeap = std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCompare>;

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

void logStepTime(double startTimeSeconds, const char *description) {
    std::cout << description << " " << (currentTimeSeconds() - startTimeSeconds) << std::endl;
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

void recomputeEdgeScore(EdgeAgg &edge, const WatershedRagAgglomerationOptions &options) {
    switch (options.boundaryEvidenceStrategy) {
        case BoundaryEvidenceStrategy::RawInterfaceMean: {
            if (edge.totalSupport <= 0.0) {
                edge.signedSum = 0.0;
                edge.support = 0.0;
                edge.score = 0.0;
                return;
            }
            const double meanBoundary = edge.totalBoundarySum / edge.totalSupport;
            edge.signedSum = edge.totalSupport * (options.tau - meanBoundary);
            edge.support = edge.totalSupport;
            break;
        }
        case BoundaryEvidenceStrategy::OpenInterfaceMean: {
            if (edge.openSupport <= 0.0) {
                edge.signedSum = 0.0;
                edge.support = 0.0;
                edge.score = 0.0;
                return;
            }
            const double meanBoundary = edge.openBoundarySum / edge.openSupport;
            edge.signedSum = edge.openSupport * (options.tau - meanBoundary);
            edge.support = edge.openSupport;
            break;
        }
        case BoundaryEvidenceStrategy::OpenFractionWeighted: {
            if (edge.openSupport <= 0.0) {
                edge.signedSum = 0.0;
                edge.support = edge.totalSupport;
                edge.score = 0.0;
                return;
            }
            const double meanBoundary = edge.openBoundarySum / edge.openSupport;
            edge.signedSum = edge.openSupport * (options.tau - meanBoundary);
            edge.support = edge.totalSupport;
            break;
        }
    }

    if (options.linkage == RagLinkage::Sum) {
        edge.score = edge.signedSum;
        return;
    }
    if (edge.support <= 0.0) {
        edge.score = 0.0;
        return;
    }
    edge.score = edge.signedSum / edge.support;
}

uint64_t makePairKey(int a, int b) {
    const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
    const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(lo) << 32u) | static_cast<uint64_t>(hi);
}

std::pair<int, int> unpackPairKey(uint64_t key) {
    return {
        static_cast<int>(key >> 32u),
        static_cast<int>(key & 0xffffffffu)
    };
}

int findRoot(std::vector<Cluster> &clusters, int node) {
    int root = node;
    while (clusters[root].parent != root) {
        root = clusters[root].parent;
    }
    while (clusters[node].parent != node) {
        const int parent = clusters[node].parent;
        clusters[node].parent = root;
        node = parent;
    }
    return root;
}

int findRootConst(const std::vector<Cluster> &clusters, int node) {
    int root = node;
    while (clusters[root].parent != root) {
        root = clusters[root].parent;
    }
    return root;
}

std::pair<int, int> chooseMergeRoots(const std::vector<Cluster> &clusters, int a, int b) {
    int winner = a;
    int loser = b;
    const auto winnerNeighborCount = clusters[winner].neighbors.size();
    const auto loserNeighborCount = clusters[loser].neighbors.size();
    if (winnerNeighborCount < loserNeighborCount ||
        (winnerNeighborCount == loserNeighborCount && clusters[winner].rank < clusters[loser].rank) ||
        (winnerNeighborCount == loserNeighborCount && clusters[winner].rank == clusters[loser].rank && winner > loser)) {
        std::swap(winner, loser);
    }
    return {winner, loser};
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
    std::cout << "WatershedRagAgglomeration:"
              << " fragments=" << stats.inputFragmentCount
              << " edges=" << stats.ragEdgeCount
              << " merges=" << stats.mergeCount
              << " output_clusters=" << stats.outputClusterCount
              << " policy=" << agglomerationExecutionPolicyName(stats.executionPolicyUsed)
              << " batches=" << stats.batchCount
              << " max_batch_pairs=" << stats.maxBatchPairs
              << " compact_ms=" << stats.compactLabelsMs
              << " rag_ms=" << stats.ragBuildMs
              << " heap_ms=" << stats.heapInitMs
              << " agglomeration_ms=" << stats.agglomerationMs
              << " batch_select_ms=" << stats.batchSelectionMs
              << " batch_reduce_ms=" << stats.batchReduceMs
              << " batch_apply_ms=" << stats.batchApplyMs
              << " projection_ms=" << stats.projectionMs
              << " boundary_min=" << stats.boundaryMin
              << " boundary_max=" << stats.boundaryMax
              << " boundary_scale=" << boundaryNormalizationModeName(stats.resolvedBoundaryNormalization)
              << "\n";
}

template <typename IsActiveVoxel>
struct AgglomerationContext {
    SegmentsImageType::Pointer labels;
    BoundaryFloatImageType::Pointer boundary;
    BoundaryMaskImageType::Pointer thresholdMask;
    const WatershedRagAgglomerationOptions &options;
    IsActiveVoxel isActiveVoxel;
    WatershedRagAgglomerationStats stats;
    std::vector<int> denseLabels;
    std::vector<Cluster> clusters;
    std::vector<EdgeAgg> edges;
    EdgeHeap heap;
    BoundaryNormalizationMode resolvedBoundaryMode = BoundaryNormalizationMode::AutoDetect;
    size_t dimX = 0;
    size_t dimY = 0;
    size_t dimZ = 0;
    size_t voxelCount = 0;
    const SegmentIdType *labelBuffer = nullptr;
    const float *boundaryBuffer = nullptr;
    const unsigned char *thresholdMaskBuffer = nullptr;
    std::vector<int> rootRemap;
};

template <typename IsActiveVoxel>
void computeBoundaryRange(AgglomerationContext<IsActiveVoxel> &ctx) {
    double boundaryMin = std::numeric_limits<double>::infinity();
    double boundaryMax = -std::numeric_limits<double>::infinity();
    const int threadCount = effectiveThreadCount(ctx.options);
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount) reduction(min:boundaryMin) reduction(max:boundaryMax)
#endif
    for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
        const double value = static_cast<double>(ctx.boundaryBuffer[index]);
        if (!std::isfinite(value)) {
            continue;
        }
        boundaryMin = std::min(boundaryMin, value);
        boundaryMax = std::max(boundaryMax, value);
    }
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
void compactLabels(AgglomerationContext<IsActiveVoxel> &ctx) {
    double t = currentTimeSeconds();
    ctx.denseLabels.assign(ctx.voxelCount, -1);

    SegmentIdType maxLabel = 0;
    std::size_t activeNonZeroCount = 0;
    for (size_t index = 0; index < ctx.voxelCount; ++index) {
        if (!ctx.isActiveVoxel(index)) {
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
        std::vector<int> labelToDense(static_cast<size_t>(maxLabel) + 1, -1);
        for (size_t index = 0; index < ctx.voxelCount; ++index) {
            if (!ctx.isActiveVoxel(index)) {
                continue;
            }
            const SegmentIdType label = ctx.labelBuffer[index];
            // Label 0 is reserved for watershed/boundary/background voxels and is never turned into a fragment node.
            if (label == 0) {
                continue;
            }
            if (labelToDense[label] < 0) {
                labelToDense[label] = static_cast<int>(ctx.clusters.size());
                Cluster cluster;
                cluster.parent = static_cast<int>(ctx.clusters.size());
                ctx.clusters.push_back(std::move(cluster));
            }
        }

        const int threadCount = effectiveThreadCount(ctx.options);
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount)
#endif
        for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
            if (!ctx.isActiveVoxel(static_cast<size_t>(index))) {
                continue;
            }
            const SegmentIdType label = ctx.labelBuffer[index];
            if (label == 0) {
                continue;
            }
            const int denseId = labelToDense[label];
            ctx.denseLabels[index] = denseId;
#ifdef USE_OMP
#pragma omp atomic
#endif
            ctx.clusters[denseId].voxelCount += 1;
        }
    } else {
        std::unordered_map<SegmentIdType, int> oldToDense;
        oldToDense.reserve(activeNonZeroCount / 4 + 16);
        for (size_t index = 0; index < ctx.voxelCount; ++index) {
            if (!ctx.isActiveVoxel(index)) {
                continue;
            }
            const SegmentIdType label = ctx.labelBuffer[index];
            // Label 0 is reserved for watershed/boundary/background voxels and is never turned into a fragment node.
            if (label == 0) {
                continue;
            }
            auto insertion = oldToDense.emplace(label, static_cast<int>(ctx.clusters.size()));
            if (insertion.second) {
                Cluster cluster;
                cluster.parent = static_cast<int>(ctx.clusters.size());
                ctx.clusters.push_back(std::move(cluster));
            }
            const int denseId = insertion.first->second;
            ctx.denseLabels[index] = denseId;
            ++ctx.clusters[denseId].voxelCount;
        }
    }

    ctx.stats.inputFragmentCount = ctx.clusters.size();
    ctx.rootRemap.resize(ctx.clusters.size());
    std::iota(ctx.rootRemap.begin(), ctx.rootRemap.end(), 0);
    ctx.stats.compactLabelsMs = elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration compact labels done:");
}

template <typename IsActiveVoxel>
void buildRag(AgglomerationContext<IsActiveVoxel> &ctx) {
    double t = currentTimeSeconds();
    const int threadCount = effectiveThreadCount(ctx.options);
    const double spacingX = ctx.labels->GetSpacing()[0];
    const double spacingY = ctx.labels->GetSpacing()[1];
    const double spacingZ = ctx.labels->GetSpacing()[2];
    const double faceAreaX = ctx.options.usePhysicalFaceArea ? spacingY * spacingZ : 1.0;
    const double faceAreaY = ctx.options.usePhysicalFaceArea ? spacingX * spacingZ : 1.0;
    const double faceAreaZ = ctx.options.usePhysicalFaceArea ? spacingX * spacingY : 1.0;

    std::vector<std::unordered_map<uint64_t, EdgeStats>> threadMaps(static_cast<size_t>(threadCount));
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

                const auto accumulateFace = [&](size_t idxA, size_t idxB, double support) {
                    if (!ctx.isActiveVoxel(idxB)) {
                        return;
                    }
                    const int denseA = ctx.denseLabels[idxA];
                    const int denseB = ctx.denseLabels[idxB];
                    if (denseA < 0 || denseB < 0 || denseA == denseB) {
                        return;
                    }
                    const double boundaryA = normalizeBoundaryValue(ctx.boundaryBuffer[idxA], ctx.resolvedBoundaryMode);
                    const double boundaryB = normalizeBoundaryValue(ctx.boundaryBuffer[idxB], ctx.resolvedBoundaryMode);
                    EdgeStats &edge = ragStats[makePairKey(denseA, denseB)];
                    const double faceBoundary = 0.5 * (boundaryA + boundaryB) * support;
                    edge.totalBoundarySum += faceBoundary;
                    edge.totalSupport += support;
                    const bool isOpenFace = ctx.thresholdMaskBuffer == nullptr ||
                                            (ctx.thresholdMaskBuffer[idxA] == 0 && ctx.thresholdMaskBuffer[idxB] == 0);
                    if (isOpenFace) {
                        edge.openBoundarySum += faceBoundary;
                        edge.openSupport += support;
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

    std::unordered_map<uint64_t, EdgeStats> ragStats;
    ragStats.reserve(std::max<std::size_t>(32, ctx.stats.inputFragmentCount * 4));
    for (const auto &threadMap : threadMaps) {
        for (const auto &entry : threadMap) {
            EdgeStats &edge = ragStats[entry.first];
            edge.totalBoundarySum += entry.second.totalBoundarySum;
            edge.totalSupport += entry.second.totalSupport;
            edge.openBoundarySum += entry.second.openBoundarySum;
            edge.openSupport += entry.second.openSupport;
        }
    }

    std::vector<uint64_t> keys;
    keys.reserve(ragStats.size());
    for (const auto &entry : ragStats) {
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());

    ctx.edges.reserve(keys.size());
    for (uint64_t key : keys) {
        const auto endpoints = unpackPairKey(key);
        const EdgeStats &edgeStats = ragStats.at(key);
        EdgeAgg edge;
        edge.a = endpoints.first;
        edge.b = endpoints.second;
        edge.totalBoundarySum = edgeStats.totalBoundarySum;
        edge.totalSupport = edgeStats.totalSupport;
        edge.openBoundarySum = edgeStats.openBoundarySum;
        edge.openSupport = edgeStats.openSupport;
        recomputeEdgeScore(edge, ctx.options);
        const int edgeId = static_cast<int>(ctx.edges.size());
        ctx.edges.push_back(edge);
        ctx.clusters[edge.a].neighbors.emplace(edge.b, edgeId);
        ctx.clusters[edge.b].neighbors.emplace(edge.a, edgeId);
    }

    ctx.stats.ragEdgeCount = ctx.edges.size();
    ctx.stats.ragBuildMs = elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration build rag done:");
}

template <typename IsActiveVoxel>
void initHeap(AgglomerationContext<IsActiveVoxel> &ctx) {
    double t = currentTimeSeconds();
    for (int edgeId = 0; edgeId < static_cast<int>(ctx.edges.size()); ++edgeId) {
        if (ctx.edges[edgeId].score > 0.0) {
            ctx.heap.push({ctx.edges[edgeId].score, edgeId, ctx.edges[edgeId].version});
        }
    }
    ctx.stats.heapInitMs = elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration init heap done:");
}

template <typename IsActiveVoxel>
void runSerialAgglomeration(AgglomerationContext<IsActiveVoxel> &ctx) {
    double t = currentTimeSeconds();
    while (!ctx.heap.empty()) {
        const HeapItem item = ctx.heap.top();
        ctx.heap.pop();

        if (item.edgeId < 0 || item.edgeId >= static_cast<int>(ctx.edges.size())) {
            continue;
        }

        EdgeAgg &edge = ctx.edges[item.edgeId];
        if (!edge.alive || edge.version != item.version) {
            continue;
        }

        const int rootA = findRoot(ctx.clusters, edge.a);
        const int rootB = findRoot(ctx.clusters, edge.b);
        if (rootA == rootB) {
            edge.alive = false;
            continue;
        }
        if (edge.score <= 0.0) {
            break;
        }

        const auto chosenRoots = chooseMergeRoots(ctx.clusters, rootA, rootB);
        const int winner = chosenRoots.first;
        const int loser = chosenRoots.second;
        ctx.clusters[loser].parent = winner;
        if (ctx.clusters[winner].rank == ctx.clusters[loser].rank) {
            ++ctx.clusters[winner].rank;
        }
        ctx.clusters[winner].voxelCount += ctx.clusters[loser].voxelCount;

        auto mergedEdgeIt = ctx.clusters[winner].neighbors.find(loser);
        if (mergedEdgeIt != ctx.clusters[winner].neighbors.end()) {
            ctx.edges[mergedEdgeIt->second].alive = false;
            ctx.clusters[winner].neighbors.erase(mergedEdgeIt);
        }
        ctx.clusters[loser].neighbors.erase(winner);

        std::vector<std::pair<int, int>> loserNeighbors;
        loserNeighbors.reserve(ctx.clusters[loser].neighbors.size());
        for (const auto &entry : ctx.clusters[loser].neighbors) {
            loserNeighbors.push_back(entry);
        }

        for (const auto &entry : loserNeighbors) {
            const int neighborRoot = findRoot(ctx.clusters, entry.first);
            const int edgeId = entry.second;
            if (neighborRoot == winner) {
                if (edgeId >= 0 && edgeId < static_cast<int>(ctx.edges.size())) {
                    ctx.edges[edgeId].alive = false;
                }
                continue;
            }
            if (edgeId < 0 || edgeId >= static_cast<int>(ctx.edges.size()) || !ctx.edges[edgeId].alive) {
                continue;
            }

            auto existingIt = ctx.clusters[winner].neighbors.find(neighborRoot);
            if (existingIt != ctx.clusters[winner].neighbors.end()) {
                const int keepEdgeId = existingIt->second;
                EdgeAgg &keepEdge = ctx.edges[keepEdgeId];
                EdgeAgg &dropEdge = ctx.edges[edgeId];
                keepEdge.totalBoundarySum += dropEdge.totalBoundarySum;
                keepEdge.totalSupport += dropEdge.totalSupport;
                keepEdge.openBoundarySum += dropEdge.openBoundarySum;
                keepEdge.openSupport += dropEdge.openSupport;
                keepEdge.signedSum += dropEdge.signedSum;
                keepEdge.support += dropEdge.support;
                keepEdge.a = std::min(winner, neighborRoot);
                keepEdge.b = std::max(winner, neighborRoot);
                recomputeEdgeScore(keepEdge, ctx.options);
                ++keepEdge.version;
                dropEdge.alive = false;

                ctx.clusters[neighborRoot].neighbors.erase(loser);
                ctx.clusters[neighborRoot].neighbors[winner] = keepEdgeId;
                if (keepEdge.score > 0.0) {
                    ctx.heap.push({keepEdge.score, keepEdgeId, keepEdge.version});
                }
            } else {
                EdgeAgg &movedEdge = ctx.edges[edgeId];
                movedEdge.a = std::min(winner, neighborRoot);
                movedEdge.b = std::max(winner, neighborRoot);
                recomputeEdgeScore(movedEdge, ctx.options);
                ++movedEdge.version;
                ctx.clusters[winner].neighbors[neighborRoot] = edgeId;
                ctx.clusters[neighborRoot].neighbors.erase(loser);
                ctx.clusters[neighborRoot].neighbors[winner] = edgeId;
                if (movedEdge.score > 0.0) {
                    ctx.heap.push({movedEdge.score, edgeId, movedEdge.version});
                }
            }
        }

        ctx.clusters[loser].neighbors.clear();
        ctx.clusters[loser].alive = false;
        ++ctx.stats.mergeCount;
    }

    ctx.stats.agglomerationMs += elapsedMilliseconds(t);
    logStepTime(t, "WatershedRagAgglomeration agglomeration done:");
}

template <typename IsActiveVoxel>
bool selectMergeBatch(AgglomerationContext<IsActiveVoxel> &ctx, std::vector<SelectedMerge> &batch) {
    double t = currentTimeSeconds();
    batch.clear();
    std::unordered_set<int> usedRoots;
    std::vector<HeapItem> deferredItems;

    while (!ctx.heap.empty()) {
        const HeapItem item = ctx.heap.top();
        ctx.heap.pop();
        if (item.edgeId < 0 || item.edgeId >= static_cast<int>(ctx.edges.size())) {
            continue;
        }

        EdgeAgg &edge = ctx.edges[item.edgeId];
        if (!edge.alive || edge.version != item.version) {
            continue;
        }

        const int rootA = findRoot(ctx.clusters, edge.a);
        const int rootB = findRoot(ctx.clusters, edge.b);
        if (rootA == rootB) {
            edge.alive = false;
            continue;
        }
        if (edge.score <= 0.0) {
            break;
        }
        if (usedRoots.count(rootA) > 0 || usedRoots.count(rootB) > 0) {
            deferredItems.push_back(item);
            continue;
        }

        const auto chosenRoots = chooseMergeRoots(ctx.clusters, rootA, rootB);
        batch.push_back({item.edgeId, item.version, rootA, rootB, chosenRoots.first, chosenRoots.second});
        usedRoots.insert(rootA);
        usedRoots.insert(rootB);
    }

    for (const HeapItem &deferredItem : deferredItems) {
        ctx.heap.push(deferredItem);
    }

    ctx.stats.batchSelectionMs += elapsedMilliseconds(t);
    ctx.stats.maxBatchPairs = std::max(ctx.stats.maxBatchPairs, batch.size());
    return !batch.empty();
}

template <typename IsActiveVoxel>
bool runParallelBatch(AgglomerationContext<IsActiveVoxel> &ctx, const std::vector<SelectedMerge> &batch) {
#ifndef USE_OMP
    (void)ctx;
    (void)batch;
    return false;
#else
    if (batch.size() < 2) {
        return false;
    }

    const int threadCount = effectiveThreadCount(ctx.options);
    std::vector<std::vector<ReductionEdge>> threadReductions(static_cast<size_t>(threadCount));

    double applyStart = currentTimeSeconds();
    for (const SelectedMerge &merge : batch) {
        ctx.clusters[merge.loser].parent = merge.winner;
        if (ctx.clusters[merge.winner].rank == ctx.clusters[merge.loser].rank) {
            ++ctx.clusters[merge.winner].rank;
        }
        ctx.clusters[merge.winner].voxelCount += ctx.clusters[merge.loser].voxelCount;
        ctx.clusters[merge.loser].alive = false;
        ctx.clusters[merge.loser].neighbors.clear();
        ++ctx.stats.mergeCount;
    }

    double reduceStart = currentTimeSeconds();
#pragma omp parallel num_threads(threadCount)
    {
        std::vector<ReductionEdge> localReductions;
        localReductions.reserve(ctx.edges.size() / static_cast<size_t>(threadCount) + 32);
        const int threadId = omp_get_thread_num();

#pragma omp for schedule(static)
        for (long long edgeIndex = 0; edgeIndex < static_cast<long long>(ctx.edges.size()); ++edgeIndex) {
            const EdgeAgg &edge = ctx.edges[static_cast<size_t>(edgeIndex)];
            if (!edge.alive) {
                continue;
            }
            const int rootA = findRootConst(ctx.clusters, edge.a);
            const int rootB = findRootConst(ctx.clusters, edge.b);
            if (rootA == rootB) {
                continue;
            }
            localReductions.push_back({makePairKey(rootA, rootB),
                                       edge.totalBoundarySum,
                                       edge.totalSupport,
                                       edge.openBoundarySum,
                                       edge.openSupport,
                                       edge.signedSum,
                                       edge.support});
        }

        std::sort(localReductions.begin(), localReductions.end(),
                  [](const ReductionEdge &lhs, const ReductionEdge &rhs) {
                      return lhs.key < rhs.key;
                  });
        std::vector<ReductionEdge> reduced;
        reduced.reserve(localReductions.size());
        for (const ReductionEdge &entry : localReductions) {
            if (!reduced.empty() && reduced.back().key == entry.key) {
                reduced.back().totalBoundarySum += entry.totalBoundarySum;
                reduced.back().totalSupport += entry.totalSupport;
                reduced.back().openBoundarySum += entry.openBoundarySum;
                reduced.back().openSupport += entry.openSupport;
                reduced.back().signedSum += entry.signedSum;
                reduced.back().support += entry.support;
            } else {
                reduced.push_back(entry);
            }
        }
        threadReductions[static_cast<size_t>(threadId)] = std::move(reduced);
    }
    ctx.stats.batchReduceMs += elapsedMilliseconds(reduceStart);

    std::unordered_map<uint64_t, EdgeStats> reducedEdges;
    reducedEdges.reserve(ctx.edges.size() / 2 + 16);
    for (const auto &threadReduction : threadReductions) {
        for (const ReductionEdge &entry : threadReduction) {
            EdgeStats &stats = reducedEdges[entry.key];
            stats.totalBoundarySum += entry.totalBoundarySum;
            stats.totalSupport += entry.totalSupport;
            stats.openBoundarySum += entry.openBoundarySum;
            stats.openSupport += entry.openSupport;
        }
    }

    for (size_t clusterIndex = 0; clusterIndex < ctx.clusters.size(); ++clusterIndex) {
        Cluster &cluster = ctx.clusters[clusterIndex];
        cluster.neighbors.clear();
        cluster.alive = (cluster.parent == static_cast<int>(clusterIndex));
    }

    std::vector<uint64_t> keys;
    keys.reserve(reducedEdges.size());
    for (const auto &entry : reducedEdges) {
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());

    ctx.edges.clear();
    ctx.edges.reserve(keys.size());
    ctx.heap = EdgeHeap();
    for (uint64_t key : keys) {
        const auto endpoints = unpackPairKey(key);
        EdgeAgg edge;
        edge.a = endpoints.first;
        edge.b = endpoints.second;
        edge.totalBoundarySum = reducedEdges[key].totalBoundarySum;
        edge.totalSupport = reducedEdges[key].totalSupport;
        edge.openBoundarySum = reducedEdges[key].openBoundarySum;
        edge.openSupport = reducedEdges[key].openSupport;
        recomputeEdgeScore(edge, ctx.options);
        const int edgeId = static_cast<int>(ctx.edges.size());
        ctx.edges.push_back(edge);
        ctx.clusters[edge.a].neighbors[edge.b] = edgeId;
        ctx.clusters[edge.b].neighbors[edge.a] = edgeId;
        if (edge.score > 0.0) {
            ctx.heap.push({edge.score, edgeId, edge.version});
        }
    }

    ++ctx.stats.batchCount;
    ctx.stats.batchApplyMs += elapsedMilliseconds(applyStart);
    return true;
#endif
}

template <typename IsActiveVoxel>
void runAutoOrParallelAgglomeration(AgglomerationContext<IsActiveVoxel> &ctx, bool previewMode) {
    const bool useOmpBatching =
#ifdef USE_OMP
        !previewMode &&
        effectiveThreadCount(ctx.options) > 1 &&
        ctx.stats.ragEdgeCount >= ctx.options.parallelMergeEdgeThreshold &&
        ctx.options.executionPolicy != AgglomerationExecutionPolicy::Serial;
#else
        false;
#endif

    if (ctx.options.executionPolicy == AgglomerationExecutionPolicy::Serial || !useOmpBatching) {
        ctx.stats.executionPolicyUsed = AgglomerationExecutionPolicy::Serial;
        runSerialAgglomeration(ctx);
        return;
    }

    ctx.stats.executionPolicyUsed = AgglomerationExecutionPolicy::OmpBatched;
    double t = currentTimeSeconds();
    std::vector<SelectedMerge> batch;
    bool serialFallbackUsed = false;
    while (selectMergeBatch(ctx, batch)) {
        if (!runParallelBatch(ctx, batch)) {
            for (const SelectedMerge &merge : batch) {
                if (merge.edgeId >= 0 && merge.edgeId < static_cast<int>(ctx.edges.size())) {
                    const EdgeAgg &edge = ctx.edges[merge.edgeId];
                    if (edge.alive && edge.version == merge.version) {
                        ctx.heap.push({edge.score, merge.edgeId, merge.version});
                    }
                }
            }
            serialFallbackUsed = true;
            runSerialAgglomeration(ctx);
            break;
        }
    }
    ctx.stats.agglomerationMs += elapsedMilliseconds(t);
    if (!serialFallbackUsed) {
        logStepTime(t, "WatershedRagAgglomeration agglomeration done:");
    }
}

template <typename IsActiveVoxel>
void projectLabels(AgglomerationContext<IsActiveVoxel> &ctx, WatershedRagAgglomerationResult &result) {
    double t = currentTimeSeconds();
    result.agglomeratedLabels = allocateImageLike<SegmentsImageType>(ctx.labels);
    SegmentIdType *outputBuffer = result.agglomeratedLabels->GetBufferPointer();

    std::vector<int> survivingRoots;
    survivingRoots.reserve(ctx.clusters.size());
    for (int clusterId = 0; clusterId < static_cast<int>(ctx.clusters.size()); ++clusterId) {
        if (findRoot(ctx.clusters, clusterId) == clusterId && ctx.clusters[clusterId].alive) {
            survivingRoots.push_back(clusterId);
        }
    }
    std::sort(survivingRoots.begin(), survivingRoots.end());
    std::vector<SegmentIdType> rootToFinalLabel(ctx.clusters.size(), 0);
    SegmentIdType nextLabel = 1;
    for (int root : survivingRoots) {
        rootToFinalLabel[root] = nextLabel++;
    }

    const int threadCount = effectiveThreadCount(ctx.options);
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount)
#endif
    for (long long index = 0; index < static_cast<long long>(ctx.voxelCount); ++index) {
        if (!ctx.isActiveVoxel(static_cast<size_t>(index))) {
            outputBuffer[index] = 0;
            continue;
        }
        const int denseId = ctx.denseLabels[static_cast<size_t>(index)];
        if (denseId < 0) {
            outputBuffer[index] = 0;
            continue;
        }
        const int root = findRootConst(ctx.clusters, denseId);
        outputBuffer[index] = rootToFinalLabel[root];
    }

    ctx.stats.outputClusterCount = survivingRoots.size();
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

    std::cout << "WatershedRagAgglomeration compact labels\n";
    computeBoundaryRange(ctx);
    compactLabels(ctx);

    std::cout << "WatershedRagAgglomeration build rag\n";
    buildRag(ctx);

    std::cout << "WatershedRagAgglomeration init heap\n";
    initHeap(ctx);

    std::cout << "WatershedRagAgglomeration agglomeration\n";
    runAutoOrParallelAgglomeration(ctx, previewMode);

    WatershedRagAgglomerationResult result;
    result.stats = ctx.stats;
    std::cout << "WatershedRagAgglomeration projection\n";
    projectLabels(ctx, result);
    result.stats = ctx.stats;
    printStats(result.stats);
    return result;
}

} // namespace

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

WatershedRagAgglomerationResult runWatershedRagAgglomeration(
    dataType::SegmentsImageType::Pointer labels,
    BoundaryFloatImageType::Pointer boundary,
    BoundaryMaskImageType::Pointer thresholdMask,
    const WatershedRagAgglomerationOptions &options) {
    return runWatershedRagAgglomerationImpl(
        labels,
        boundary,
        thresholdMask,
        options,
        [](size_t) {
            return true;
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
    return runWatershedRagAgglomerationImpl(
        labels,
        boundary,
        thresholdMask,
        previewOptions,
        [clampedSlices, dimX, planeSizeXY](size_t linearIndex) {
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
