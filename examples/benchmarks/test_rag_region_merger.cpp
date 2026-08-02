#include "src/utils/RegionAdjacencyGraph.h"
#include "src/utils/RegionMerger.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace segment_puzzler;
using namespace segment_puzzler::rag;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectNear(double actual, double expected, const std::string &message) {
    if (std::abs(actual - expected) > 1e-12) {
        throw std::runtime_error(message + ": expected " + std::to_string(expected) + ", got " +
                                 std::to_string(actual));
    }
}

RagEdgeStats makeEdgeStats(double totalBoundarySum, double totalContactArea, double openBoundarySum,
                           double openContactArea) {
    return {totalBoundarySum, totalContactArea, openBoundarySum, openContactArea};
}

void expectConsistentGraph(const RegionAdjacencyGraph &graph) {
    for (RegionId regionId = 0; regionId < static_cast<RegionId>(graph.regionCount()); ++regionId) {
        const Region &region = graph.region(regionId);
        if (!graph.isActiveRegion(regionId)) {
            expect(region.neighborIdToEdgeId.empty(), "Inactive region still has neighbors");
            continue;
        }
        for (const auto &neighborAndEdge : region.neighborIdToEdgeId) {
            const RegionId neighborId = neighborAndEdge.first;
            const EdgeId edgeId = neighborAndEdge.second;
            expect(graph.isActiveRegion(neighborId), "Active region points to an inactive neighbor");
            const RagEdge &edge = graph.edge(edgeId);
            expect(edge.active, "Active region points to an inactive edge");
            expect(edge.firstRegionId == std::min(regionId, neighborId) &&
                       edge.secondRegionId == std::max(regionId, neighborId),
                   "Edge endpoints do not match the adjacency map");
            const auto reciprocalEdge = graph.region(neighborId).neighborIdToEdgeId.find(regionId);
            expect(reciprocalEdge != graph.region(neighborId).neighborIdToEdgeId.end() &&
                       reciprocalEdge->second == edgeId,
                   "Adjacency map is not reciprocal");
        }
    }

    for (EdgeId edgeId = 0; edgeId < static_cast<EdgeId>(graph.storedEdgeCount()); ++edgeId) {
        const RagEdge &edge = graph.edge(edgeId);
        if (!edge.active) {
            continue;
        }
        expect(graph.isActiveRegion(edge.firstRegionId) && graph.isActiveRegion(edge.secondRegionId),
               "Active edge has an inactive endpoint");
        expect(graph.region(edge.firstRegionId).neighborIdToEdgeId.at(edge.secondRegionId) == edgeId &&
                   graph.region(edge.secondRegionId).neighborIdToEdgeId.at(edge.firstRegionId) == edgeId,
               "Active edge is missing from an adjacency map");
    }
}

void testLocalRegionMergeCombinesSharedInterfaceEvidence() {
    RegionAdjacencyGraph graph;
    graph.addRegion(10);
    graph.addRegion(20);
    graph.addRegion(30);
    const EdgeId mergedEdgeId = graph.addEdge(0, 1, makeEdgeStats(1.0, 2.0, 0.5, 1.0));
    const EdgeId keptEdgeId = graph.addEdge(0, 2, makeEdgeStats(2.0, 3.0, 1.0, 1.5));
    const EdgeId removedDuplicateEdgeId = graph.addEdge(1, 2, makeEdgeStats(4.0, 5.0, 2.0, 2.5));

    const RegionMergeChanges changes = graph.applyRegionMerge({1, 0});

    expect(graph.findRegionIdAfterAppliedMerges(1) == 0, "Merged region does not resolve to its target");
    expect(graph.region(0).voxelCount == 30, "Merged voxel count is incorrect");
    expect(!graph.edge(mergedEdgeId).active, "Contracted interface is still active");
    expect(!graph.edge(removedDuplicateEdgeId).active, "Duplicate interface is still active");
    expect(graph.edge(keptEdgeId).active, "Combined interface was removed");
    expectNear(graph.edge(keptEdgeId).stats.totalBoundarySum, 6.0, "Total boundary sum was not combined");
    expectNear(graph.edge(keptEdgeId).stats.totalContactArea, 8.0, "Total contact area was not combined");
    expectNear(graph.edge(keptEdgeId).stats.openBoundarySum, 3.0, "Open boundary sum was not combined");
    expectNear(graph.edge(keptEdgeId).stats.openContactArea, 4.0, "Open contact area was not combined");
    expect(changes.changedEdgeIds.size() == 1 && changes.changedEdgeIds.front() == keptEdgeId,
           "Local merge did not report exactly the changed interface");
    expect(changes.removedEdgeIds.size() == 2, "Local merge did not report both removed interfaces");
    expectConsistentGraph(graph);
}

