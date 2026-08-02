#include "src/utils/RegionMerger.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace segment_puzzler::rag {
namespace {

struct BoundaryScore {
    double signedSum = 0.0;
    double contactArea = 0.0;
    double meanScore = 0.0;
};

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

bool softSizeBiasIsEnabled(const WatershedRagAgglomerationOptions &options) {
    return options.sizeBiasStrategy == SizeBiasStrategy::SoftBias ||
           options.sizeBiasStrategy == SizeBiasStrategy::SoftBiasAndCleanup;
}

bool smallRegionCleanupIsEnabled(const WatershedRagAgglomerationOptions &options) {
    return options.sizeBiasStrategy == SizeBiasStrategy::Cleanup ||
           options.sizeBiasStrategy == SizeBiasStrategy::SoftBiasAndCleanup;
}

double contactAreaUsedForSizeBias(const RagEdge &edge, const WatershedRagAgglomerationOptions &options) {
    if (!options.sizeBiasRespectMask) {
        return edge.rawContactArea;
    }
    if (options.boundaryEvidenceStrategy == BoundaryEvidenceStrategy::RawInterfaceMean) {
        return edge.stats.totalContactArea;
    }
    return edge.stats.openContactArea;
}

double boundaryScoreUsedForCleanup(const RagEdge &edge, const WatershedRagAgglomerationOptions &options) {
    return options.sizeBiasRespectMask ? edge.selectedMeanScore : edge.rawMeanScore;
}

BoundaryScore calculateBoundaryScore(const RagEdgeStats &edgeStats, BoundaryEvidenceStrategy strategy,
                                     double boundaryThreshold) {
    BoundaryScore result;
    switch (strategy) {
        case BoundaryEvidenceStrategy::RawInterfaceMean: {
            if (edgeStats.totalContactArea <= 0.0) {
                return result;
            }
            const double meanBoundary = edgeStats.totalBoundarySum / edgeStats.totalContactArea;
            result.signedSum = edgeStats.totalContactArea * (boundaryThreshold - meanBoundary);
            result.contactArea = edgeStats.totalContactArea;
            break;
        }
        case BoundaryEvidenceStrategy::OpenInterfaceMean: {
            if (edgeStats.openContactArea <= 0.0) {
                return result;
            }
            const double meanBoundary = edgeStats.openBoundarySum / edgeStats.openContactArea;
            result.signedSum = edgeStats.openContactArea * (boundaryThreshold - meanBoundary);
            result.contactArea = edgeStats.openContactArea;
            break;
        }
        case BoundaryEvidenceStrategy::OpenFractionWeighted: {
            if (edgeStats.openContactArea <= 0.0) {
                return result;
            }
            const double meanBoundary = edgeStats.openBoundarySum / edgeStats.openContactArea;
            result.signedSum = edgeStats.openContactArea * (boundaryThreshold - meanBoundary);
            result.contactArea = edgeStats.totalContactArea;
            break;
        }
    }
    if (result.contactArea > 0.0) {
        result.meanScore = result.signedSum / result.contactArea;
    }
    return result;
}

double calculateSmallness(uint64_t voxelCount, uint64_t sizeThreshold) {
    if (sizeThreshold == 0 || voxelCount >= sizeThreshold) {
        return 0.0;
    }
    return std::clamp(static_cast<double>(sizeThreshold - voxelCount) / static_cast<double>(sizeThreshold), 0.0, 1.0);
}

double calculateLinkageScore(const BoundaryScore &boundaryScore, RagLinkage linkage) {
    if (linkage == RagLinkage::Sum) {
        return boundaryScore.signedSum;
    }
    return boundaryScore.contactArea > 0.0 ? boundaryScore.meanScore : 0.0;
}

} // namespace

RegionMerger::RegionMerger(RegionAdjacencyGraph &graph, const WatershedRagAgglomerationOptions &options,
                           WatershedRagAgglomerationStats &stats, int threadCount)
    : graph_(graph), options_(options), stats_(stats), threadCount_(std::max(1, threadCount)) {
}

