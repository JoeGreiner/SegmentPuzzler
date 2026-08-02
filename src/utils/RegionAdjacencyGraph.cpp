#include "src/utils/RegionAdjacencyGraph.h"

#include <algorithm>
#include <stdexcept>

namespace segment_puzzler::rag {

void RagEdgeStats::add(const RagEdgeStats &other) {
    totalBoundarySum += other.totalBoundarySum;
    totalContactArea += other.totalContactArea;
    openBoundarySum += other.openBoundarySum;
    openContactArea += other.openContactArea;
}

uint64_t makeRegionPairKey(RegionId firstRegionId, RegionId secondRegionId) {
    const uint32_t lowerRegionId = static_cast<uint32_t>(std::min(firstRegionId, secondRegionId));
    const uint32_t higherRegionId = static_cast<uint32_t>(std::max(firstRegionId, secondRegionId));
    return (static_cast<uint64_t>(lowerRegionId) << 32u) | static_cast<uint64_t>(higherRegionId);
}

std::pair<RegionId, RegionId> unpackRegionPairKey(uint64_t key) {
    return {static_cast<RegionId>(key >> 32u), static_cast<RegionId>(key & 0xffffffffu)};
}

void RegionAdjacencyGraph::reserveRegions(std::size_t regionCount) {
    regions_.reserve(regionCount);
}

void RegionAdjacencyGraph::reserveEdges(std::size_t edgeCount) {
    edges_.reserve(edgeCount);
}

RegionId RegionAdjacencyGraph::addRegion(uint64_t voxelCount) {
    const RegionId regionId = static_cast<RegionId>(regions_.size());
    Region newRegion;
    newRegion.parentRegionId = regionId;
    newRegion.voxelCount = voxelCount;
    regions_.push_back(std::move(newRegion));
    return regionId;
}

EdgeId RegionAdjacencyGraph::addEdge(RegionId firstRegionId, RegionId secondRegionId, const RagEdgeStats &stats) {
    if (!isActiveRegion(firstRegionId) || !isActiveRegion(secondRegionId) || firstRegionId == secondRegionId) {
        throw std::invalid_argument("A RAG edge requires two different active regions.");
    }

    RegionId lowerRegionId = std::min(firstRegionId, secondRegionId);
    RegionId higherRegionId = std::max(firstRegionId, secondRegionId);
    if (regions_[lowerRegionId].neighborIdToEdgeId.count(higherRegionId) > 0) {
        throw std::invalid_argument("A RAG edge already exists between the regions.");
    }

    RagEdge newEdge;
    newEdge.firstRegionId = lowerRegionId;
    newEdge.secondRegionId = higherRegionId;
    newEdge.stats = stats;
    const EdgeId edgeId = static_cast<EdgeId>(edges_.size());
    edges_.push_back(newEdge);
    regions_[lowerRegionId].neighborIdToEdgeId.emplace(higherRegionId, edgeId);
    regions_[higherRegionId].neighborIdToEdgeId.emplace(lowerRegionId, edgeId);
    return edgeId;
}

std::size_t RegionAdjacencyGraph::regionCount() const {
    return regions_.size();
}

std::size_t RegionAdjacencyGraph::storedEdgeCount() const {
    return edges_.size();
}

Region &RegionAdjacencyGraph::region(RegionId regionId) {
    if (!isValidRegionId(regionId)) {
        throw std::out_of_range("RAG region ID is out of range.");
    }
    return regions_[static_cast<std::size_t>(regionId)];
}

const Region &RegionAdjacencyGraph::region(RegionId regionId) const {
    if (!isValidRegionId(regionId)) {
        throw std::out_of_range("RAG region ID is out of range.");
    }
    return regions_[static_cast<std::size_t>(regionId)];
}

RagEdge &RegionAdjacencyGraph::edge(EdgeId edgeId) {
    if (!isValidEdgeId(edgeId)) {
        throw std::out_of_range("RAG edge ID is out of range.");
    }
    return edges_[static_cast<std::size_t>(edgeId)];
}

const RagEdge &RegionAdjacencyGraph::edge(EdgeId edgeId) const {
    if (!isValidEdgeId(edgeId)) {
        throw std::out_of_range("RAG edge ID is out of range.");
    }
    return edges_[static_cast<std::size_t>(edgeId)];
}

bool RegionAdjacencyGraph::isActiveRegion(RegionId regionId) const {
    return isValidRegionId(regionId) && regions_[static_cast<std::size_t>(regionId)].active &&
           regions_[static_cast<std::size_t>(regionId)].parentRegionId == regionId;
}

RegionId RegionAdjacencyGraph::findRegionIdAfterAppliedMerges(RegionId regionId) {
    if (!isValidRegionId(regionId)) {
        throw std::out_of_range("RAG region ID is out of range.");
    }

    RegionId resultingRegionId = regionId;
    while (regions_[static_cast<std::size_t>(resultingRegionId)].parentRegionId != resultingRegionId) {
        resultingRegionId = regions_[static_cast<std::size_t>(resultingRegionId)].parentRegionId;
    }
    while (regions_[static_cast<std::size_t>(regionId)].parentRegionId != regionId) {
        const RegionId parentRegionId = regions_[static_cast<std::size_t>(regionId)].parentRegionId;
        regions_[static_cast<std::size_t>(regionId)].parentRegionId = resultingRegionId;
        regionId = parentRegionId;
    }
    return resultingRegionId;
}

RegionId RegionAdjacencyGraph::findRegionIdAfterAppliedMerges(RegionId regionId) const {
    if (!isValidRegionId(regionId)) {
        throw std::out_of_range("RAG region ID is out of range.");
    }

    while (regions_[static_cast<std::size_t>(regionId)].parentRegionId != regionId) {
        regionId = regions_[static_cast<std::size_t>(regionId)].parentRegionId;
    }
    return regionId;
}

RegionMerge RegionAdjacencyGraph::chooseRegionMergeDirection(RegionId firstRegionId, RegionId secondRegionId) const {
    if (!isActiveRegion(firstRegionId) || !isActiveRegion(secondRegionId) || firstRegionId == secondRegionId) {
        throw std::invalid_argument("A region merge requires two different active regions.");
    }

    RegionId targetRegionId = firstRegionId;
    RegionId mergedRegionId = secondRegionId;
    const Region &initialTarget = regions_[static_cast<std::size_t>(targetRegionId)];
    const Region &initialMergedRegion = regions_[static_cast<std::size_t>(mergedRegionId)];
    const bool mergedRegionIsBetterTarget =
        initialTarget.neighborIdToEdgeId.size() < initialMergedRegion.neighborIdToEdgeId.size() ||
        (initialTarget.neighborIdToEdgeId.size() == initialMergedRegion.neighborIdToEdgeId.size() &&
         initialTarget.treeRank < initialMergedRegion.treeRank) ||
        (initialTarget.neighborIdToEdgeId.size() == initialMergedRegion.neighborIdToEdgeId.size() &&
         initialTarget.treeRank == initialMergedRegion.treeRank && targetRegionId > mergedRegionId);
    if (mergedRegionIsBetterTarget) {
        std::swap(targetRegionId, mergedRegionId);
    }
    return {mergedRegionId, targetRegionId};
}

RegionMergeChanges RegionAdjacencyGraph::applyRegionMerge(const RegionMerge &merge) {
    if (!isActiveRegion(merge.mergedRegionId) || !isActiveRegion(merge.targetRegionId) ||
        merge.mergedRegionId == merge.targetRegionId) {
        throw std::invalid_argument("Cannot apply a merge to inactive or identical regions.");
    }

    Region &mergedRegion = regions_[static_cast<std::size_t>(merge.mergedRegionId)];
    Region &targetRegion = regions_[static_cast<std::size_t>(merge.targetRegionId)];
    const auto sharedEdge = targetRegion.neighborIdToEdgeId.find(merge.mergedRegionId);
    if (sharedEdge == targetRegion.neighborIdToEdgeId.end()) {
        throw std::invalid_argument("A region merge requires two adjacent regions.");
    }

    mergedRegion.parentRegionId = merge.targetRegionId;
    if (targetRegion.treeRank == mergedRegion.treeRank) {
        ++targetRegion.treeRank;
    }
    targetRegion.voxelCount += mergedRegion.voxelCount;

    RegionMergeChanges changes;
    markEdgeAsRemoved(sharedEdge->second, changes);
    targetRegion.neighborIdToEdgeId.erase(sharedEdge);
    mergedRegion.neighborIdToEdgeId.erase(merge.targetRegionId);

    std::vector<std::pair<RegionId, EdgeId>> mergedRegionNeighbors;
    mergedRegionNeighbors.reserve(mergedRegion.neighborIdToEdgeId.size());
    for (const auto &neighborAndEdge : mergedRegion.neighborIdToEdgeId) {
        mergedRegionNeighbors.push_back(neighborAndEdge);
    }

    for (const auto &neighborAndEdge : mergedRegionNeighbors) {
        const RegionId neighborRegionId = findRegionIdAfterAppliedMerges(neighborAndEdge.first);
        const EdgeId mergedRegionEdgeId = neighborAndEdge.second;
        if (!isValidEdgeId(mergedRegionEdgeId) || !edges_[static_cast<std::size_t>(mergedRegionEdgeId)].active) {
            continue;
        }
        if (neighborRegionId == merge.targetRegionId) {
            markEdgeAsRemoved(mergedRegionEdgeId, changes);
            continue;
        }

        auto existingTargetEdge = targetRegion.neighborIdToEdgeId.find(neighborRegionId);
        if (existingTargetEdge != targetRegion.neighborIdToEdgeId.end()) {
            const EdgeId targetEdgeId = existingTargetEdge->second;
            RagEdge &targetEdge = edges_[static_cast<std::size_t>(targetEdgeId)];
            targetEdge.stats.add(edges_[static_cast<std::size_t>(mergedRegionEdgeId)].stats);
            targetEdge.firstRegionId = std::min(merge.targetRegionId, neighborRegionId);
            targetEdge.secondRegionId = std::max(merge.targetRegionId, neighborRegionId);
            markEdgeAsRemoved(mergedRegionEdgeId, changes);

            Region &neighborRegion = regions_[static_cast<std::size_t>(neighborRegionId)];
            neighborRegion.neighborIdToEdgeId.erase(merge.mergedRegionId);
            neighborRegion.neighborIdToEdgeId[merge.targetRegionId] = targetEdgeId;
            changes.changedEdgeIds.push_back(targetEdgeId);
        } else {
            RagEdge &movedEdge = edges_[static_cast<std::size_t>(mergedRegionEdgeId)];
            movedEdge.firstRegionId = std::min(merge.targetRegionId, neighborRegionId);
            movedEdge.secondRegionId = std::max(merge.targetRegionId, neighborRegionId);
            targetRegion.neighborIdToEdgeId[neighborRegionId] = mergedRegionEdgeId;

            Region &neighborRegion = regions_[static_cast<std::size_t>(neighborRegionId)];
            neighborRegion.neighborIdToEdgeId.erase(merge.mergedRegionId);
            neighborRegion.neighborIdToEdgeId[merge.targetRegionId] = mergedRegionEdgeId;
            changes.changedEdgeIds.push_back(mergedRegionEdgeId);
        }
    }

    mergedRegion.neighborIdToEdgeId.clear();
    mergedRegion.active = false;

    std::sort(changes.changedEdgeIds.begin(), changes.changedEdgeIds.end());
    changes.changedEdgeIds.erase(std::unique(changes.changedEdgeIds.begin(), changes.changedEdgeIds.end()),
                                 changes.changedEdgeIds.end());
    std::sort(changes.removedEdgeIds.begin(), changes.removedEdgeIds.end());
    changes.removedEdgeIds.erase(std::unique(changes.removedEdgeIds.begin(), changes.removedEdgeIds.end()),
                                 changes.removedEdgeIds.end());
    return changes;
}

bool RegionAdjacencyGraph::isValidRegionId(RegionId regionId) const {
    return regionId >= 0 && static_cast<std::size_t>(regionId) < regions_.size();
}

bool RegionAdjacencyGraph::isValidEdgeId(EdgeId edgeId) const {
    return edgeId >= 0 && static_cast<std::size_t>(edgeId) < edges_.size();
}

void RegionAdjacencyGraph::markEdgeAsRemoved(EdgeId edgeId, RegionMergeChanges &changes) {
    if (!isValidEdgeId(edgeId)) {
        return;
    }
    RagEdge &removedEdge = edges_[static_cast<std::size_t>(edgeId)];
    if (!removedEdge.active) {
        return;
    }
    removedEdge.active = false;
    changes.removedEdgeIds.push_back(edgeId);
}

} // namespace segment_puzzler::rag