double calculateSingleEdgeMergeScore(BoundaryEvidenceStrategy strategy, RagLinkage linkage = RagLinkage::Average,
                                     SizeBiasStrategy sizeBiasStrategy = SizeBiasStrategy::Off) {
    RegionAdjacencyGraph graph;
    graph.addRegion(2);
    graph.addRegion(100);
    const EdgeId edgeId = graph.addEdge(0, 1, makeEdgeStats(0.4, 2.0, 0.1, 1.0));

    WatershedRagAgglomerationOptions options;
    options.boundaryEvidenceStrategy = strategy;
    options.linkage = linkage;
    options.sizeBiasStrategy = sizeBiasStrategy;
    options.sizeBiasThreshold = 10;
    options.sizeBiasStrength = 0.3;
    WatershedRagAgglomerationStats stats;
    RegionMerger merger(graph, options, stats, 1);
    merger.initializeMergeQueue();
    return graph.edge(edgeId).mergeScore;
}

void testMergeScoreStrategies() {
    expectNear(calculateSingleEdgeMergeScore(BoundaryEvidenceStrategy::RawInterfaceMean), 0.3,
               "Raw-interface average score changed");
    expectNear(calculateSingleEdgeMergeScore(BoundaryEvidenceStrategy::OpenInterfaceMean), 0.4,
               "Open-interface average score changed");
    expectNear(calculateSingleEdgeMergeScore(BoundaryEvidenceStrategy::OpenFractionWeighted), 0.2,
               "Open-fraction-weighted score changed");
    expectNear(calculateSingleEdgeMergeScore(BoundaryEvidenceStrategy::RawInterfaceMean, RagLinkage::Sum), 0.6,
               "Sum-linkage score changed");
    expectNear(calculateSingleEdgeMergeScore(BoundaryEvidenceStrategy::RawInterfaceMean, RagLinkage::Average,
                                             SizeBiasStrategy::SoftBias),
               0.54, "Soft size bias changed");
}

RegionAdjacencyGraph makeTwoPairGraph() {
    RegionAdjacencyGraph graph;
    for (int index = 0; index < 4; ++index) {
        graph.addRegion(1);
    }
    graph.addEdge(0, 1, makeEdgeStats(0.1, 1.0, 0.1, 1.0));
    graph.addEdge(2, 3, makeEdgeStats(0.1, 1.0, 0.1, 1.0));
    graph.addEdge(1, 2, makeEdgeStats(0.9, 1.0, 0.9, 1.0));
    return graph;
}

std::vector<int> resultingPartition(RegionAdjacencyGraph &graph) {
    std::vector<int> partition;
    partition.reserve(graph.regionCount());
    for (RegionId regionId = 0; regionId < static_cast<RegionId>(graph.regionCount()); ++regionId) {
        partition.push_back(graph.findRegionIdAfterAppliedMerges(regionId));
    }
    return partition;
}

void testSerialAndBatchedMergingProduceTheSamePartition() {
    WatershedRagAgglomerationOptions options;
    options.boundaryEvidenceStrategy = BoundaryEvidenceStrategy::RawInterfaceMean;

    RegionAdjacencyGraph serialGraph = makeTwoPairGraph();
    WatershedRagAgglomerationStats serialStats;
    RegionMerger serialMerger(serialGraph, options, serialStats, 1);
    serialMerger.initializeMergeQueue();
    serialMerger.mergeGreedily();

    RegionAdjacencyGraph batchedGraph = makeTwoPairGraph();
    WatershedRagAgglomerationStats batchedStats;
    RegionMerger batchedMerger(batchedGraph, options, batchedStats, 4);
    batchedMerger.initializeMergeQueue();
    batchedMerger.mergeInNonOverlappingBatches();

    expect(resultingPartition(serialGraph) == resultingPartition(batchedGraph),
           "Serial and batched merging produced different partitions");
    expect(serialStats.mergeCount == 2 && batchedStats.mergeCount == 2, "Unexpected number of merges");
    expect(batchedStats.batchCount == 1 && batchedStats.maxBatchPairs == 2,
           "Independent merges were not applied in one batch");
    expectConsistentGraph(serialGraph);
    expectConsistentGraph(batchedGraph);
}