bool RegionMerger::HigherMergeScoreFirst::operator()(const QueuedMergeCandidate &left,
                                                     const QueuedMergeCandidate &right) const {
    if (left.mergeScore != right.mergeScore) {
        return left.mergeScore < right.mergeScore;
    }
    return left.edgeId > right.edgeId;
}

bool RegionMerger::SmallRegionMergeCandidate::isValid() const {
    return smallRegionId != invalidRegionId && targetRegionId != invalidRegionId;
}

void RegionMerger::initializeMergeQueue() {
    mergeCandidateQueue_ = MergeCandidateQueue();
    for (EdgeId edgeId = 0; edgeId < static_cast<EdgeId>(graph_.storedEdgeCount()); ++edgeId) {
        RagEdge &edge = graph_.edge(edgeId);
        if (!edge.active) {
            continue;
        }
        recalculateMergeScore(edgeId);
        addEdgeToMergeQueueIfAllowed(edgeId);
    }
}

void RegionMerger::recalculateMergeScore(EdgeId edgeId) {
    RagEdge &edge = graph_.edge(edgeId);
    if (!edge.active) {
        return;
    }

    const RegionAdjacencyGraph &readOnlyGraph = graph_;
    const RegionId firstRegionId = readOnlyGraph.findRegionIdAfterAppliedMerges(edge.firstRegionId);
    const RegionId secondRegionId = readOnlyGraph.findRegionIdAfterAppliedMerges(edge.secondRegionId);
    if (firstRegionId == secondRegionId) {
        edge.active = false;
        return;
    }
    edge.firstRegionId = std::min(firstRegionId, secondRegionId);
    edge.secondRegionId = std::max(firstRegionId, secondRegionId);

    const BoundaryScore selectedBoundaryScore =
        calculateBoundaryScore(edge.stats, options_.boundaryEvidenceStrategy, options_.tau);
    const BoundaryScore rawBoundaryScore =
        calculateBoundaryScore(edge.stats, BoundaryEvidenceStrategy::RawInterfaceMean, options_.tau);
    edge.selectedContactArea = selectedBoundaryScore.contactArea;
    edge.selectedMeanScore = selectedBoundaryScore.meanScore;
    edge.rawContactArea = rawBoundaryScore.contactArea;
    edge.rawMeanScore = rawBoundaryScore.meanScore;

    double sizeBias = 0.0;
    const double sizeBiasContactArea = contactAreaUsedForSizeBias(edge, options_);
    if (softSizeBiasIsEnabled(options_) && sizeBiasContactArea > 0.0) {
        const double firstRegionSmallness =
            calculateSmallness(graph_.region(edge.firstRegionId).voxelCount, options_.sizeBiasThreshold);
        const double secondRegionSmallness =
            calculateSmallness(graph_.region(edge.secondRegionId).voxelCount, options_.sizeBiasThreshold);
        sizeBias = options_.sizeBiasStrength * std::max(firstRegionSmallness, secondRegionSmallness);
        if (options_.linkage == RagLinkage::Sum) {
            sizeBias *= sizeBiasContactArea;
        }
    }
    edge.mergeScore = calculateLinkageScore(selectedBoundaryScore, options_.linkage) + sizeBias;
    ++edge.mergeScoreUpdateCount;
}

void RegionMerger::addEdgeToMergeQueueIfAllowed(EdgeId edgeId) {
    const RagEdge &edge = graph_.edge(edgeId);
    if (!edge.active || edge.mergeScore <= 0.0) {
        return;
    }
    mergeCandidateQueue_.push({edge.mergeScore, edgeId, edge.mergeScoreUpdateCount});
}

