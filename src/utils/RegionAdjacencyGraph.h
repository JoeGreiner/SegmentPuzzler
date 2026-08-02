#ifndef SEGMENTPUZZLER_REGIONADJACENCYGRAPH_H
#define SEGMENTPUZZLER_REGIONADJACENCYGRAPH_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace segment_puzzler::rag {

using RegionId = int;
using EdgeId = int;

constexpr RegionId invalidRegionId = -1;
constexpr EdgeId invalidEdgeId = -1;

struct RagEdgeStats {
    double totalBoundarySum = 0.0;
    double totalContactArea = 0.0;
    double openBoundarySum = 0.0;
    double openContactArea = 0.0;

    void add(const RagEdgeStats &other);
};

struct RagEdge {
    RegionId firstRegionId = invalidRegionId;
    RegionId secondRegionId = invalidRegionId;
    RagEdgeStats stats;

    double selectedContactArea = 0.0;
    double selectedMeanScore = 0.0;
    double rawContactArea = 0.0;
    double rawMeanScore = 0.0;
    double mergeScore = 0.0;
    uint32_t mergeScoreUpdateCount = 0;
    bool active = true;
};

struct Region {
    RegionId parentRegionId = invalidRegionId;
    uint32_t treeRank = 0;
    uint64_t voxelCount = 0;
    bool active = true;
    std::unordered_map<RegionId, EdgeId> neighborIdToEdgeId;
};

struct RegionMerge {
    RegionId mergedRegionId = invalidRegionId;
    RegionId targetRegionId = invalidRegionId;
};

struct RegionMergeChanges {
    std::vector<EdgeId> changedEdgeIds;
    std::vector<EdgeId> removedEdgeIds;
};

uint64_t makeRegionPairKey(RegionId firstRegionId, RegionId secondRegionId);
std::pair<RegionId, RegionId> unpackRegionPairKey(uint64_t key);

class RegionAdjacencyGraph {
  public:
    void reserveRegions(std::size_t regionCount);
    void reserveEdges(std::size_t edgeCount);

    RegionId addRegion(uint64_t voxelCount = 0);
    EdgeId addEdge(RegionId firstRegionId, RegionId secondRegionId, const RagEdgeStats &stats);

    std::size_t regionCount() const;
    std::size_t storedEdgeCount() const;

    Region &region(RegionId regionId);
    const Region &region(RegionId regionId) const;
    RagEdge &edge(EdgeId edgeId);
    const RagEdge &edge(EdgeId edgeId) const;

    bool isActiveRegion(RegionId regionId) const;
    RegionId findRegionIdAfterAppliedMerges(RegionId regionId);
    RegionId findRegionIdAfterAppliedMerges(RegionId regionId) const;

    RegionMerge chooseRegionMergeDirection(RegionId firstRegionId, RegionId secondRegionId) const;
    RegionMergeChanges applyRegionMerge(const RegionMerge &merge);

  private:
    bool isValidRegionId(RegionId regionId) const;
    bool isValidEdgeId(EdgeId edgeId) const;
    void markEdgeAsRemoved(EdgeId edgeId, RegionMergeChanges &changes);

    std::vector<Region> regions_;
    std::vector<RagEdge> edges_;
};

} // namespace segment_puzzler::rag

#endif