void testRegionLookupFollowsMultipleAppliedMerges() {
    RegionAdjacencyGraph graph;
    graph.addRegion();
    graph.addRegion();
    graph.addRegion();
    graph.addEdge(0, 1, makeEdgeStats(0.0, 1.0, 0.0, 1.0));
    graph.addEdge(1, 2, makeEdgeStats(0.0, 1.0, 0.0, 1.0));

    graph.applyRegionMerge({2, 1});
    graph.applyRegionMerge({1, 0});

    expect(graph.findRegionIdAfterAppliedMerges(2) == 0, "Region lookup did not follow the complete merge chain");
    expectConsistentGraph(graph);
}

void testCleanupMergesSmallRegionAcrossAllowedBoundary() {
    RegionAdjacencyGraph graph;
    graph.addRegion(1);
    graph.addRegion(10);
    graph.addRegion(20);
    graph.addEdge(0, 1, makeEdgeStats(0.9, 1.0, 0.9, 1.0));
    graph.addEdge(0, 2, makeEdgeStats(0.7, 1.0, 0.7, 1.0));

    WatershedRagAgglomerationOptions options;
    options.boundaryEvidenceStrategy = BoundaryEvidenceStrategy::RawInterfaceMean;
    options.sizeBiasStrategy = SizeBiasStrategy::Cleanup;
    options.sizeBiasThreshold = 5;
    options.sizeBiasProtection = 0.3;
    WatershedRagAgglomerationStats stats;
    RegionMerger merger(graph, options, stats, 1);
    merger.initializeMergeQueue();
    merger.mergeGreedily();
    merger.mergeRemainingSmallRegions();

    expect(graph.findRegionIdAfterAppliedMerges(0) == 2, "Cleanup did not select the best allowed target region");
    expect(stats.mergeCount == 1 && stats.sizeBiasCleanupMergeCount == 1, "Cleanup merge statistics are incorrect");
    expectConsistentGraph(graph);
}

void testSingleSelectedMergeFallsBackToGreedyMerging() {
    RegionAdjacencyGraph graph;
    graph.addRegion();
    graph.addRegion();
    graph.addEdge(0, 1, makeEdgeStats(0.1, 1.0, 0.1, 1.0));

    WatershedRagAgglomerationOptions options;
    options.boundaryEvidenceStrategy = BoundaryEvidenceStrategy::RawInterfaceMean;
    WatershedRagAgglomerationStats stats;
    RegionMerger merger(graph, options, stats, 4);
    merger.initializeMergeQueue();
    merger.mergeInNonOverlappingBatches();

    expect(stats.mergeCount == 1, "Greedy fallback did not apply the selected merge");
    expect(stats.batchCount == 0, "A single merge was incorrectly counted as a parallel batch");
    expect(graph.findRegionIdAfterAppliedMerges(0) == graph.findRegionIdAfterAppliedMerges(1),
           "Greedy fallback left the regions separate");
    expectConsistentGraph(graph);
}

void testSoftSizeBiasIsUpdatedForTheEnlargedTargetRegion() {
    RegionAdjacencyGraph graph;
    graph.addRegion(6);
    graph.addRegion(5);
    graph.addRegion(100);
    graph.addEdge(0, 1, makeEdgeStats(0.0, 1.0, 0.0, 1.0));
    graph.addEdge(1, 2, makeEdgeStats(0.6, 1.0, 0.6, 1.0));

    WatershedRagAgglomerationOptions options;
    options.boundaryEvidenceStrategy = BoundaryEvidenceStrategy::RawInterfaceMean;
    options.sizeBiasStrategy = SizeBiasStrategy::SoftBias;
    options.sizeBiasThreshold = 10;
    options.sizeBiasStrength = 0.3;
    WatershedRagAgglomerationStats stats;
    RegionMerger merger(graph, options, stats, 1);
    merger.initializeMergeQueue();
    merger.mergeGreedily();

    expect(stats.mergeCount == 1, "An outdated size-bias score caused an additional merge");
    expect(graph.findRegionIdAfterAppliedMerges(0) == 1 && graph.findRegionIdAfterAppliedMerges(2) == 2,
           "Unexpected partition after updating the target-region size bias");
    expectConsistentGraph(graph);
}

} // namespace

int main() {
    try {
        testLocalRegionMergeCombinesSharedInterfaceEvidence();
        testMergeScoreStrategies();
        testSerialAndBatchedMergingProduceTheSamePartition();
        testRegionLookupFollowsMultipleAppliedMerges();
        testCleanupMergesSmallRegionAcrossAllowedBoundary();
        testSingleSelectedMergeFallsBackToGreedyMerging();
        testSoftSizeBiasIsUpdatedForTheEnlargedTargetRegion();
        std::cout << "RAG region-merger tests passed." << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "RAG region-merger test failed: " << error.what() << std::endl;
        return 1;
    }
}