bool RegionMerger::isCurrentCandidate(const QueuedMergeCandidate &candidate) {
    if (candidate.edgeId < 0 || static_cast<std::size_t>(candidate.edgeId) >= graph_.storedEdgeCount()) {
        ++stats_.outdatedMergeCandidateCount;
        return false;
    }
    const RagEdge &edge = graph_.edge(candidate.edgeId);
    if (!edge.active || candidate.mergeScoreUpdateCountAtQueueInsertion != edge.mergeScoreUpdateCount) {
        ++stats_.outdatedMergeCandidateCount;
        return false;
    }
    return true;
}

std::vector<EdgeId> RegionMerger::findEdgesNeedingMergeScoreUpdate(const RegionMerge &appliedMerge,
                                                                   const RegionMergeChanges &changes) const {
    std::vector<EdgeId> edgeIds = changes.changedEdgeIds;
    if (softSizeBiasIsEnabled(options_)) {
        const Region &targetRegion = graph_.region(appliedMerge.targetRegionId);
        edgeIds.reserve(edgeIds.size() + targetRegion.neighborIdToEdgeId.size());
        for (const auto &neighborAndEdge : targetRegion.neighborIdToEdgeId) {
            edgeIds.push_back(neighborAndEdge.second);
        }
    }
    std::sort(edgeIds.begin(), edgeIds.end());
    edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());
    return edgeIds;
}

void RegionMerger::applyRegionMergeAndUpdateScores(const RegionMerge &merge, bool addUpdatedEdgesToQueue) {
    const RegionMergeChanges changes = graph_.applyRegionMerge(merge);
    const std::vector<EdgeId> edgeIdsToUpdate = findEdgesNeedingMergeScoreUpdate(merge, changes);
    ++stats_.mergeCount;
    stats_.updatedRagEdgeCount += edgeIdsToUpdate.size();
    for (EdgeId edgeId : edgeIdsToUpdate) {
        recalculateMergeScore(edgeId);
        if (addUpdatedEdgesToQueue) {
            addEdgeToMergeQueueIfAllowed(edgeId);
        }
    }
}

void RegionMerger::mergeGreedily() {
    while (!mergeCandidateQueue_.empty()) {
        const QueuedMergeCandidate candidate = mergeCandidateQueue_.top();
        mergeCandidateQueue_.pop();
        if (!isCurrentCandidate(candidate)) {
            continue;
        }

        RagEdge &edge = graph_.edge(candidate.edgeId);
        const RegionId firstRegionId = graph_.findRegionIdAfterAppliedMerges(edge.firstRegionId);
        const RegionId secondRegionId = graph_.findRegionIdAfterAppliedMerges(edge.secondRegionId);
        if (firstRegionId == secondRegionId) {
            edge.active = false;
            continue;
        }
        if (edge.mergeScore <= 0.0) {
            break;
        }

        const RegionMerge merge = graph_.chooseRegionMergeDirection(firstRegionId, secondRegionId);
        applyRegionMergeAndUpdateScores(merge, /*addUpdatedEdgesToQueue=*/true);
    }
}

std::vector<RegionMerger::SelectedRegionMerge> RegionMerger::selectNonOverlappingMerges() {
    const double selectionStart = currentTimeSeconds();
    std::vector<SelectedRegionMerge> selectedMerges;
    std::vector<unsigned char> regionAlreadySelected(graph_.regionCount(), 0);
    std::vector<QueuedMergeCandidate> deferredCandidates;

    while (!mergeCandidateQueue_.empty()) {
        const QueuedMergeCandidate candidate = mergeCandidateQueue_.top();
        mergeCandidateQueue_.pop();
        if (!isCurrentCandidate(candidate)) {
            continue;
        }

        RagEdge &edge = graph_.edge(candidate.edgeId);
        const RegionId firstRegionId = graph_.findRegionIdAfterAppliedMerges(edge.firstRegionId);
        const RegionId secondRegionId = graph_.findRegionIdAfterAppliedMerges(edge.secondRegionId);
        if (firstRegionId == secondRegionId) {
            edge.active = false;
            continue;
        }
        if (edge.mergeScore <= 0.0) {
            break;
        }
        if (regionAlreadySelected[static_cast<std::size_t>(firstRegionId)] != 0 ||
            regionAlreadySelected[static_cast<std::size_t>(secondRegionId)] != 0) {
            deferredCandidates.push_back(candidate);
            continue;
        }

        const RegionMerge merge = graph_.chooseRegionMergeDirection(firstRegionId, secondRegionId);
        selectedMerges.push_back({candidate.edgeId, edge.mergeScoreUpdateCount, merge});
        regionAlreadySelected[static_cast<std::size_t>(firstRegionId)] = 1;
        regionAlreadySelected[static_cast<std::size_t>(secondRegionId)] = 1;
    }

    for (const QueuedMergeCandidate &candidate : deferredCandidates) {
        mergeCandidateQueue_.push(candidate);
    }
    stats_.batchSelectionMs += elapsedMilliseconds(selectionStart);
    stats_.maxBatchPairs = std::max(stats_.maxBatchPairs, selectedMerges.size());
    return selectedMerges;
}

