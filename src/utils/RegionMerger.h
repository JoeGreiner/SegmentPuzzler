#ifndef SEGMENTPUZZLER_REGIONMERGER_H
#define SEGMENTPUZZLER_REGIONMERGER_H

#include "src/utils/RegionAdjacencyGraph.h"
#include "src/utils/WatershedRagAgglomeration.h"

#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

namespace segment_puzzler::rag {

class RegionMerger {
  public:
    RegionMerger(RegionAdjacencyGraph &graph, const WatershedRagAgglomerationOptions &options,
                 WatershedRagAgglomerationStats &stats, int threadCount);

    void initializeMergeQueue();
    void mergeGreedily();
    void mergeInNonOverlappingBatches();
    void mergeRemainingSmallRegions();

    std::size_t countSmallRegions() const;
    std::size_t countSmallRegionsEligibleForCleanup();

  private:
    struct QueuedMergeCandidate {
        double mergeScore = 0.0;
        EdgeId edgeId = invalidEdgeId;
        uint32_t mergeScoreUpdateCountAtQueueInsertion = 0;
    };

    struct HigherMergeScoreFirst {
        bool operator()(const QueuedMergeCandidate &left, const QueuedMergeCandidate &right) const;
    };

    struct SelectedRegionMerge {
        EdgeId edgeId = invalidEdgeId;
        uint32_t mergeScoreUpdateCountAtSelection = 0;
        RegionMerge regionMerge;
    };

    struct SmallRegionMergeCandidate {
        RegionId smallRegionId = invalidRegionId;
        RegionId targetRegionId = invalidRegionId;
        double boundaryScore = 0.0;
        uint64_t smallRegionSize = 0;
        uint64_t targetRegionSize = 0;

        bool isValid() const;
    };

    using MergeCandidateQueue =
        std::priority_queue<QueuedMergeCandidate, std::vector<QueuedMergeCandidate>, HigherMergeScoreFirst>;

    void recalculateMergeScore(EdgeId edgeId);
    void addEdgeToMergeQueueIfAllowed(EdgeId edgeId);
    bool isCurrentCandidate(const QueuedMergeCandidate &candidate);

    std::vector<EdgeId> findEdgesNeedingMergeScoreUpdate(const RegionMerge &appliedMerge,
                                                         const RegionMergeChanges &changes) const;
    void applyRegionMergeAndUpdateScores(const RegionMerge &merge, bool addUpdatedEdgesToQueue);
    std::vector<SelectedRegionMerge> selectNonOverlappingMerges();
    void applySelectedMerges(const std::vector<SelectedRegionMerge> &selectedMerges);

    SmallRegionMergeCandidate findBestCleanupMergeForRegion(RegionId smallRegionId);
    static bool shouldPreferCleanupMerge(const SmallRegionMergeCandidate &candidate,
                                         const SmallRegionMergeCandidate &currentBest);

    RegionAdjacencyGraph &graph_;
    const WatershedRagAgglomerationOptions &options_;
    WatershedRagAgglomerationStats &stats_;
    int threadCount_ = 1;
    MergeCandidateQueue mergeCandidateQueue_;
};

} // namespace segment_puzzler::rag

#endif