void RegionMerger::applySelectedMerges(const std::vector<SelectedRegionMerge> &selectedMerges) {
    const double applyStart = currentTimeSeconds();
    const double regionMergeStart = currentTimeSeconds();
    std::vector<EdgeId> changedEdgeIds;

    for (const SelectedRegionMerge &selectedMerge : selectedMerges) {
        const RagEdge &selectedEdge = graph_.edge(selectedMerge.edgeId);
        if (!selectedEdge.active ||
            selectedEdge.mergeScoreUpdateCount != selectedMerge.mergeScoreUpdateCountAtSelection) {
            continue;
        }
        const RegionMergeChanges changes = graph_.applyRegionMerge(selectedMerge.regionMerge);
        ++stats_.mergeCount;
        changedEdgeIds.insert(changedEdgeIds.end(), changes.changedEdgeIds.begin(), changes.changedEdgeIds.end());
    }
    stats_.batchRegionMergeMs += elapsedMilliseconds(regionMergeStart);

    std::sort(changedEdgeIds.begin(), changedEdgeIds.end());
    changedEdgeIds.erase(std::unique(changedEdgeIds.begin(), changedEdgeIds.end()), changedEdgeIds.end());
    stats_.updatedRagEdgeCount += changedEdgeIds.size();

    const double scoreUpdateStart = currentTimeSeconds();
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount_) if (changedEdgeIds.size() >= 4096) schedule(static)
#endif
    for (long long index = 0; index < static_cast<long long>(changedEdgeIds.size()); ++index) {
        recalculateMergeScore(changedEdgeIds[static_cast<std::size_t>(index)]);
    }
    for (EdgeId edgeId : changedEdgeIds) {
        addEdgeToMergeQueueIfAllowed(edgeId);
    }
    stats_.batchMergeScoreUpdateMs += elapsedMilliseconds(scoreUpdateStart);
    stats_.batchApplyMs += elapsedMilliseconds(applyStart);
}

void RegionMerger::mergeInNonOverlappingBatches() {
    while (true) {
        const std::vector<SelectedRegionMerge> selectedMerges = selectNonOverlappingMerges();
        if (selectedMerges.empty()) {
            return;
        }
        if (selectedMerges.size() == 1) {
            const SelectedRegionMerge &selectedMerge = selectedMerges.front();
            const RagEdge &edge = graph_.edge(selectedMerge.edgeId);
            mergeCandidateQueue_.push(
                {edge.mergeScore, selectedMerge.edgeId, selectedMerge.mergeScoreUpdateCountAtSelection});
            mergeGreedily();
            return;
        }
        applySelectedMerges(selectedMerges);
        ++stats_.batchCount;
    }
}

RegionMerger::SmallRegionMergeCandidate RegionMerger::findBestCleanupMergeForRegion(RegionId smallRegionId) {
    SmallRegionMergeCandidate best;
    if (!graph_.isActiveRegion(smallRegionId) ||
        graph_.region(smallRegionId).voxelCount >= options_.sizeBiasThreshold) {
        return best;
    }

    const Region &smallRegion = graph_.region(smallRegionId);
    for (const auto &neighborAndEdge : smallRegion.neighborIdToEdgeId) {
        const RegionId targetRegionId = graph_.findRegionIdAfterAppliedMerges(neighborAndEdge.first);
        if (targetRegionId == smallRegionId) {
            continue;
        }
        const RagEdge &edge = graph_.edge(neighborAndEdge.second);
        const double evidenceContactArea = contactAreaUsedForSizeBias(edge, options_);
        if (!edge.active || evidenceContactArea <= 0.0) {
            continue;
        }

        const double boundaryScore = boundaryScoreUsedForCleanup(edge, options_);
        if (boundaryScore <= -options_.sizeBiasProtection) {
            continue;
        }

        const uint64_t targetRegionSize = graph_.region(targetRegionId).voxelCount;
        if (!best.isValid() || boundaryScore > best.boundaryScore ||
            (boundaryScore == best.boundaryScore && targetRegionSize > best.targetRegionSize) ||
            (boundaryScore == best.boundaryScore && targetRegionSize == best.targetRegionSize &&
             targetRegionId < best.targetRegionId)) {
            best.smallRegionId = smallRegionId;
            best.targetRegionId = targetRegionId;
            best.boundaryScore = boundaryScore;
            best.smallRegionSize = smallRegion.voxelCount;
            best.targetRegionSize = targetRegionSize;
        }
    }
    return best;
}

bool RegionMerger::shouldPreferCleanupMerge(const SmallRegionMergeCandidate &candidate,
                                            const SmallRegionMergeCandidate &currentBest) {
    if (!currentBest.isValid()) {
        return true;
    }
    if (candidate.smallRegionSize != currentBest.smallRegionSize) {
        return candidate.smallRegionSize < currentBest.smallRegionSize;
    }
    if (candidate.boundaryScore != currentBest.boundaryScore) {
        return candidate.boundaryScore > currentBest.boundaryScore;
    }
    if (candidate.smallRegionId != currentBest.smallRegionId) {
        return candidate.smallRegionId < currentBest.smallRegionId;
    }
    if (candidate.targetRegionSize != currentBest.targetRegionSize) {
        return candidate.targetRegionSize > currentBest.targetRegionSize;
    }
    return candidate.targetRegionId < currentBest.targetRegionId;
}

void RegionMerger::mergeRemainingSmallRegions() {
    if (!smallRegionCleanupIsEnabled(options_)) {
        return;
    }

    while (true) {
        SmallRegionMergeCandidate best;
        for (RegionId regionId = 0; regionId < static_cast<RegionId>(graph_.regionCount()); ++regionId) {
            const SmallRegionMergeCandidate candidate = findBestCleanupMergeForRegion(regionId);
            if (candidate.isValid() && shouldPreferCleanupMerge(candidate, best)) {
                best = candidate;
            }
        }
        if (!best.isValid()) {
            return;
        }

        applyRegionMergeAndUpdateScores({best.smallRegionId, best.targetRegionId},
                                        /*addUpdatedEdgesToQueue=*/false);
        ++stats_.sizeBiasCleanupMergeCount;
    }
}

std::size_t RegionMerger::countSmallRegions() const {
    std::size_t count = 0;
    for (RegionId regionId = 0; regionId < static_cast<RegionId>(graph_.regionCount()); ++regionId) {
        if (graph_.isActiveRegion(regionId) && graph_.region(regionId).voxelCount < options_.sizeBiasThreshold) {
            ++count;
        }
    }
    return count;
}

std::size_t RegionMerger::countSmallRegionsEligibleForCleanup() {
    std::size_t count = 0;
    for (RegionId regionId = 0; regionId < static_cast<RegionId>(graph_.regionCount()); ++regionId) {
        if (findBestCleanupMergeForRegion(regionId).isValid()) {
            ++count;
        }
    }
    return count;
}

} // namespace segment_puzzler::rag
