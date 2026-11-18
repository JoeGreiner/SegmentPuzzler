#include "src/image_storage/LabelImageStorage.h"
#include <itkImageRegionConstIterator.h>
#include <itkMultiThreaderBase.h>
#include <itkImageRegionIterator.h>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include "Graph.h"
#include "LabelContacts.h"
#include <itkImageFileWriter.h>
#include <algorithm>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <itkNeighborhoodIterator.h>
#include <itkBinaryThresholdImageFunction.h>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <chrono>
#include <QStringList>
#include "src/utils/AppLogger.h"
#include "src/utils/MinMaxValues.h"
#include "src/utils/systemStats.h"
#include "src/utils/utils.h"
#include "src/viewers/VolumeRegionIO.h"
#include "graphBase.h"

namespace {

using segment_puzzler::app_logging::AppLogger;
using segment_puzzler::app_logging::LogLevel;

const QString kSegmentationCategory = QStringLiteral("segmentation");
const QString kIoCategory = QStringLiteral("io");

void logGraph(LogLevel level, const char *functionName, const QString &message, const QString &category = kSegmentationCategory) {
    AppLogger::log(level, category, message, functionName);
}

void logGraphDebugIf(bool enabled, const char *functionName, const QString &message) {
    if (!enabled) {
        return;
    }
    logGraph(LogLevel::Debug, functionName, message);
}

bool isGraphDebugLoggingEnabled(bool verbose) {
    if (!verbose) {
        return false;
    }
    const auto settings = AppLogger::settings();
    return static_cast<int>(settings.minimumLevel) <= static_cast<int>(LogLevel::Debug) &&
           settings.categoryEnabled.value(kSegmentationCategory, true) &&
           (settings.consoleEnabled || settings.fileEnabled);
}

template<typename Container>
QString joinIds(const Container &values) {
    QStringList parts;
    for (const auto &value : values) {
        parts << QString::number(static_cast<qulonglong>(value));
    }
    return parts.join(QStringLiteral(" "));
}

class ScopedGraphTimer {
public:
    ScopedGraphTimer(bool enabled, const char *functionName, QString operation, QString category = kSegmentationCategory)
        : enabled_(enabled), functionName_(functionName), operation_(std::move(operation)), category_(std::move(category)) {
        if (!enabled_) {
            return;
        }
        AppLogger::log(LogLevel::Debug, category_, operation_ + QStringLiteral(" started"), functionName_);
        timer_.start();
    }

    ~ScopedGraphTimer() {
        if (!enabled_) {
            return;
        }
        const double elapsedMs = static_cast<double>(timer_.nsecsElapsed()) / 1000000.0;
        AppLogger::log(
            LogLevel::Debug,
            category_,
            QStringLiteral("%1 finished (%2 ms)").arg(operation_).arg(elapsedMs, 0, 'f', 3),
            functionName_);
    }

private:
    bool enabled_ = false;
    const char *functionName_ = nullptr;
    QString operation_;
    QString category_;
    QElapsedTimer timer_;
};

bool regionsMatch(const Graph::SegmentsImageType::RegionType &left,
                  const Graph::SegmentsImageType::RegionType &right) {
    return left.GetIndex() == right.GetIndex() && left.GetSize() == right.GetSize();
}

enum class ComponentMatch {
    Exact,
    Different,
    Invalid
};

ComponentMatch selectedComponentMatchesWorkingNode(
        const dataType::SegmentsStorage::Pointer &selectedSegmentation,
        Graph::SegmentIdType selectedLabel,
        const Graph::SegmentsImageType::IndexType &seed,
        const WorkingNode &workingNode,
        std::optional<std::size_t> knownSelectedLabelVoxelCount = std::nullopt) {
    std::size_t workingVoxelCount = 0;
    for (const auto &initialNodeEntry : workingNode.subInitialNodes) {
        const auto &initialNode = initialNodeEntry.second;
        if (initialNode == nullptr ||
            initialNode->voxels.size() > std::numeric_limits<std::size_t>::max() - workingVoxelCount) {
            return ComponentMatch::Invalid;
        }
        workingVoxelCount += initialNode->voxels.size();
    }
    if (workingVoxelCount == 0) {
        return ComponentMatch::Invalid;
    }

    const auto region = selectedSegmentation->GetLargestPossibleRegion();
    if (!region.IsInside(seed) ||
        selectedSegmentation->GetPixel(seed) != selectedLabel) {
        return ComponentMatch::Invalid;
    }

    const auto linearIndex = [&selectedSegmentation](const Graph::SegmentsImageType::IndexType &index) {
        return static_cast<std::ptrdiff_t>(selectedSegmentation->ComputeOffset(index));
    };

    if (knownSelectedLabelVoxelCount.has_value()) {
        if (workingVoxelCount != *knownSelectedLabelVoxelCount) {
            return ComponentMatch::Different;
        }
        // The plan already counted the selected label globally. If the
        // WorkingNode has the same cardinality and every metadata voxel belongs
        // to that label, both voxel sets are identical. This avoids rebuilding
        // the component in a large hash set for every involved label.
        for (const auto &initialNodeEntry : workingNode.subInitialNodes) {
            for (const Voxel &voxel : initialNodeEntry.second->voxels) {
                const Graph::SegmentsImageType::IndexType index{{voxel.x, voxel.y, voxel.z}};
                if (!region.IsInside(index)) {
                    return ComponentMatch::Invalid;
                }
                if (selectedSegmentation->GetPixel(index) != selectedLabel) {
                    return ComponentMatch::Different;
                }
            }
        }
        return ComponentMatch::Exact;
    }

    std::unordered_set<std::ptrdiff_t> unmatchedWorkingVoxels;
    unmatchedWorkingVoxels.reserve(workingVoxelCount);
    for (const auto &initialNodeEntry : workingNode.subInitialNodes) {
        for (const auto &voxel : initialNodeEntry.second->voxels) {
            const Graph::SegmentsImageType::IndexType index{{voxel.x, voxel.y, voxel.z}};
            if (!region.IsInside(index)) {
                return ComponentMatch::Invalid;
            }
            if (!unmatchedWorkingVoxels.insert(linearIndex(index)).second) {
                return ComponentMatch::Invalid;
            }
        }
    }

    segment_puzzler::connected_components::ConnectedComponentVisitResult traversal;
    try {
        traversal = segment_puzzler::connected_components::visitLabelComponent(
            selectedSegmentation,
            seed,
            segment_puzzler::connected_components::ConnectivityStencil::SixConnected,
            [&](const Graph::SegmentsImageType::IndexType &index) {
                return unmatchedWorkingVoxels.erase(linearIndex(index)) > 0;
            });
    } catch (...) {
        return ComponentMatch::Invalid;
    }
    if (!traversal.completed || traversal.voxelCount != workingVoxelCount ||
        !unmatchedWorkingVoxels.empty()) {
        return ComponentMatch::Different;
    }
    return ComponentMatch::Exact;
}

struct NeighborMergeLabelStats {
    std::size_t voxelCount = 0;
    Graph::SegmentsImageType::IndexType seed{};
    Graph::SegmentsImageType::IndexType minimum{};
    Graph::SegmentsImageType::IndexType maximum{};
    bool hasSeed = false;

    Graph::SegmentsImageType::RegionType boundingRegion() const {
        Graph::SegmentsImageType::SizeType size;
        for (unsigned int dimension = 0; dimension < Graph::Dimension; ++dimension) {
            size[dimension] = static_cast<Graph::SegmentsImageType::SizeType::SizeValueType>(
                maximum[dimension] - minimum[dimension] + 1);
        }
        return {minimum, size};
    }
};

struct NeighborMergePlan {
    std::set<Graph::SegmentIdType> selectedLabels;
    std::unordered_map<Graph::SegmentIdType, NeighborMergeLabelStats> statsByLabel;
    std::vector<std::pair<Graph::SegmentIdType, Graph::SegmentIdType>> labelPairs;
    std::set<Graph::SegmentIdType> consumedLabels;
    std::size_t skippedNoNeighborCount = 0;
    Graph::SegmentIdType actualMaximumLabel = 0;
    QString error;

    bool valid() const {
        return error.isEmpty();
    }
};

NeighborMergePlan buildNeighborMergePlan(
    const dataType::SegmentsStorage::Pointer &selectedSegmentation,
    Graph::SegmentIdType backgroundId,
    const std::unordered_set<Graph::SegmentIdType> &ignoredLabels,
    const std::vector<Graph::SegmentIdType> &requestedLabels,
    Graph::SegmentationNeighborSelection neighborSelection) {
    NeighborMergePlan plan;
    if (Graph::segmentationNeighborSelectionName(neighborSelection) == nullptr) {
        plan.error = QStringLiteral("Unsupported neighbor-selection mode %1.")
                         .arg(static_cast<int>(neighborSelection));
        return plan;
    }
    plan.selectedLabels.insert(requestedLabels.begin(), requestedLabels.end());
    plan.actualMaximumLabel = 0;
    for (const Graph::SegmentIdType label : plan.selectedLabels) {
        if (ignoredLabels.count(label) > 0) {
            plan.error = QStringLiteral("Ignored label %1 cannot be merged with a neighbor.")
                             .arg(label);
            return plan;
        }
    }

    const auto region = selectedSegmentation->GetLargestPossibleRegion();
    const auto size = region.GetSize();
    const auto start = region.GetIndex();
    const std::size_t dimX = size[0];
    const std::size_t dimY = size[1];
    const std::size_t dimZ = size[2];
    if (dimX == 0 || dimY == 0 || dimZ == 0 ||
        dimX > std::numeric_limits<std::size_t>::max() / dimY) {
        plan.error = QStringLiteral("The segmentation image has invalid dimensions.");
        return plan;
    }
    const std::size_t sliceStride = dimX * dimY;
    if (sliceStride > std::numeric_limits<std::size_t>::max() / dimZ) {
        plan.error = QStringLiteral("The segmentation image is too large to index safely.");
        return plan;
    }
    std::vector<Graph::SegmentIdType> currentSlice, nextSlice;
    selectedSegmentation->readSlice(2, start[2], currentSlice);

    std::unordered_map<Graph::SegmentIdType,
                       std::unordered_map<Graph::SegmentIdType, std::size_t>>
        contactFaceCountByLabel;
    contactFaceCountByLabel.reserve(plan.selectedLabels.size());
    for (const Graph::SegmentIdType label : plan.selectedLabels) {
        contactFaceCountByLabel.emplace(
            label,
            std::unordered_map<Graph::SegmentIdType, std::size_t>{});
    }
    const auto incrementContactFaceCount = [&](Graph::SegmentIdType selectedLabel,
                                               Graph::SegmentIdType neighborLabel) {
        auto &contactFaceCount = contactFaceCountByLabel[selectedLabel][neighborLabel];
        if (contactFaceCount == std::numeric_limits<std::size_t>::max()) {
            plan.error = QStringLiteral("The shared face count between labels %1 and %2 overflowed.")
                             .arg(selectedLabel)
                             .arg(neighborLabel);
            return;
        }
        ++contactFaceCount;
    };
    const auto recordAdjacency = [&](Graph::SegmentIdType firstLabel, Graph::SegmentIdType secondLabel) {
        if (firstLabel == secondLabel || ignoredLabels.count(firstLabel) > 0
            || ignoredLabels.count(secondLabel) > 0) {
            return;
        }
        if (plan.selectedLabels.count(firstLabel) > 0) {
            incrementContactFaceCount(firstLabel, secondLabel);
        }
        if (plan.selectedLabels.count(secondLabel) > 0) {
            incrementContactFaceCount(secondLabel, firstLabel);
        }
    };

    for (std::size_t z = 0; z < dimZ; ++z) {
        if (z + 1 < dimZ) selectedSegmentation->readSlice(2, start[2] + z + 1, nextSlice);
        const auto *selectedBuffer = currentSlice.data();
        for (std::size_t y = 0; y < dimY; ++y) {
            for (std::size_t x = 0; x < dimX; ++x) {
                const std::size_t index = x + y * dimX;
                const Graph::SegmentIdType label = selectedBuffer[index];
                auto &stats = plan.statsByLabel[label];
                ++stats.voxelCount;
                const Graph::SegmentsImageType::IndexType voxelIndex{{
                    start[0] + static_cast<Graph::SegmentsImageType::IndexType::IndexValueType>(x),
                    start[1] + static_cast<Graph::SegmentsImageType::IndexType::IndexValueType>(y),
                    start[2] + static_cast<Graph::SegmentsImageType::IndexType::IndexValueType>(z)}};
                if (!stats.hasSeed) {
                    stats.seed = voxelIndex;
                    stats.minimum = voxelIndex;
                    stats.maximum = voxelIndex;
                    stats.hasSeed = true;
                } else {
                    for (unsigned int dimension = 0; dimension < Graph::Dimension; ++dimension) {
                        stats.minimum[dimension] = std::min(stats.minimum[dimension], voxelIndex[dimension]);
                        stats.maximum[dimension] = std::max(stats.maximum[dimension], voxelIndex[dimension]);
                    }
                }
                plan.actualMaximumLabel = std::max(plan.actualMaximumLabel, label);

                if (x + 1 < dimX) recordAdjacency(label, selectedBuffer[index + 1]);
                if (y + 1 < dimY) recordAdjacency(label, selectedBuffer[index + dimX]);
                if (z + 1 < dimZ) recordAdjacency(label, nextSlice[index]);
            }
        }
        currentSlice.swap(nextSlice);
    }

    if (!plan.valid()) {
        return plan;
    }

    plan.labelPairs.reserve(plan.selectedLabels.size());
    for (const Graph::SegmentIdType selectedLabel : plan.selectedLabels) {
        const auto selectedStats = plan.statsByLabel.find(selectedLabel);
        if (selectedStats == plan.statsByLabel.end() || selectedStats->second.voxelCount == 0) {
            plan.error = QStringLiteral("Selected label %1 is no longer present in the segmentation.")
                             .arg(selectedLabel);
            return plan;
        }

        const auto &neighbors = contactFaceCountByLabel.at(selectedLabel);
        bool foundNeighbor = false;
        Graph::SegmentIdType selectedNeighbor = backgroundId;
        std::size_t selectedNeighborVoxelCount = 0;
        std::size_t selectedNeighborContactFaceCount = 0;
        for (const auto &[neighborLabel, contactFaceCount] : neighbors) {
            const auto neighborStats = plan.statsByLabel.find(neighborLabel);
            if (neighborStats == plan.statsByLabel.end() || neighborStats->second.voxelCount == 0) {
                continue;
            }
            bool preferred = false;
            bool metricTied = false;
            switch (neighborSelection) {
                case Graph::SegmentationNeighborSelection::Smallest:
                    preferred = neighborStats->second.voxelCount < selectedNeighborVoxelCount;
                    metricTied = neighborStats->second.voxelCount == selectedNeighborVoxelCount;
                    break;
                case Graph::SegmentationNeighborSelection::Largest:
                    preferred = neighborStats->second.voxelCount > selectedNeighborVoxelCount;
                    metricTied = neighborStats->second.voxelCount == selectedNeighborVoxelCount;
                    break;
                case Graph::SegmentationNeighborSelection::MostConnected:
                    preferred = contactFaceCount > selectedNeighborContactFaceCount;
                    metricTied = contactFaceCount == selectedNeighborContactFaceCount;
                    break;
            }
            if (!foundNeighbor || preferred || (metricTied && neighborLabel < selectedNeighbor)) {
                foundNeighbor = true;
                selectedNeighbor = neighborLabel;
                selectedNeighborVoxelCount = neighborStats->second.voxelCount;
                selectedNeighborContactFaceCount = contactFaceCount;
            }
        }
        if (!foundNeighbor) {
            ++plan.skippedNoNeighborCount;
            continue;
        }
        plan.labelPairs.push_back({selectedLabel, selectedNeighbor});
        plan.consumedLabels.insert(selectedLabel);
        plan.consumedLabels.insert(selectedNeighbor);
    }
    return plan;
}

QString neighborMergePlanSummary(const NeighborMergePlan &plan) {
    QStringList pairs;
    for (const auto &[sourceLabel, targetLabel] : plan.labelPairs) {
        pairs << QStringLiteral("%1(%2)->%3(%4)")
                     .arg(sourceLabel)
                     .arg(plan.statsByLabel.at(sourceLabel).voxelCount)
                     .arg(targetLabel)
                     .arg(plan.statsByLabel.at(targetLabel).voxelCount);
    }
    return pairs.join(QStringLiteral(", "));
}

QString connectedComponentMappingSummary(
    const std::unordered_map<Graph::SegmentIdType, std::vector<Graph::SegmentIdType>> &mapping) {
    QStringList entries;
    for (const auto &[originalLabel, finalLabels] : mapping) {
        entries << QStringLiteral("%1=[%2]").arg(originalLabel).arg(joinIds(finalLabels));
    }
    entries.sort();
    return entries.join(QStringLiteral(", "));
}

const char *neighborMergeStatusName(Graph::SegmentationNeighborMergeResult::Status status) {
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    switch (status) {
        case Status::Merged: return "merged";
        case Status::NeedsInsertionConfirmation: return "needs_insertion_confirmation";
        case Status::NeedsConnectedComponentConfirmation: return "needs_connected_component_confirmation";
        case Status::NothingToMerge: return "nothing_to_merge";
        case Status::Failed: return "failed";
    }
    return "unknown";
}

} // namespace

const char *Graph::segmentationNeighborSelectionName(
        SegmentationNeighborSelection selection) noexcept {
    switch (selection) {
        case SegmentationNeighborSelection::Smallest: return "smallest";
        case SegmentationNeighborSelection::Largest: return "largest";
        case SegmentationNeighborSelection::MostConnected: return "most_connected";
    }
    return nullptr;
}

Graph::~Graph() = default;

Graph::Graph(std::shared_ptr<GraphBase> graphBaseIn, bool verboseIn) {
    verbose = verboseIn;
    nextFreeId = 0;
    pIgnoredSegmentLabels = nullptr;
    backgroundIdStrategy = "backgroundIsLowestId";
    backgroundId = 0;

    graphBase = graphBaseIn;

    initialNodes = std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>>();
    initialLabelPairToTwoSidedInitialEdge = std::map<EdgePairIdType, std::shared_ptr<TwoSidedInitialEdge>>();
    initialEdgeIdLookup = std::unordered_map<EdgeNumIdType, EdgePairIdType>();
    workingNodes = std::unordered_map<SegmentIdType, std::shared_ptr<WorkingNode>>();
    workingLabelPairToWorkingEdge = std::map<EdgePairIdType, std::shared_ptr<WorkingEdge>>();

    segmentManager = SegmentManager(graphBase, &initialNodes, &initialLabelPairToTwoSidedInitialEdge,
                                    &initialEdgeIdLookup, &graphBase->edgeStatus,
                                    &graphBase->pEdgesInitialSegmentsImage, &graphBase->pWorkingSegmentsImage,
                                    &workingNodes, &workingLabelPairToWorkingEdge,
                                    pIgnoredSegmentLabels, &nextFreeId);
}

namespace {

struct GraphStorageSummary {
    MemoryStats memory;
    std::size_t initialNodeVoxelCount = 0;
    std::size_t initialNodeVoxelBytes = 0;
    std::size_t oneSidedInitialEdgeCount = 0;
    std::size_t oneSidedInitialEdgeVoxelCount = 0;
    std::size_t oneSidedInitialEdgeVoxelBytes = 0;
    std::size_t twoSidedInitialEdgeReferencedVoxelCount = 0;
    std::size_t twoSidedInitialEdgeOwnedVoxelCapacity = 0;
    std::size_t twoSidedInitialEdgeOneSidedEdgeReferenceCount = 0;
    std::size_t workingNodeInitialNodeReferenceCount = 0;
    std::size_t workingNodeEdgeReferenceCount = 0;
    std::size_t workingEdgeTwoSidedInitialEdgeReferenceCount = 0;
    std::size_t workingEdgeTwoSidedInitialEdgeReferenceCapacity = 0;
    double trackedVectorCapacityMB = 0.0;
};

GraphStorageSummary collectGraphStorageSummary(const Graph &graph) {
    GraphStorageSummary summary;
    summary.memory = systemStats::queryMemory();

    for (const auto &[label, initialNode] : graph.initialNodes) {
        if (initialNode == nullptr) {
            continue;
        }
        summary.initialNodeVoxelCount += initialNode->voxels.size();
        summary.initialNodeVoxelBytes += initialNode->voxels.storedBytes();
        for (const auto &[neighborLabel, oneSidedInitialEdge] : initialNode->neighborLabelToOneSidedInitialEdge) {
            if (oneSidedInitialEdge == nullptr) {
                continue;
            }
            ++summary.oneSidedInitialEdgeCount;
            summary.oneSidedInitialEdgeVoxelCount += oneSidedInitialEdge->voxels.size();
            summary.oneSidedInitialEdgeVoxelBytes += oneSidedInitialEdge->voxels.storedBytes();
        }
    }

    for (const auto &[labelPair, twoSidedInitialEdge] : graph.initialLabelPairToTwoSidedInitialEdge) {
        if (twoSidedInitialEdge == nullptr) {
            continue;
        }
        summary.twoSidedInitialEdgeReferencedVoxelCount += twoSidedInitialEdge->getVoxelCount();
        summary.twoSidedInitialEdgeOneSidedEdgeReferenceCount += 2;
    }

    for (const auto &[label, workingNode] : graph.workingNodes) {
        if (workingNode == nullptr) {
            continue;
        }
        summary.workingNodeInitialNodeReferenceCount += workingNode->subInitialNodes.size();
        summary.workingNodeEdgeReferenceCount += workingNode->neighborLabelToWorkingEdge.size();
    }
    for (const auto &[labelPair, workingEdge] : graph.workingLabelPairToWorkingEdge) {
        if (workingEdge == nullptr) {
            continue;
        }
        summary.workingEdgeTwoSidedInitialEdgeReferenceCount += workingEdge->constituentTwoSidedInitialEdges.size();
        summary.workingEdgeTwoSidedInitialEdgeReferenceCapacity += workingEdge->constituentTwoSidedInitialEdges.capacity();
    }

    const long double trackedVectorCapacityBytes =
        (static_cast<long double>(summary.initialNodeVoxelBytes) +
         static_cast<long double>(summary.oneSidedInitialEdgeVoxelBytes)) +
        static_cast<long double>(summary.twoSidedInitialEdgeOwnedVoxelCapacity) * sizeof(Voxel) +
        static_cast<long double>(summary.workingEdgeTwoSidedInitialEdgeReferenceCapacity) *
            sizeof(std::shared_ptr<TwoSidedInitialEdge>);
    summary.trackedVectorCapacityMB =
        static_cast<double>(trackedVectorCapacityBytes / (1024.0L * 1024.0L));
    return summary;
}

void logStorageAfterGraphPhase(const Graph &graph, const QString &graphPhase) {
    const GraphStorageSummary summary = collectGraphStorageSummary(graph);
    logGraph(
        LogLevel::Debug,
        __func__,
        QStringLiteral(
            "Graph storage phase=%1 process_rss_gb=%2 peak_process_rss_gb=%3 available_memory_gb=%4 "
            "initial_nodes=%5 initial_node_voxels=%6 initial_node_voxel_bytes=%7 "
            "one_sided_initial_edges=%8 one_sided_edge_voxels=%9 one_sided_edge_voxel_bytes=%10 "
            "two_sided_initial_edges=%11 two_sided_edge_referenced_voxels=%12 "
            "two_sided_edge_owned_voxel_capacity=%13 two_sided_edge_one_sided_edge_references=%14 "
            "working_nodes=%15 working_edges=%16 working_node_initial_node_references=%17 "
            "working_node_edge_references=%18 working_edge_two_sided_initial_edge_references=%19 "
            "working_edge_two_sided_initial_edge_reference_capacity=%20 tracked_vector_capacity_mb=%21")
            .arg(graphPhase)
            .arg(summary.memory.processResidentMemoryGB, 0, 'f', 3)
            .arg(summary.memory.peakProcessResidentMemoryGB, 0, 'f', 3)
            .arg(summary.memory.availableSystemMemoryGB, 0, 'f', 3)
            .arg(static_cast<qulonglong>(graph.initialNodes.size()))
            .arg(static_cast<qulonglong>(summary.initialNodeVoxelCount))
            .arg(static_cast<qulonglong>(summary.initialNodeVoxelBytes))
            .arg(static_cast<qulonglong>(summary.oneSidedInitialEdgeCount))
            .arg(static_cast<qulonglong>(summary.oneSidedInitialEdgeVoxelCount))
            .arg(static_cast<qulonglong>(summary.oneSidedInitialEdgeVoxelBytes))
            .arg(static_cast<qulonglong>(graph.initialLabelPairToTwoSidedInitialEdge.size()))
            .arg(static_cast<qulonglong>(summary.twoSidedInitialEdgeReferencedVoxelCount))
            .arg(static_cast<qulonglong>(summary.twoSidedInitialEdgeOwnedVoxelCapacity))
            .arg(static_cast<qulonglong>(summary.twoSidedInitialEdgeOneSidedEdgeReferenceCount))
            .arg(static_cast<qulonglong>(graph.workingNodes.size()))
            .arg(static_cast<qulonglong>(graph.workingLabelPairToWorkingEdge.size()))
            .arg(static_cast<qulonglong>(summary.workingNodeInitialNodeReferenceCount))
            .arg(static_cast<qulonglong>(summary.workingNodeEdgeReferenceCount))
            .arg(static_cast<qulonglong>(summary.workingEdgeTwoSidedInitialEdgeReferenceCount))
            .arg(static_cast<qulonglong>(summary.workingEdgeTwoSidedInitialEdgeReferenceCapacity))
            .arg(summary.trackedVectorCapacityMB, 0, 'f', 3));
}

struct LocalVoxelGrid {
    int minX = 0;
    int minY = 0;
    int minZ = 0;
    int sizeX = 0;
    int sizeY = 0;
    int sizeZ = 0;
    std::vector<int> voxelIndices;

    bool contains(int x, int y, int z) const {
        return x >= minX && y >= minY && z >= minZ &&
               x < minX + sizeX &&
               y < minY + sizeY &&
               z < minZ + sizeZ;
    }

    std::size_t linearIndex(int x, int y, int z) const {
        return static_cast<std::size_t>(z - minZ) * static_cast<std::size_t>(sizeY) * static_cast<std::size_t>(sizeX) +
               static_cast<std::size_t>(y - minY) * static_cast<std::size_t>(sizeX) +
               static_cast<std::size_t>(x - minX);
    }

    int lookup(int x, int y, int z) const {
        if (!contains(x, y, z)) {
            return -1;
        }
        return voxelIndices[linearIndex(x, y, z)];
    }

    template<typename Fn>
    void forEachPresentNeighborIndex(const Voxel &voxel, Fn &&fn) const {
        const int localX = voxel.x - minX;
        const int localY = voxel.y - minY;
        const int localZ = voxel.z - minZ;
        const std::size_t strideY = static_cast<std::size_t>(sizeX);
        const std::size_t strideZ = strideY * static_cast<std::size_t>(sizeY);
        const std::size_t localIndex =
            static_cast<std::size_t>(localZ) * strideZ +
            static_cast<std::size_t>(localY) * strideY +
            static_cast<std::size_t>(localX);

        if (localX + 1 < sizeX) {
            const int neighborIndex = voxelIndices[localIndex + 1];
            if (neighborIndex >= 0) {
                fn(neighborIndex);
            }
        }
        if (localX > 0) {
            const int neighborIndex = voxelIndices[localIndex - 1];
            if (neighborIndex >= 0) {
                fn(neighborIndex);
            }
        }
        if (localY + 1 < sizeY) {
            const int neighborIndex = voxelIndices[localIndex + strideY];
            if (neighborIndex >= 0) {
                fn(neighborIndex);
            }
        }
        if (localY > 0) {
            const int neighborIndex = voxelIndices[localIndex - strideY];
            if (neighborIndex >= 0) {
                fn(neighborIndex);
            }
        }
        if (localZ + 1 < sizeZ) {
            const int neighborIndex = voxelIndices[localIndex + strideZ];
            if (neighborIndex >= 0) {
                fn(neighborIndex);
            }
        }
        if (localZ > 0) {
            const int neighborIndex = voxelIndices[localIndex - strideZ];
            if (neighborIndex >= 0) {
                fn(neighborIndex);
            }
        }
    }
};

struct ReplacementInitialComponent {
    int finalComponentId = -1;
    std::vector<int> voxelIndices;
};

struct PreparedInitialEdge {
    std::shared_ptr<OneSidedInitialEdge> smallerSide;
    std::shared_ptr<OneSidedInitialEdge> largerSide;
    std::unique_ptr<TwoSidedInitialEdge> edge;

    void install(Graph &graph, SegmentManager &manager) {
        const auto [a, b] = edge->pairId;
        const auto &first = graph.initialNodes.at(a);
        const auto &second = graph.initialNodes.at(b);
        first->neighborLabelToOneSidedInitialEdge.emplace(b, std::move(smallerSide));
        second->neighborLabelToOneSidedInitialEdge.emplace(a, std::move(largerSide));
        if (first->getCurrentWorkingNodeLabel() == second->getCurrentWorkingNodeLabel())
            edge->setShouldMergeYes();
        manager.registerTwoSidedInitialEdge(std::move(edge));
    }
};

using BoundaryRuns = std::map<Graph::EdgePairIdType, std::array<std::vector<VoxelList::Run>, 2>>;

std::vector<PreparedInitialEdge> prepareInitialEdges(BoundaryRuns &contacts) {
    std::vector<PreparedInitialEdge> edges;
    for (auto &[pair, sides] : contacts) {
        if (sides[0].empty() && sides[1].empty()) continue;
        auto a = segment_puzzler::makeContactEdge(pair.first, pair.second, sides[0]);
        auto b = segment_puzzler::makeContactEdge(pair.second, pair.first, sides[1]);
        auto edge = std::make_unique<TwoSidedInitialEdge>(a, b);
        edge->calculateEdgeFeatures();
        edges.push_back({std::move(a), std::move(b), std::move(edge)});
    }
    return edges;
}

// Contacts outside editRegion are supplied by the caller when a previous
// boundary extends beyond the edited area.
template<typename ChangedLabel>
void collectInitialContacts(const Graph::SegmentsImageType::Pointer &labels,
                            const Graph::SegmentsImageType::RegionType &editRegion,
                            Graph::SegmentIdType background, ChangedLabel changed, BoundaryRuns &contacts) {
    using Image = Graph::SegmentsImageType;
    const auto region = labels->GetLargestPossibleRegion();
    const auto local = segment_puzzler::DenseImageStorage<Graph::SegmentIdType>::fromImage(labels);
    segment_puzzler::visitLabelContacts(*local, region, region.GetIndex()[2],
        [&](auto a, auto b, Image::IndexType left, Image::IndexType right, std::size_t length) {
            if (a == b || a == background || b == background || (!changed(a) && !changed(b))) return;
            if (a > b) { std::swap(a, b); std::swap(left, right); }
            auto &sides = contacts[{a, b}];
            for (unsigned side = 0; side < 2; ++side) {
                Image::RegionType row(side == 0 ? left : right, {length, 1, 1});
                if (!row.Crop(editRegion)) continue;
                const auto start = row.GetIndex();
                sides[side].push_back({Voxel(start[0], start[1], start[2]), static_cast<std::uint32_t>(row.GetSize()[0])});
            }
        });
}

// Only the opposite contact voxels are needed to identify the initial nodes
// along the outside of the target, including constituents of merged neighbors.
bool prepareSplitEdges(const Graph &graph, const WorkingNode &target,
                       const std::unordered_map<Graph::SegmentIdType, std::shared_ptr<InitialNode>> &replacements,
                       const Graph::SegmentsImageType::RegionType &region, std::vector<PreparedInitialEdge> &edges) {
    using Image = Graph::SegmentsImageType;
    auto labels = Image::New();
    labels->SetRegions(region);
    labels->Allocate();
    labels->FillBuffer(graph.backgroundId);
    for (const auto &[id, node] : target.subInitialNodes) {
        for (const auto &[neighborId, side] : node->neighborLabelToOneSidedInitialEdge) {
            if (target.subInitialNodes.count(neighborId)) continue;
            const auto neighbor = graph.initialNodes.find(neighborId);
            if (neighbor == graph.initialNodes.end() || !neighbor->second) return false;
            const auto &opposite = neighbor->second->neighborLabelToOneSidedInitialEdge;
            const auto boundary = opposite.find(id);
            if (boundary == opposite.end() || !boundary->second) return false;
            const auto owner = graph.workingNodes.find(neighbor->second->getCurrentWorkingNodeLabel());
            if (owner == graph.workingNodes.end() || !owner->second) return false;
            for (const auto &voxel : boundary->second->voxels) {
                const Image::IndexType index{voxel.x, voxel.y, voxel.z};
                if (!region.IsInside(index)) return false;
                labels->SetPixel(index, neighborId);
            }
        }
    }
    for (const auto &[id, node] : replacements)
        for (const auto &voxel : node->voxels) labels->SetPixel({voxel.x, voxel.y, voxel.z}, id);

    BoundaryRuns contacts;
    collectInitialContacts(labels, region, graph.backgroundId, [&](auto id) { return replacements.count(id); }, contacts);
    edges = prepareInitialEdges(contacts);
    return true;
}

} // namespace


void Graph::constructFromVolume(itk::Image<SegmentIdType, 3>::Pointer pImage, std::optional<SegmentIdType> sourceBackground) {
    constructFromVolume(segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(pImage), sourceBackground);
}

void Graph::constructFromVolume(dataType::SegmentsStorage::Pointer pImage, std::optional<SegmentIdType> sourceBackground) {
    const bool logGraphStorage = isGraphDebugLoggingEnabled(verbose);
    initializeEdgeVolumeAndEdgeStatus();
    updateBackgroundIdFromVolume(pImage, sourceBackground);
    pIgnoredSegmentLabels->push_back(backgroundId);

    nextFreeId = getNextFreeId(pImage);
    constructInitialNodes(pImage);
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("initial_nodes"));
    }
    segmentManager.computeOneSidedEdgesOnAllInitialNodes(*pImage);
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("one_sided_initial_edges"));
    }
    segmentManager.buildTwoSidedInitialEdgesFromOneSidedInitialEdges();
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("two_sided_initial_edges"));
    }
    segmentManager.buildWorkingGraphFromInitialGraph();
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("working_graph"));
    }
}

void Graph::constructInitialNodes(dataType::SegmentsStorage::Pointer pImage) {
    ScopedGraphTimer totalTimer(verbose, __func__, QStringLiteral("Constructing initial nodes"));
    std::unordered_map<SegmentIdType, std::size_t> voxelCountByLabel, runCountByLabel;
    {   ScopedGraphTimer histogramTimer(verbose, __func__, QStringLiteral("Building segment histogram"));
        const int threadCount = segment_puzzler::workerThreadCount();
        struct LabelCounts { std::size_t voxels = 0, runs = 0; };
        std::vector<std::unordered_map<SegmentIdType, LabelCounts>> localHistograms(threadCount);
        const auto full = pImage->GetLargestPossibleRegion();
        const auto depth = full.GetSize()[2];
        auto parallel = itk::MultiThreaderBase::New();
        parallel->SetMaximumNumberOfThreads(threadCount);
        parallel->SetNumberOfWorkUnits(threadCount);
        parallel->ParallelizeArray(0, threadCount, [&](itk::SizeValueType worker) {
            auto &histogram = localHistograms[worker];
            const auto begin = depth * worker / threadCount;
            const auto end = depth * (worker + 1) / threadCount;
            if (begin == end) return;
            auto region = full;
            region.SetIndex(2, full.GetIndex()[2] + begin);
            region.SetSize(2, end - begin);
            pImage->forEachRun(region, [&](const auto &, std::size_t length, SegmentIdType label) {
                auto &counts = histogram[label];
                counts.voxels += length;
                ++counts.runs;
            });
        }, nullptr);
        for (const auto &histogram : localHistograms) {
            for (const auto &[label, counts] : histogram) {
                voxelCountByLabel[label] += counts.voxels;
                runCountByLabel[label] += counts.runs;
            }
        }
    }

    std::size_t initialNodeCount = 0;
    std::size_t ignoredLabelCount = 0;
    std::size_t ignoredVoxelCount = 0;
    std::size_t backgroundVoxelCount = 0;
    std::size_t largestForegroundLabelVoxelCount = 0;
    std::vector<SegmentIdType> presentLabels;
    presentLabels.reserve(voxelCountByLabel.size());
    for (const auto &[label, count] : voxelCountByLabel) presentLabels.push_back(label);
    std::sort(presentLabels.begin(), presentLabels.end());
    for (const auto labelIndex : presentLabels) {
        const std::size_t voxelCount = voxelCountByLabel.at(labelIndex);
        if (voxelCount == 0) {
            continue;
        }

        if (labelIndex == static_cast<std::size_t>(backgroundId)) {
            backgroundVoxelCount = voxelCount;
        } else {
            largestForegroundLabelVoxelCount = std::max(largestForegroundLabelVoxelCount, voxelCount);
        }
        if (isIgnoredId(static_cast<SegmentIdType>(labelIndex))) {
            ++ignoredLabelCount;
            ignoredVoxelCount += voxelCount;
        } else {
            ++initialNodeCount;
        }
    }

    {
        std::unordered_map<SegmentIdType, InitialNode *> initialNodeByLabel;
        {
            ScopedGraphTimer initialNodeTimer(verbose, __func__, QStringLiteral("Allocating initial nodes"));
            QElapsedTimer phaseTimer;
            phaseTimer.start();
            initialNodeByLabel.reserve(presentLabels.size());
            const double resizeDenseNodeIndexMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

            phaseTimer.restart();
            segmentManager.clearGraphAndReserveInitialNodes(initialNodeCount);
            const double resetPreviousGraphMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

            phaseTimer.restart();
            for (const auto labelIndex : presentLabels) {
                const std::size_t voxelCount = voxelCountByLabel.at(labelIndex);
                const auto label = static_cast<SegmentIdType>(labelIndex);
                if (voxelCount == 0 || isIgnoredId(label)) {
                    continue;
                }
                segmentManager.addInitialNode(label, runCountByLabel.at(label));
                initialNodeByLabel[labelIndex] = initialNodes.at(label).get();
            }
            const double createInitialNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

            logGraphDebugIf(
                verbose,
                __func__,
                QStringLiteral(
                    "Initial node allocation phases label_slots=%1 initial_nodes=%2 ignored_labels=%3 "
                    "ignored_voxel_count=%4 background_voxel_count=%5 largest_foreground_label_voxel_count=%6 "
                    "resize_dense_index_ms=%7 reset_previous_graph_ms=%8 create_initial_nodes_ms=%9")
                    .arg(static_cast<qulonglong>(nextFreeId))
                    .arg(static_cast<qulonglong>(initialNodeCount))
                    .arg(static_cast<qulonglong>(ignoredLabelCount))
                    .arg(static_cast<qulonglong>(ignoredVoxelCount))
                    .arg(static_cast<qulonglong>(backgroundVoxelCount))
                    .arg(static_cast<qulonglong>(largestForegroundLabelVoxelCount))
                    .arg(resizeDenseNodeIndexMs, 0, 'f', 3)
                    .arg(resetPreviousGraphMs, 0, 'f', 3)
                    .arg(createInitialNodesMs, 0, 'f', 3));
        }

        {
            ScopedGraphTimer voxelAssignmentTimer(verbose, __func__, QStringLiteral("Assigning voxels to initial nodes"));
            pImage->forEachRun([&](const auto &index, std::size_t length, SegmentIdType label) {
                const auto node = initialNodeByLabel.find(label);
                if (node != initialNodeByLabel.end())
                    node->second->voxels.appendRun(Voxel(index[0], index[1], index[2]), length);
            });
        }

        {
            ScopedGraphTimer finalizeNodesTimer(verbose, __func__, QStringLiteral("Finalizing initial node ROIs"));
            for (const auto &[label, initialNode] : initialNodeByLabel) {
                if (initialNode != nullptr) {
                    initialNode->roi.updateBoundingRoi(initialNode->voxels);
                }
            }
        }
    }

    {   ScopedGraphTimer featureTimer(verbose, __func__, QStringLiteral("Calculating initial node features"));
        std::vector<SegmentIdType> idsOfInitialNodes = utils::getKeyVecOfSharedPtrMap<SegmentIdType>(initialNodes);
//#pragma omp parallel for schedule(dynamic)
        for (long long i = 0; i < static_cast<long long>(idsOfInitialNodes.size()); i++) {
            initialNodes[idsOfInitialNodes[i]]->calculateNodeFeatures();
        }
    }
}


void Graph::initializeEdgeVolumeAndEdgeStatus() {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Initializing edge volume and edge status"));
    graphBase->edgeCounter = 0;
    graphBase->edgeStatus.clear();
    graphBase->colorLookUpEdgesStatus.clear();
    initialEdgeIdLookup.clear();
    graphBase->pEdgesInitialSegmentsImage = segment_puzzler::StreamVByteImageStorage<dataType::MappedEdgeIdType>::create(
        *graphBase->pWorkingSegmentsImage->imageInformation());

    graphBase->colorLookUpEdgesStatus.emplace(0, qRgb(255, 255, 255));
    graphBase->colorLookUpEdgesStatus.emplace(1, qRgb(0, 0, 255));
    graphBase->colorLookUpEdgesStatus.emplace(-1, qRgb(255, 255, 0));
    graphBase->colorLookUpEdgesStatus.emplace(-2, qRgb(255, 0, 0));
    graphBase->colorLookUpEdgesStatus.emplace(2, qRgb(0, 255, 0));


    if (ownedEdgesSignal == nullptr) {
        ownedEdgesSignal = std::make_unique<itkSignal<dataType::MappedEdgeIdType>>(
                graphBase->pEdgesInitialSegmentsImage);
    } else {
        ownedEdgesSignal->updateStorage(graphBase->pEdgesInitialSegmentsImage);
    }
    graphBase->pEdgesInitialSegmentsITKSignal = ownedEdgesSignal.get();
}


Graph::SegmentIdType Graph::getNextFreeId(dataType::SegmentsStorage::Pointer pImage) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Finding next free segment id"));
    SegmentIdType largestId = 0;
    pImage->forEachRun([&](const auto &, std::size_t, SegmentIdType label) {
        if (label > largestId && label != backgroundId && (!pIgnoredSegmentLabels || !isIgnoredId(label))) {
            largestId = label;
        }
    });
    if (largestId == std::numeric_limits<SegmentIdType>::max()) return largestId;
    logGraphDebugIf(verbose, __func__, QStringLiteral("Next free segment id=%1").arg(largestId + 1));
    return largestId + 1;
}


void Graph::setPointerToIgnoredSegmentLabels(std::vector<SegmentIdType> *pIgnoredSegmentLabelsIn) {
    pIgnoredSegmentLabels = pIgnoredSegmentLabelsIn;
    segmentManager.setPointerToIgnoredSegmentsLabels(pIgnoredSegmentLabelsIn);
}

bool Graph::isIgnoredId(Graph::SegmentIdType idToCheck) {
    // check if idToCFheck is in pIgnoredSegmentsLabel
    return (std::find(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end(), idToCheck) !=
            pIgnoredSegmentLabels->end());
}


std::set<Graph::SegmentIdType>
Graph::mergeEdges(const std::set<EdgeNumIdType> &edgeIdsToMerge) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Merging selected edges"));
    logGraphDebugIf(verbose, __func__, QStringLiteral("Edge ids=%1").arg(joinIds(edgeIdsToMerge)));

    for (const auto id : edgeIdsToMerge) {
        const auto &pair = initialEdgeIdLookup.at(id);
        mergeEdge(initialLabelPairToTwoSidedInitialEdge.at(pair).get(), false);
    }

    std::set<SegmentIdType> newWorkingNodeIds;
    for (const auto id : edgeIdsToMerge) {
        const auto &pair = initialEdgeIdLookup.at(id);
        newWorkingNodeIds.insert(initialNodes.at(pair.first)->getCurrentWorkingNodeLabel());
    }
    for (const auto label : newWorkingNodeIds) insertWorkingNodeInSegmentImage(*workingNodes.at(label));
    return newWorkingNodeIds;
}

void Graph::mergeEdge(TwoSidedInitialEdge *edge, bool updateSegmentImage) {
    const auto idWorkingNodeA = initialNodes.at(edge->getLabelSmaller())->getCurrentWorkingNodeLabel();
    const auto idWorkingNodeB = initialNodes.at(edge->getLabelBigger())->getCurrentWorkingNodeLabel();
    if (idWorkingNodeA == idWorkingNodeB) return;

    const auto idOfNewNode = segmentManager.takeNextSegmentId(nextFreeId);
    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("Working nodes %1,%2 -> %3 via initial edge %4")
            .arg(idWorkingNodeA)
            .arg(idWorkingNodeB)
            .arg(idOfNewNode)
            .arg(edge->numId));

    // remove old nodes and edges
    WorkingNode nodeA = *workingNodes.at(idWorkingNodeA);
    WorkingNode nodeB = *workingNodes.at(idWorkingNodeB);

    std::vector<WorkingNode> vecOfWorkingNodes{nodeA, nodeB};
    WorkingNode *newNode = new WorkingNode(vecOfWorkingNodes, idOfNewNode, initialNodes);

    segmentManager.removeWorkingNode(workingNodes.at(idWorkingNodeA).get());
    segmentManager.removeWorkingNode(workingNodes.at(idWorkingNodeB).get());

    segmentManager.addWorkingNode(newNode);
    segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(idOfNewNode).get());


    if (updateSegmentImage) {
        insertWorkingNodeInSegmentImage(*workingNodes.at(idOfNewNode));
    }

    edge->setShouldMergeYes();
    auto numId = edge->numId;
    graphBase->edgeStatus[numId] = 2;
}

void Graph::insertWorkingNodeInSegmentImage(WorkingNode &pWorkingNode) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Writing working node into segment image"));

//    auto voxelList = pWorkingNode.getVoxelArray();
    segment_puzzler::setVoxelLists(*graphBase->pWorkingSegmentsImage,
                                  pWorkingNode.getVoxelLists(), pWorkingNode.getLabel());

}


namespace {
using InitialNodeGroup = SegmentManager::InitialNodeGroup;

InitialNodeGroup memberLabels(const WorkingNode &node) {
    InitialNodeGroup labels;
    for (const auto &[id, initial] : node.subInitialNodes) labels.insert(id);
    return labels;
}

std::pair<InitialNodeGroup, InitialNodeGroup> partitionAtEdge(
        const Graph &graph, const InitialNodeGroup &members, TwoSidedInitialEdge &edge) {
    const auto [first, second] = edge.pairId;
    const CenterOfMass contact(graph.initialNodes.at(first)->neighborLabelToOneSidedInitialEdge.at(second).get());
    const auto distancesFrom = [&](Graph::SegmentIdType seed) {
        std::map<Graph::SegmentIdType, float> distances;
        for (const auto id : members) distances.emplace(id, std::numeric_limits<float>::infinity());
        distances.at(seed) = 0;
        using Entry = std::pair<float, Graph::SegmentIdType>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
        queue.emplace(0, seed);
        while (!queue.empty()) {
            const auto [distance, id] = queue.top();
            queue.pop();
            if (distance != distances.at(id)) continue;
            const auto &node = graph.initialNodes.at(id);
            CenterOfMass source(node.get());
            if (id == seed) {
                const float length = source.distTo(contact);
                if (length > 0) {
                    source.x = contact.x + (source.x - contact.x) / length;
                    source.y = contact.y + (source.y - contact.y) / length;
                    source.z = contact.z + (source.z - contact.z) / length;
                }
            }
            for (const auto &[neighbor, boundary] : node->neighborLabelToTwoSidedInitialEdge) {
                if (boundary->numId == edge.numId || !members.count(neighbor)) continue;
                const float next = distance + source.distTo(CenterOfMass(graph.initialNodes.at(neighbor).get()));
                if (next < distances.at(neighbor)) {
                    distances.at(neighbor) = next;
                    queue.emplace(next, neighbor);
                }
            }
        }
        return distances;
    };
    const auto a = distancesFrom(first);
    const auto b = distancesFrom(second);
    std::pair<InitialNodeGroup, InitialNodeGroup> parts;
    for (const auto id : members) {
        if (id == first || (id != second && a.at(id) < b.at(id))) parts.first.insert(id);
        else parts.second.insert(id);
    }
    return parts;
}
}

void Graph::unmergeEdges(std::set<EdgeNumIdType> &vecOfEdgeIdsToUnMerge) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Unmerging selected edges"));
    logGraphDebugIf(verbose, __func__, QStringLiteral("Edge ids=%1").arg(joinIds(vecOfEdgeIdsToUnMerge)));

    SegmentManager::WorkingNodeSplits splits;
    for (const auto id : vecOfEdgeIdsToUnMerge) {
        const auto &pair = initialEdgeIdLookup.at(id);
        const auto owner = initialNodes.at(pair.first)->getCurrentWorkingNodeLabel();
        if (owner != initialNodes.at(pair.second)->getCurrentWorkingNodeLabel()) continue;
        auto [it, firstCut] = splits.try_emplace(owner);
        auto &groups = it->second;
        if (firstCut) groups.push_back(memberLabels(*workingNodes.at(owner)));
        const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &members) {
            return members.count(pair.first) && members.count(pair.second);
        });
        if (group == groups.end()) continue;  // An earlier cut already separated these endpoints.
        auto [a, b] = partitionAtEdge(*this, *group, *initialLabelPairToTwoSidedInitialEdge.at(pair));
        groups.erase(group);
        segmentManager.appendConnectedComponents(std::move(a), groups);
        segmentManager.appendConnectedComponents(std::move(b), groups);
    }
    segmentManager.replaceWorkingGroups(splits);
    for (const auto id : vecOfEdgeIdsToUnMerge) {
        initialLabelPairToTwoSidedInitialEdge.at(initialEdgeIdLookup.at(id))->setShouldMergeNo();
        graphBase->edgeStatus.at(id) = -2;
    }
}

std::pair<std::vector<Graph::SegmentIdType>, std::vector<Graph::SegmentIdType>>
Graph::calculateGraphDistancesFromEdge(WorkingNode *node, TwoSidedInitialEdge *edge) {
    const auto [a, b] = partitionAtEdge(*this, memberLabels(*node), *edge);
    return {{a.begin(), a.end()}, {b.begin(), b.end()}};
}

// generate a landscape/mask from allowed segment ids
Graph::LandscapeType::Pointer
Graph::generateLandscapePathfinding(SegmentsImageType::Pointer pSegments, SegmentIdType allowedWorkingNodeLabel,
                                    TwoSidedInitialEdge &forbiddenEdge) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Generating pathfinding landscape"));

    // get the roi of all allowed segments. the roi will define the maximal size of the landscape
    Roi mergedRoi = workingNodes[allowedWorkingNodeLabel]->roi;


    LandscapeType::Pointer pLandscape = LandscapeType::New();
    LandscapeType::IndexType startIndex = {{mergedRoi.minX, mergedRoi.minY, mergedRoi.minZ}};
    int dX = mergedRoi.maxX - mergedRoi.minX;
    int dY = mergedRoi.maxY - mergedRoi.minY;
    int dZ = mergedRoi.maxZ - mergedRoi.minZ;
    LandscapeType::SizeType sizeLandscape = {{static_cast<unsigned int> (dX + 1),
                                                     static_cast<unsigned int> (dY + 1),
                                                     static_cast<unsigned int> (dZ + 1)}};
    LandscapeType::RegionType regionLandscape(startIndex, sizeLandscape);

    pLandscape->SetRegions(regionLandscape);
    pLandscape->Allocate();
    pLandscape->FillBuffer(0);

    itk::ImageRegionConstIterator<SegmentsImageType> itSegments(pSegments, regionLandscape);
    itk::ImageRegionIterator<LandscapeType> itLandscape(pLandscape, regionLandscape);
    itSegments.GoToBegin();
    itLandscape.GoToBegin();
    while (!itSegments.IsAtEnd() && !itLandscape.IsAtEnd()) {
        SegmentIdType tmpLabel = itSegments.Get();
        if (tmpLabel == allowedWorkingNodeLabel) {
            itLandscape.Set(255);
        }
        ++itSegments;
        ++itLandscape;
    }


    for (const VoxelList *voxelList : forbiddenEdge.getVoxelLists()) {
        for (const Voxel &voxel : *voxelList) {
            pLandscape->SetPixel({voxel.x, voxel.y, voxel.z}, 0);
        }
    }

    return pLandscape;
}

// find shortest path given a landscape/mask
itk::Image<short, 3>::Pointer Graph::shortestPath(OneSidedInitialEdge &initialEdge,
                                                  LandscapeType::Pointer pLandscape) { //TODO: This is slow, make it fast! Fast Marching? Multithreaded?
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Computing shortest path distances"));





    // put all points of the one sided edge in the start set
    // expand till all voxels visited
    // read value as distance
    std::vector<itk::Index<3>> openVoxels;
    std::vector<itk::Index<3>> processingVoxels;
    short distValue = 0;

    DistanceType::Pointer pDistance = itk::Image<short, 3>::New();
    pDistance->SetRegions(pLandscape->GetLargestPossibleRegion());
    pDistance->Allocate();
    pDistance->FillBuffer(std::numeric_limits<short>::max());

    itk::Image<unsigned char, 3>::Pointer pVisitedBefore = itk::Image<unsigned char, 3>::New();
    pVisitedBefore->SetRegions(pLandscape->GetLargestPossibleRegion());
    pVisitedBefore->Allocate();
    pVisitedBefore->FillBuffer(0);

    for (const auto &voxel : initialEdge.voxels) {
        if (pLandscape->GetPixel({voxel.x, voxel.y, voxel.z})) { //1vx wide nodes
            openVoxels.push_back({voxel.x, voxel.y, voxel.z});
            pVisitedBefore->SetPixel({voxel.x, voxel.y, voxel.z}, 255);
        }
    }


    itk::NeighborhoodIterator<LandscapeType> neighborItLandscape({1, 1, 1}, pLandscape,
                                                                 pLandscape->GetLargestPossibleRegion());
    itk::NeighborhoodIterator<DistanceType> neighborItDistance({1, 1, 1}, pDistance,
                                                               pDistance->GetLargestPossibleRegion());
    itk::NeighborhoodIterator<itk::Image<unsigned char, 3>> neighborItVisitedBefore({1, 1, 1}, pVisitedBefore,
                                                                                    pVisitedBefore->GetLargestPossibleRegion());
    std::vector<unsigned int> offSetIndices = {4, 10, 12, 14, 16, 22};


    bool isInBound, wasVisitedBefore, isInsideMask;
    while (!openVoxels.empty()) {
        processingVoxels = openVoxels;
        for (auto &voxelIndex : processingVoxels) {
            pDistance->SetPixel(voxelIndex, distValue);
        }
//        ITKImageWriter<DistanceType>(pDistance, "distIter.nrrd");

        distValue++;
        openVoxels.clear(); // clear openVoxels queue
        // add neighbors, that were not visited before, are inside the image region, and are inside the bool mask to openVoxels voxels
        // to openvoxels!
        for (auto &voxelIndex : processingVoxels) {
//            std::cout << voxelIndex[0] << " " << voxelIndex[1] << " " << voxelIndex[2] << "\n";
            neighborItLandscape.SetLocation(voxelIndex);
            neighborItVisitedBefore.SetLocation(voxelIndex);
            for (unsigned int offsetIndex : offSetIndices) {
                isInsideMask = neighborItLandscape.GetPixel(offsetIndex, isInBound);
                wasVisitedBefore = neighborItVisitedBefore.GetPixel(offsetIndex);
                if (isInBound && isInsideMask && !wasVisitedBefore) {
                    auto index = neighborItLandscape.GetIndex(offsetIndex);
                    openVoxels.push_back(index);
                    pVisitedBefore->SetPixel(index, 255);

                }
            }

        }
    }
    return pDistance;
}


void Graph::unmergeEdge(WorkingNode *workingNodeToSplit, std::vector<SegmentIdType> initialNodeIdsA,
                        std::vector<SegmentIdType> initialNodeIdsB) {
    SegmentManager::WorkingNodeSplits splits;
    auto &groups = splits[workingNodeToSplit->getLabel()];
    segmentManager.appendConnectedComponents({initialNodeIdsA.begin(), initialNodeIdsA.end()}, groups);
    segmentManager.appendConnectedComponents({initialNodeIdsB.begin(), initialNodeIdsB.end()}, groups);
    segmentManager.replaceWorkingGroups(splits);
}

void Graph::unmergeEdge(TwoSidedInitialEdge *initialEdge, DistanceType::Pointer pDistanceSmaller,
                        DistanceType::Pointer pDistanceBigger) {
    //TODO: Handle cases where distance is inf/max, and assign them a own label (as they are not connected to the rest of the label at all)
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Unmerging edge from distance volumes"));

    SegmentIdType labelInitialNodeSmaller = initialEdge->getLabelSmaller();
    SegmentIdType labelInitialNodeBigger = initialEdge->getLabelBigger();
    SegmentIdType workingNodeSmaller = initialNodes[labelInitialNodeSmaller]->getCurrentWorkingNodeLabel();
    SegmentIdType workingNodeLarger = initialNodes[labelInitialNodeBigger]->getCurrentWorkingNodeLabel();
    //FIXME: has to be two nodes has to be forced!! uncomment the initialnode thingy
    //FIXME: Enforce Connectivity for segments after splitting? would be a good function to have anyway
    if (workingNodeLarger == workingNodeSmaller) {

        // this is an approx solution, best would be a geodesic distance to the edge
        SegmentIdType currentWorkingNodeLabel = workingNodeLarger;
        logGraphDebugIf(verbose,
                        __func__,
                        QStringLiteral("Unmerging working node %1 for initial edge %2/%3")
                            .arg(currentWorkingNodeLabel)
                            .arg(labelInitialNodeSmaller)
                            .arg(labelInitialNodeBigger));
        WorkingNode currentWorkingNode = *workingNodes[currentWorkingNodeLabel];
        std::vector<SegmentIdType> labelsOfInitialNodesNearSmaller, labelsOfInitialNodesNearBigger;

        for (auto &initialNode : currentWorkingNode.subInitialNodes) {
            if ((initialNode.first != labelInitialNodeSmaller) && (initialNode.first != labelInitialNodeBigger)) {
                double distanceToEdgeSmaller = 0, distanceToEdgeBigger = 0;

                for (const auto &voxel : initialNode.second->voxels) {
                    distanceToEdgeSmaller += pDistanceSmaller->GetPixel({voxel.x, voxel.y, voxel.z});
                    distanceToEdgeBigger += pDistanceBigger->GetPixel({voxel.x, voxel.y, voxel.z});
                }
                distanceToEdgeSmaller /= initialNode.second->voxels.size();
                distanceToEdgeBigger /= initialNode.second->voxels.size();
//                std::cout << "distSmaller: " << distanceToEdgeSmaller << "\n";
//                std::cout << "distLarger: " << distanceToEdgeBigger << "\n";
                // assign bucket which has the shorter distance
//                std::cout << distanceToEdgeSmaller << " " << distanceToEdgeBigger << "\n";
                if (distanceToEdgeSmaller > distanceToEdgeBigger) {
                    labelsOfInitialNodesNearBigger.push_back(initialNode.first);
                } else {
                    labelsOfInitialNodesNearSmaller.push_back(initialNode.first);
                }
            } else {
                if (initialNode.first ==
                    labelInitialNodeSmaller) { // this case has to be there to guarantee at least 1 segment in each bucket
                    // when changing to shortest path/geodesic distances, this should not be necessary
                    labelsOfInitialNodesNearSmaller.push_back(labelInitialNodeSmaller);
                } else {
                    labelsOfInitialNodesNearBigger.push_back(labelInitialNodeBigger);
                }
            }
        }

        logGraphDebugIf(verbose,
                        __func__,
                        QStringLiteral("Unmerge buckets smaller=[%1], bigger=[%2]")
                            .arg(joinIds(labelsOfInitialNodesNearSmaller))
                            .arg(joinIds(labelsOfInitialNodesNearBigger)));
        unmergeEdge(workingNodes.at(currentWorkingNodeLabel).get(), std::move(labelsOfInitialNodesNearSmaller),
                    std::move(labelsOfInitialNodesNearBigger));
    }
}

void Graph::splitIntoConnectedComponentsOfWorkingNode(WorkingNode &node) {
    segmentManager.splitIntoConnectedComponentsOfWorkingNode(node);
}

Graph::SegmentIdType Graph::getLargestIdInSegmentVolume(Graph::SegmentsImageType::Pointer pSegment) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Finding largest id in segment volume"));
    const SegmentIdType largestId = segment_puzzler::minMaxValues(
        pSegment->GetBufferPointer(), pSegment->GetLargestPossibleRegion().GetNumberOfPixels()).second;
    logGraphDebugIf(verbose, __func__, QStringLiteral("Largest id in segment volume=%1").arg(largestId));
    return largestId;
}

Graph::SegmentIdType Graph::selectedBackground() const {
    return graphBase->selectedSegmentationContext->background.value_or(0);
}

std::uint64_t Graph::nextSelectedLabel() const {
    const auto next = graphBase->selectedSegmentationContext->nextLabel;
    return next == selectedBackground() ? next + 1 : next;
}

Graph::SegmentIdType Graph::selectedLabelHighWaterMark() const {
    return static_cast<SegmentIdType>(std::min<std::uint64_t>(
        graphBase->selectedSegmentationContext->nextLabel - 1, std::numeric_limits<SegmentIdType>::max()));
}

Graph::SegmentIdType Graph::reserveSelectedLabelIds(std::size_t count) {
    return volume_io::reserveLabels(*graphBase->selectedSegmentationContext, count);
}

void Graph::splitWorkingNodeIntoInitialNodes(int x, int y, int z) {
    if (graphBase->pWorkingSegmentsImage == nullptr) return;
    SegmentIdType labelOfWorkingNode = graphBase->pWorkingSegmentsImage->GetPixel({x, y, z});
    if (!isIgnoredId(labelOfWorkingNode)) {
        splitWorkingNodeIntoInitialNodes(labelOfWorkingNode);
    }
}

void Graph::removeInitialNodeFromWorkingNodeAtPosition(int x, int y, int z) {
    segmentManager.removeInitialNodeFromWorkingNodeAtPosition(x, y, z);
}

void Graph::splitWorkingNodeIntoInitialNodes(SegmentIdType workingNodeIdToSplit) {
    segmentManager.splitWorkingNodeIntoInitialNodes(workingNodeIdToSplit);
}

segment_puzzler::connected_components::ConnectedComponentSplitStats Graph::splitDisconnectedInitialSegments(
        segment_puzzler::connected_components::ConnectivityStencil connectivity) {
    using segment_puzzler::connected_components::ConnectedComponentSplitOptions;
    using segment_puzzler::connected_components::ConnectedComponentSplitStats;
    using segment_puzzler::connected_components::connectivityStencilName;
    using segment_puzzler::connected_components::splitDisconnectedLabelComponentsInPlace;

    ScopedGraphTimer timer(
        verbose,
        __func__,
        QStringLiteral("Splitting disconnected initial segments (%1 connectivity)")
            .arg(QString::fromLatin1(connectivityStencilName(connectivity))));

    ConnectedComponentSplitStats stats;
    stats.nextFreeLabel = nextFreeId;
    stats.maxLabel = nextFreeId > 0 ? static_cast<SegmentIdType>(nextFreeId - 1) : 0;

    if (graphBase == nullptr || graphBase->pWorkingSegmentsImage == nullptr || pIgnoredSegmentLabels == nullptr) {
        return stats;
    }

    auto initialImage = segment_puzzler::StreamVByteImageStorage<SegmentIdType>::create(
        *graphBase->pWorkingSegmentsImage->imageInformation(), backgroundId);
    std::unordered_map<SegmentIdType, SegmentIdType> ownerByInitial;
    for (const auto &[id, node] : initialNodes) {
        ownerByInitial.emplace(id, node->getCurrentWorkingNodeLabel());
        segment_puzzler::setVoxelLists(*initialImage, {&node->voxels}, id);
    }

    ConnectedComponentSplitOptions options;
    options.connectivity = connectivity;
    options.ignoredLabels.insert(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end());
    options.ignoredLabels.insert(backgroundId);
    options.nextFreeLabel = nextFreeId;
    stats = splitDisconnectedLabelComponentsInPlace(initialImage, options);
    if (!stats.changed()) return stats;

    for (const auto &[original, parts] : stats.finalLabelsByOriginalLabel) {
        const auto owner = ownerByInitial.at(original);
        for (const auto id : parts) ownerByInitial.emplace(id, owner);
    }
    auto groups = segment_puzzler::connected_components::connectedWorkingGroups(*initialImage, ownerByInitial, connectivity);
    auto cursor = static_cast<SegmentIdType>(std::min<std::uint64_t>(
        stats.nextFreeLabel, std::numeric_limits<SegmentIdType>::max()));
    std::map<SegmentIdType, std::vector<SegmentIdType>> plannedNodes;
    for (auto &[owner, parts] : groups) {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto label = i == 0 ? owner : segmentManager.takeNextSegmentId(cursor);
            plannedNodes.emplace(label, std::move(parts[i]));
        }
    }

    // All initial and working IDs are available before the live graph is rebuilt.
    nextFreeId = cursor;
    initializeEdgeVolumeAndEdgeStatus();
    constructInitialNodes(initialImage);
    segmentManager.computeOneSidedEdgesOnAllInitialNodes(*initialImage);
    segmentManager.buildTwoSidedInitialEdgesFromOneSidedInitialEdges();
    for (const auto &[label, members] : plannedNodes)
        segmentManager.addWorkingNode(new WorkingNode(members, label, initialNodes));
    for (const auto &[label, node] : workingNodes)
        segmentManager.recalculateEdgesOnWorkingNode(node.get());
    graphBase->pWorkingSegmentsImage->FillBuffer(backgroundId);
    for (const auto &[label, node] : workingNodes) insertWorkingNodeInSegmentImage(*node);

    for (const auto &[pair, edge] : initialLabelPairToTwoSidedInitialEdge) {
        if (initialNodes.at(pair.first)->getCurrentWorkingNodeLabel() == initialNodes.at(pair.second)->getCurrentWorkingNodeLabel()) {
            edge->setShouldMergeYes();
            graphBase->edgeStatus.at(edge->numId) = 2;
        }
    }

    stats.nextFreeLabel = nextFreeId;
    stats.maxLabel = getLargestIdInSegmentVolume(graphBase->pWorkingSegmentsImage);
    return stats;
}

Graph::SegmentIdType Graph::getLargestIdInSegmentVolume(dataType::SegmentsStorage::Pointer image) {
    SegmentIdType maximum = 0;
    image->forEachRun([&](const auto &, std::size_t, SegmentIdType label) { maximum = std::max(maximum, label); });
    return maximum;
}

bool Graph::splitWorkingNodeByProjected3DCut(const Projected3DCutRequest &request,
                                             Projected3DCutProfile *profileOut,
                                             std::vector<SegmentIdType> *resultingWorkingLabelsOut) {
    using Clock = std::chrono::steady_clock;
    const auto durationMs = [](const Clock::time_point &start, const Clock::time_point &end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    if (profileOut != nullptr) {
        *profileOut = Projected3DCutProfile{};
    }
    if (resultingWorkingLabelsOut != nullptr) {
        resultingWorkingLabelsOut->clear();
    }
    const auto totalStart = Clock::now();
    const auto finish = [&](bool mutated) {
        if (profileOut != nullptr) {
            profileOut->totalMs = durationMs(totalStart, Clock::now());
        }
        return mutated;
    };

    if (graphBase == nullptr || graphBase->pWorkingSegmentsImage == nullptr) {
        return finish(false);
    }
    if (request.targetWorkingLabel == backgroundId || request.strokePixels.size() < 2 ||
        request.viewportSize[0] <= 0 || request.viewportSize[1] <= 0) {
        return finish(false);
    }

    const auto workingNodeIt = workingNodes.find(request.targetWorkingLabel);
    if (workingNodeIt == workingNodes.end() || workingNodeIt->second == nullptr) {
        return finish(false);
    }

    const auto targetWorkingNode = workingNodeIt->second;
    const Roi targetRoi = targetWorkingNode->roi;
    Projected3DCutMaskImage::IndexType globalOffset{{
        targetRoi.minX - 1, targetRoi.minY - 1, targetRoi.minZ - 1}};
    Projected3DCutMaskImage::SizeType localSize{{
        static_cast<Projected3DCutMaskImage::SizeType::SizeValueType>(
            targetRoi.maxX - targetRoi.minX + 3),
        static_cast<Projected3DCutMaskImage::SizeType::SizeValueType>(
            targetRoi.maxY - targetRoi.minY + 3),
        static_cast<Projected3DCutMaskImage::SizeType::SizeValueType>(
            targetRoi.maxZ - targetRoi.minZ + 3)}};
    Projected3DCutMaskImage::IndexType localStart;
    localStart.Fill(0);
    Projected3DCutMaskImage::RegionType localRegion(localStart, localSize);
    Projected3DCutMaskImage::PointType localOrigin;
    graphBase->pWorkingSegmentsImage->TransformIndexToPhysicalPoint(
        globalOffset, localOrigin);
    auto mask = Projected3DCutMaskImage::New();
    mask->SetRegions(localRegion);
    mask->SetSpacing(graphBase->pWorkingSegmentsImage->GetSpacing());
    mask->SetOrigin(localOrigin);
    mask->SetDirection(graphBase->pWorkingSegmentsImage->GetDirection());
    mask->Allocate();
    mask->FillBuffer(0);
    for (const auto &[initialLabel, initialNode] : targetWorkingNode->subInitialNodes) {
        static_cast<void>(initialLabel);
        if (initialNode == nullptr) {
            return finish(false);
        }
        for (const Voxel &voxel : initialNode->voxels) {
            Projected3DCutMaskImage::IndexType localIndex{{
                voxel.x - globalOffset[0],
                voxel.y - globalOffset[1],
                voxel.z - globalOffset[2]}};
            mask->SetPixel(localIndex, 1);
        }
    }

    Projected3DCutResult projectedCut = computeProjected3DCut(mask, request);
    if (profileOut != nullptr) {
        *profileOut = projectedCut.profile;
    }
    if (!projectedCut.valid()) {
        return finish(false);
    }
    const bool mutated = splitWorkingNodeByVoxelPartition(
        request.targetWorkingLabel,
        projectedCut.partition,
        globalOffset,
        resultingWorkingLabelsOut,
        profileOut);
    return finish(mutated);
}

bool Graph::splitWorkingNodeByVoxelPartition(
    SegmentIdType targetWorkingLabel,
    SegmentsImageType::Pointer localPartition,
    const SegmentsImageType::IndexType &globalOffset,
    std::vector<SegmentIdType> *resultingWorkingLabelsOut,
    Projected3DCutProfile *profileOut)
{
    if (resultingWorkingLabelsOut != nullptr) {
        resultingWorkingLabelsOut->clear();
    }
    if (localPartition.IsNull() || targetWorkingLabel == backgroundId) {
        return false;
    }
    const auto targetIt = workingNodes.find(targetWorkingLabel);
    if (targetIt == workingNodes.end() || targetIt->second == nullptr) {
        return false;
    }

    std::vector<Voxel> targetVoxels;
    std::vector<SegmentIdType> targetInitialLabels;
    std::vector<int> targetComponentIds;
    std::size_t targetVoxelCount = 0;
    for (const auto &[initialLabel, initialNode] : targetIt->second->subInitialNodes) {
        if (initialNode == nullptr) {
            return false;
        }
        targetVoxelCount += initialNode->voxels.size();
    }
    targetVoxels.reserve(targetVoxelCount);
    targetInitialLabels.reserve(targetVoxelCount);
    targetComponentIds.reserve(targetVoxelCount);

    const auto partitionRegion = localPartition->GetLargestPossibleRegion();
    SegmentIdType maximumComponentLabel = 0;
    std::size_t assignedPartitionVoxelCount = 0;
    itk::ImageRegionConstIterator<SegmentsImageType> partitionIt(localPartition, partitionRegion);
    for (partitionIt.GoToBegin(); !partitionIt.IsAtEnd(); ++partitionIt) {
        if (partitionIt.Get() != 0) {
            ++assignedPartitionVoxelCount;
            maximumComponentLabel = std::max(maximumComponentLabel, partitionIt.Get());
        }
    }
    if (maximumComponentLabel < 2 || maximumComponentLabel > targetVoxelCount ||
        assignedPartitionVoxelCount != targetVoxelCount || targetVoxelCount > std::numeric_limits<int>::max()) {
        return false;
    }

    std::vector<std::size_t> componentVoxelCounts(maximumComponentLabel, 0);
    for (const auto &[initialLabel, initialNode] : targetIt->second->subInitialNodes) {
        for (const Voxel &voxel : initialNode->voxels) {
            SegmentsImageType::IndexType localIndex{{
                static_cast<SegmentsImageType::IndexType::IndexValueType>(voxel.x) - globalOffset[0],
                static_cast<SegmentsImageType::IndexType::IndexValueType>(voxel.y) - globalOffset[1],
                static_cast<SegmentsImageType::IndexType::IndexValueType>(voxel.z) - globalOffset[2]}};
            if (!partitionRegion.IsInside(localIndex)) {
                return false;
            }
            const SegmentIdType componentLabel = localPartition->GetPixel(localIndex);
            if (componentLabel == 0 || componentLabel > maximumComponentLabel) {
                return false;
            }
            targetVoxels.push_back(voxel);
            targetInitialLabels.push_back(initialLabel);
            targetComponentIds.push_back(static_cast<int>(componentLabel - 1));
            ++componentVoxelCounts[componentLabel - 1];
        }
    }
    if (std::any_of(componentVoxelCounts.begin(), componentVoxelCounts.end(),
                    [](std::size_t count) { return count == 0; })) {
        return false;
    }

    return applyWorkingNodePartition(
        targetWorkingLabel,
        targetVoxels,
        targetInitialLabels,
        targetComponentIds,
        static_cast<int>(maximumComponentLabel),
        profileOut,
        resultingWorkingLabelsOut);
}

bool Graph::applyWorkingNodePartition(
    SegmentIdType targetWorkingLabel,
    const std::vector<Voxel> &targetVoxels,
    const std::vector<SegmentIdType> &targetInitialLabels,
    const std::vector<int> &targetComponentIds,
    int componentCount,
    Projected3DCutProfile *profileOut,
    std::vector<SegmentIdType> *resultingWorkingLabelsOut)
{
    using Clock = std::chrono::steady_clock;
    const auto durationMs = [](const Clock::time_point &start, const Clock::time_point &end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    if (graphBase == nullptr || graphBase->pWorkingSegmentsImage == nullptr || componentCount < 2 ||
        targetVoxels.empty() || targetVoxels.size() != targetInitialLabels.size() ||
        targetVoxels.size() != targetComponentIds.size()) {
        return false;
    }
    const auto targetIt = workingNodes.find(targetWorkingLabel);
    if (targetIt == workingNodes.end() || targetIt->second == nullptr) {
        return false;
    }
    const auto targetWorkingNode = targetIt->second;
    const Roi targetRoi = targetWorkingNode->roi;

    for (const auto &[id, node] : targetWorkingNode->subInitialNodes) {
        const auto registered = initialNodes.find(id);
        if (!node || registered == initialNodes.end() || registered->second != node ||
            node->getCurrentWorkingNodeLabel() != targetWorkingLabel) return false;
    }

    std::unordered_map<SegmentIdType, std::vector<int>> voxelIndicesByInitialLabel;
    voxelIndicesByInitialLabel.reserve(targetWorkingNode->subInitialNodes.size());
    LocalVoxelGrid voxelGrid;
    voxelGrid.minX = targetRoi.minX;
    voxelGrid.minY = targetRoi.minY;
    voxelGrid.minZ = targetRoi.minZ;
    voxelGrid.sizeX = targetRoi.maxX - targetRoi.minX + 1;
    voxelGrid.sizeY = targetRoi.maxY - targetRoi.minY + 1;
    voxelGrid.sizeZ = targetRoi.maxZ - targetRoi.minZ + 1;
    voxelGrid.voxelIndices.assign(
        static_cast<std::size_t>(voxelGrid.sizeX) *
        static_cast<std::size_t>(voxelGrid.sizeY) *
        static_cast<std::size_t>(voxelGrid.sizeZ), -1);
    for (int voxelIndex = 0; voxelIndex < static_cast<int>(targetVoxels.size()); ++voxelIndex) {
        const Voxel &voxel = targetVoxels[static_cast<std::size_t>(voxelIndex)];
        const int componentId = targetComponentIds[static_cast<std::size_t>(voxelIndex)];
        if (componentId < 0 || componentId >= componentCount ||
            !voxelGrid.contains(voxel.x, voxel.y, voxel.z)) {
            return false;
        }
        voxelIndicesByInitialLabel[targetInitialLabels[static_cast<std::size_t>(voxelIndex)]].push_back(voxelIndex);
        voxelGrid.voxelIndices[voxelGrid.linearIndex(voxel.x, voxel.y, voxel.z)] = voxelIndex;
    }

    std::vector<ReplacementInitialComponent> replacementInitialComponents;
    std::vector<unsigned char> replacementVisited(targetVoxels.size(), 0);
    std::vector<int> initialQueue;
    initialQueue.reserve(targetVoxels.size());
    const auto splitReplacementInitialsStart = Clock::now();
    for (const auto &initialEntry : voxelIndicesByInitialLabel) {
        const SegmentIdType initialLabel = initialEntry.first;
        for (const int seedVoxelIndex : initialEntry.second) {
            if (replacementVisited[static_cast<std::size_t>(seedVoxelIndex)] != 0) {
                continue;
            }

            ReplacementInitialComponent component;
            component.finalComponentId = targetComponentIds[static_cast<std::size_t>(seedVoxelIndex)];
            initialQueue.clear();
            initialQueue.push_back(seedVoxelIndex);
            replacementVisited[static_cast<std::size_t>(seedVoxelIndex)] = 1;

            for (std::size_t queueIndex = 0; queueIndex < initialQueue.size(); ++queueIndex) {
                const int activeIndex = initialQueue[queueIndex];
                const Voxel &activeVoxel = targetVoxels[static_cast<std::size_t>(activeIndex)];
                component.voxelIndices.push_back(activeIndex);

                voxelGrid.forEachPresentNeighborIndex(activeVoxel, [&](int neighborIndex) {
                    if (replacementVisited[static_cast<std::size_t>(neighborIndex)] != 0) {
                        return;
                    }
                    if (targetInitialLabels[static_cast<std::size_t>(neighborIndex)] != initialLabel ||
                        targetComponentIds[static_cast<std::size_t>(neighborIndex)] != component.finalComponentId) {
                        return;
                    }

                    replacementVisited[static_cast<std::size_t>(neighborIndex)] = 1;
                    initialQueue.push_back(neighborIndex);
                });
            }

            if (!component.voxelIndices.empty()) {
                replacementInitialComponents.push_back(std::move(component));
            }
        }
    }
    if (profileOut != nullptr) {
        profileOut->replacementInitialCount = replacementInitialComponents.size();
        profileOut->splitReplacementInitialsMs = durationMs(splitReplacementInitialsStart, Clock::now());
    }

    if (replacementInitialComponents.empty()) {
        return false;
    }

    auto nextLabel = nextFreeId;
    std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> replacements;
    std::vector<std::vector<SegmentIdType>> initialLabelsByComponent(componentCount);
    std::vector<std::size_t> replacementVoxelCountsByComponent(componentCount);
    const auto createReplacementInitialsStart = Clock::now();
    double materializeVoxelListsMs = 0.0;
    for (const auto &component : replacementInitialComponents) {
        const auto label = segmentManager.takeNextSegmentId(nextLabel);
        auto node = std::make_shared<InitialNode>(graphBase, graphBase->pWorkingSegmentsImage, label);
        const auto start = Clock::now();
        for (const int index : component.voxelIndices) node->voxels.push_back(targetVoxels[index]);
        if (profileOut) materializeVoxelListsMs += durationMs(start, Clock::now());
        node->roi.updateBoundingRoi(node->voxels);
        replacements.emplace(label, std::move(node));
        initialLabelsByComponent[component.finalComponentId].push_back(label);
        replacementVoxelCountsByComponent[component.finalComponentId] += component.voxelIndices.size();
    }
    if (profileOut) {
        profileOut->materializeReplacementVoxelListsMs = materializeVoxelListsMs;
        profileOut->createReplacementInitialsMs = durationMs(createReplacementInitialsStart, Clock::now());
    }

    const auto recomputeInitialEdgesStart = Clock::now();
    const auto region = getDilatedRegionFromRoi(targetRoi, graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion(), 1);
    std::vector<PreparedInitialEdge> edges;
    if (!prepareSplitEdges(*this, *targetWorkingNode, replacements, region, edges)) {
        logGraph(LogLevel::Error, __func__, QStringLiteral("Could not prepare split edges for working segment %1").arg(targetWorkingLabel));
        return false;
    }
    if (profileOut) profileOut->recomputeInitialEdgesMs = durationMs(recomputeInitialEdgesStart, Clock::now());

    const auto rebuildWorkingNodesStart = Clock::now();
    std::vector<int> componentOrder;
    componentOrder.reserve(static_cast<std::size_t>(componentCount));
    for (int componentId = 0; componentId < componentCount; ++componentId) {
        componentOrder.push_back(componentId);
    }
    std::sort(componentOrder.begin(), componentOrder.end(), [&replacementVoxelCountsByComponent](int lhs, int rhs) {
        const std::size_t lhsCount = replacementVoxelCountsByComponent[static_cast<std::size_t>(lhs)];
        const std::size_t rhsCount = replacementVoxelCountsByComponent[static_cast<std::size_t>(rhs)];
        if (lhsCount != rhsCount) {
            return lhsCount > rhsCount;
        }
        return lhs < rhs;
    });

    std::vector<SegmentIdType> resultingWorkingLabels;
    std::unordered_map<SegmentIdType, std::shared_ptr<WorkingNode>> newWorkingNodes;
    std::vector<dataType::SegmentsStorage::VoxelRange> changedRuns;
    for (const int component : componentOrder) {
        const auto label = resultingWorkingLabels.empty() ? targetWorkingLabel : segmentManager.takeNextSegmentId(nextLabel);
        auto node = std::make_shared<WorkingNode>(initialLabelsByComponent[component], label, replacements);
        if (label != targetWorkingLabel) {
            for (const auto *list : node->getVoxelLists()) {
                for (const auto &run : list->getRuns()) {
                    changedRuns.push_back({{run.start.x, run.start.y, run.start.z}, run.length, label});
                }
            }
        }
        newWorkingNodes.emplace(label, std::move(node));
        resultingWorkingLabels.push_back(label);
    }
    if (profileOut) profileOut->rebuildWorkingNodesMs = durationMs(rebuildWorkingNodesStart, Clock::now());

    const auto rewriteWorkingImageStart = Clock::now();
    nextFreeId = nextLabel;
    graphBase->pWorkingSegmentsImage->fillVoxelRanges(changedRuns);
    if (profileOut) profileOut->rewriteWorkingImageMs = durationMs(rewriteWorkingImageStart, Clock::now());

    const auto removeOriginalNodesStart = Clock::now();
    segmentManager.removeWorkingNode(targetWorkingNode.get());
    for (const auto &[id, node] : targetWorkingNode->subInitialNodes) segmentManager.removeInitialNode(id);
    if (profileOut) profileOut->removeOriginalNodesMs = durationMs(removeOriginalNodesStart, Clock::now());

    initialNodes.merge(replacements);
    workingNodes.merge(newWorkingNodes);
    for (auto &contact : edges) contact.install(*this, segmentManager);
    const auto recalculateWorkingEdgesStart = Clock::now();
    for (const auto label : resultingWorkingLabels) {
        segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(label).get());
    }
    if (profileOut) profileOut->recalculateWorkingEdgesMs = durationMs(recalculateWorkingEdgesStart, Clock::now());

    if (resultingWorkingLabelsOut != nullptr) {
        *resultingWorkingLabelsOut = std::move(resultingWorkingLabels);
    }
    return true;
}


Graph::SegmentsImageType::RegionType Graph::getDilatedRegionFromRoi(
        const Roi &roi,
        const SegmentsImageType::RegionType &imageRegion,
        int numberVxDilations) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Calculating dilated ROI region"));
    if (numberVxDilations < 0 ||
        roi.minX > roi.maxX || roi.minY > roi.maxY || roi.minZ > roi.maxZ) {
        throw std::invalid_argument("Cannot dilate an invalid refinement ROI.");
    }

    const auto imageStart = imageRegion.GetIndex();
    const auto imageSize = imageRegion.GetSize();
    SegmentsImageType::IndexType start;
    SegmentsImageType::SizeType size;
    const std::array<long long, 3> roiMinimum{{roi.minX, roi.minY, roi.minZ}};
    const std::array<long long, 3> roiMaximum{{roi.maxX, roi.maxY, roi.maxZ}};

    for (unsigned int axis = 0; axis < 3; ++axis) {
        if (imageSize[axis] == 0) {
            throw std::invalid_argument("Cannot dilate an ROI inside an empty image region.");
        }
        const long long minimum = static_cast<long long>(imageStart[axis]);
        const long long maximum = minimum + static_cast<long long>(imageSize[axis]) - 1;
        const long long dilatedMinimum = std::max(
            minimum,
            roiMinimum[axis] - static_cast<long long>(numberVxDilations));
        const long long dilatedMaximum = std::min(
            maximum,
            roiMaximum[axis] + static_cast<long long>(numberVxDilations));
        if (dilatedMinimum > dilatedMaximum) {
            throw std::invalid_argument("Refinement ROI lies outside the working image.");
        }
        start[axis] = static_cast<SegmentsImageType::IndexType::IndexValueType>(dilatedMinimum);
        size[axis] = static_cast<SegmentsImageType::SizeType::SizeValueType>(
            dilatedMaximum - dilatedMinimum + 1);
    }
    return {start, size};
}


// highlevel workflow:
// calculate overlap of initial voxels with the refinement
// if edge connects two segments with the same overlaplabel, both greater than threshold, merge them
// if overlap higher than threshold, merge them

// future:
// use different labels for manually merged segments than automatically merged labels
// override automatic decision with manual decision, if available
void Graph::mergeSegmentsWithRefinement() {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Merging segments with refinement"));


    if (graphBase->pSelectedRefinement != nullptr) {

        std::map<dataType::SegmentIdType, LabelOverlap> overlapMap;
        double mergeThreshold = 0.75;
        for (auto &node : initialNodes) {
            overlapMap[node.first] = LabelOverlap();
            overlapMap[node.first].setPToOtherLabelImagePointer(graphBase->pSelectedRefinement);
            overlapMap[node.first].compute(node.second->getVoxelLists());
//            std::cout << "label: " << overlapMap[node.first].labelWithLargestOverlap << " " <<
//                      overlapMap[node.first].largestOverlapFraction << "\n";
        }

        ScopedGraphTimer mergeTimer(verbose, __func__, QStringLiteral("Collecting refinement-driven edge merges"));
//        get vec of edges to merge to then call mergeEdges on all of them
        std::set<EdgeNumIdType> edgesToMerge;
        for (auto &edge : initialLabelPairToTwoSidedInitialEdge) {
            dataType::SegmentIdType labelA = edge.first.first;
            dataType::SegmentIdType labelB = edge.first.second;
            double overlapLabelA = overlapMap[labelA].largestOverlapFraction;
            double overlapLabelB = overlapMap[labelB].largestOverlapFraction;
            long int labelWithLargestOverlapA = overlapMap[labelA].labelWithLargestOverlap;
            long int labelWithLargestOverlapB = overlapMap[labelB].labelWithLargestOverlap;

            bool bothPointToSameLabel = labelWithLargestOverlapA == labelWithLargestOverlapB;
            bool thresholdReached = (overlapLabelA > mergeThreshold) && (overlapLabelB > mergeThreshold);

            if (bothPointToSameLabel && thresholdReached) {
                edgesToMerge.insert(edge.second->numId);
//                mergeEdge(edge.second.get());
            }

        }
        mergeEdges(edgesToMerge);


//    for (auto &feature : nodeFeatures) {
//        feature->compute(voxels);
//    }

    }
}

void Graph::transferSegmentsWithRefinementOverlap(double overlapThreshold) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring segments by refinement overlap"));

    if (!std::isfinite(overlapThreshold) || overlapThreshold < 0.0 || overlapThreshold > 1.0) {
        throw std::invalid_argument("Refinement overlap threshold must be between 0 and 1.");
    }
    if (graphBase == nullptr || graphBase->pSelectedSegmentation == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No selected segmentation is loaded"));
        return;
    }
    if (graphBase->pSelectedRefinement == nullptr || graphBase->pWorkingSegmentsImage == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No selected refinement or working segments are loaded"));
        return;
    }
    if (!regionsMatch(graphBase->pSelectedRefinement->GetLargestPossibleRegion(),
                      graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion())) {
        logGraph(LogLevel::Warning,
                 __func__,
                 QStringLiteral("Selected refinement and working segments use incompatible image regions"));
        return;
    }

    std::vector<SegmentIdType> workingLabelsToTransfer;
    std::size_t evaluatedWorkingNodeCount = 0;
    std::size_t possibleMergeCount = 0;
    const bool logOverlaps = isGraphDebugLoggingEnabled(verbose);
    for (const auto &[workingLabel, node] : workingNodes) {
        if (node == nullptr) {
            continue;
        }
        ++evaluatedWorkingNodeCount;
        LabelOverlap overlapFeature;
        overlapFeature.setPToOtherLabelImagePointer(graphBase->pSelectedRefinement);
        overlapFeature.compute(node->getVoxelLists());

        const bool selectedByOverlap =
            overlapFeature.largestNonZeroOverlapFraction > overlapThreshold;
        const bool possibleMerge =
            selectedByOverlap &&
            overlapFeature.secondLargestNonZeroOverlapFraction >=
                kPossibleMergeSecondOverlapThreshold;

        if (possibleMerge || logOverlaps) {
            logGraph(
                possibleMerge ? LogLevel::Info : LogLevel::Debug,
                __func__,
                QStringLiteral(
                    "Working node %1 label_with_largest_overlap=%2 largest_overlap_fraction=%3 "
                    "largest_nonzero_overlap_fraction=%4 label_with_second_largest_nonzero_overlap=%5 "
                    "second_largest_nonzero_overlap_fraction=%6 selected=%7 possible_merge=%8")
                    .arg(workingLabel)
                    .arg(overlapFeature.labelWithLargestOverlap)
                    .arg(QString::number(overlapFeature.largestOverlapFraction, 'g', 6))
                    .arg(QString::number(overlapFeature.largestNonZeroOverlapFraction, 'g', 6))
                    .arg(overlapFeature.labelWithSecondLargestNonZeroOverlap)
                    .arg(QString::number(overlapFeature.secondLargestNonZeroOverlapFraction, 'g', 6))
                    .arg(selectedByOverlap)
                    .arg(possibleMerge));
        }
        if (possibleMerge) {
            ++possibleMergeCount;
        }
        if (selectedByOverlap) {
            workingLabelsToTransfer.push_back(workingLabel);
        }
    }

    std::sort(workingLabelsToTransfer.begin(), workingLabelsToTransfer.end());
    const auto assignedSegmentationLabels = transferWorkingNodesToSegmentation(workingLabelsToTransfer);

    logGraph(
        LogLevel::Info,
        __func__,
        QStringLiteral(
            "Refinement-overlap transfer threshold=%1 evaluated=%2 selected=%3 transferred=%4 "
            "possible_merges=%5")
            .arg(QString::number(overlapThreshold, 'g', 6))
            .arg(static_cast<qulonglong>(evaluatedWorkingNodeCount))
            .arg(static_cast<qulonglong>(workingLabelsToTransfer.size()))
            .arg(static_cast<qulonglong>(assignedSegmentationLabels.size()))
            .arg(static_cast<qulonglong>(possibleMergeCount)));
}

void Graph::transferSegmentsWithVolumeCriterion(double volumeThreshold) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring segments by volume threshold"));
    if (!graphBase->pSelectedSegmentation) return;
    const bool logVolumes = isGraphDebugLoggingEnabled(verbose);
    std::vector<SegmentIdType> labels;
    for (const auto &[label, node] : workingNodes) {
        if (label == backgroundId || (pIgnoredSegmentLabels && isIgnoredId(label))) continue;
        std::size_t voxelCount = 0;
        for (const auto &entry : node->subInitialNodes) voxelCount += entry.second->voxels.size();
        if (logVolumes) logGraph(LogLevel::Debug, __func__,
            QStringLiteral("Working node %1 volume=%2").arg(label).arg(static_cast<qulonglong>(voxelCount)));
        if (voxelCount > volumeThreshold) labels.push_back(label);
    }
    transferWorkingNodesToSegmentation(labels);
}

void Graph::transferNonOverlappingSegments(
        double maximumOverlapFraction,
        double minimumVoxelCount,
        bool preserveExistingSegmentation) {
    std::vector<SegmentIdType> workingLabels;
    workingLabels.reserve(workingNodes.size());
    for (const auto &entry : workingNodes) {
        workingLabels.push_back(entry.first);
    }
    transferNonOverlappingWorkingNodesToSegmentation(
        workingLabels, maximumOverlapFraction, minimumVoxelCount, preserveExistingSegmentation);
}

std::optional<Graph::SegmentIdType> Graph::transferNonOverlappingWorkingNodeToSegmentation(
        int x, int y, int z, double maximumOverlapFraction,
        bool preserveExistingSegmentation, NonOverlappingTransferRejection *rejection) {
    if (rejection != nullptr) {
        *rejection = NonOverlappingTransferRejection::None;
    }
    if (graphBase == nullptr || graphBase->pWorkingSegmentsImage == nullptr) {
        return std::nullopt;
    }
    const SegmentsImageType::IndexType index{{x, y, z}};
    if (!graphBase->pWorkingSegmentsImage->GetBufferedRegion().IsInside(index)) {
        return std::nullopt;
    }
    const SegmentIdType workingLabel = graphBase->pWorkingSegmentsImage->GetPixel(index);
    const auto assignedLabels = transferNonOverlappingWorkingNodesToSegmentation(
        {workingLabel}, maximumOverlapFraction, 0, preserveExistingSegmentation, rejection);
    if (assignedLabels.empty()) {
        return std::nullopt;
    }
    return assignedLabels.front();
}

std::vector<Graph::SegmentIdType> Graph::transferNonOverlappingWorkingNodesToSegmentation(
        const std::vector<SegmentIdType> &workingLabels,
        double maximumOverlapFraction, double minimumVoxelCount,
        bool preserveExistingSegmentation, NonOverlappingTransferRejection *rejection) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring non-overlapping supervoxels"));
    if (!std::isfinite(maximumOverlapFraction) ||
        maximumOverlapFraction < 0.0 || maximumOverlapFraction > 1.0) {
        throw std::invalid_argument("Maximum overlap fraction must be between 0 and 1.");
    }
    if (!std::isfinite(minimumVoxelCount) || minimumVoxelCount < 0.0) {
        throw std::invalid_argument("Minimum voxel count must be finite and non-negative.");
    }
    if (graphBase == nullptr || graphBase->pSelectedSegmentation == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No selected segmentation is loaded"));
        return {};
    }
    if (graphBase->pWorkingSegmentsImage == nullptr ||
        !regionsMatch(graphBase->pSelectedSegmentation->GetLargestPossibleRegion(),
                      graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion())) {
        logGraph(LogLevel::Warning,
                 __func__,
                 QStringLiteral("Selected segmentation and working segments use incompatible image regions"));
        return {};
    }

    // Converting a UI percentage to a fraction can round down by one representable step.
    const double overlapLimit = maximumOverlapFraction > 0.0
        ? std::nextafter(maximumOverlapFraction, 1.0) : 0.0;
    const auto selectedRegion = graphBase->pSelectedSegmentation->GetBufferedRegion();
    std::vector<SegmentIdType> workingLabelsToTransfer;
    for (const SegmentIdType workingLabel : workingLabels) {
        const auto nodeIt = workingNodes.find(workingLabel);
        if (nodeIt == workingNodes.end() || nodeIt->second == nullptr || workingLabel == backgroundId ||
            (pIgnoredSegmentLabels != nullptr && isIgnoredId(workingLabel))) {
            continue;
        }
        const auto voxelLists = nodeIt->second->getVoxelLists();
        std::size_t voxelCount = 0;
        for (const auto *voxelList : voxelLists) {
            if (voxelList == nullptr) {
                throw std::invalid_argument("Working supervoxel contains a null voxel list.");
            }
            voxelCount += voxelList->size();
        }
        if (voxelCount == 0 || voxelCount <= minimumVoxelCount) {
            continue;
        }

        std::size_t overlappingVoxelCount = 0;
        for (const auto *voxelList : voxelLists) {
            for (const Voxel &voxel : *voxelList) {
                const SegmentsImageType::IndexType index{{voxel.x, voxel.y, voxel.z}};
                if (!selectedRegion.IsInside(index)) {
                    throw std::invalid_argument("Working supervoxel lies outside the selected segmentation.");
                }
                if (graphBase->pSelectedSegmentation->GetPixel(index) != selectedBackground()) {
                    ++overlappingVoxelCount;
                }
            }
        }
        if (preserveExistingSegmentation && overlappingVoxelCount == voxelCount) {
            if (rejection != nullptr) {
                *rejection = NonOverlappingTransferRejection::NoEmptyVoxels;
            }
            continue;
        }
        const double overlapFraction =
            static_cast<double>(overlappingVoxelCount) / static_cast<double>(voxelCount);
        if (overlapFraction <= overlapLimit) {
            workingLabelsToTransfer.push_back(workingLabel);
        } else if (rejection != nullptr) {
            *rejection = NonOverlappingTransferRejection::OverlapExceeded;
        }
    }

    std::sort(workingLabelsToTransfer.begin(), workingLabelsToTransfer.end());
    return transferWorkingNodesToSegmentation(workingLabelsToTransfer, preserveExistingSegmentation);
}

void Graph::setBackgroundIdStrategy(const std::string& backgroundIdStrategyIn) {
    backgroundIdStrategy = backgroundIdStrategyIn;
    logGraphDebugIf(verbose,
                    __func__,
                    QStringLiteral("Background id strategy set to %1")
                        .arg(QString::fromStdString(backgroundIdStrategy)));
}

void Graph::updateBackgroundIdFromVolume(SegmentsImageType::Pointer pImage, std::optional<SegmentIdType> sourceBackground) {
    updateBackgroundIdFromVolume(segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(pImage), sourceBackground);
}

void Graph::updateBackgroundIdFromVolume(dataType::SegmentsStorage::Pointer pImage, std::optional<SegmentIdType> sourceBackground) {
    logGraphDebugIf(verbose,
                    __func__,
                    QStringLiteral("Resolving background id using strategy %1")
                        .arg(QString::fromStdString(backgroundIdStrategy)));
    if (sourceBackground) {
        backgroundId = *sourceBackground;
    } else {
        SegmentIdType minimum = std::numeric_limits<SegmentIdType>::max(), maximum = 0;
        pImage->forEachRun([&](const auto &, std::size_t, SegmentIdType label) {
            minimum = std::min(minimum, label);
            maximum = std::max(maximum, label);
        });
        if (backgroundIdStrategy == "backgroundIsHighestId") backgroundId = maximum;
        else if (backgroundIdStrategy == "backgroundIsLowestId") backgroundId = minimum;
        else throw std::invalid_argument("Received unknown backgroundIdStrategy in Graph::updateBackgroundIdFromVolume");
    }
    logGraphDebugIf(verbose, __func__, QStringLiteral("Background id resolved to %1").arg(backgroundId));
}

void Graph::deleteSegmentationLabel(SegmentIdType label) {
    deleteSegmentationLabels({label});
}

std::size_t Graph::deleteSegmentationLabels(
        const std::unordered_set<SegmentIdType> &labels) {
    if (graphBase == nullptr || graphBase->pSelectedSegmentation == nullptr || labels.empty()) {
        return 0;
    }

    QElapsedTimer timer;
    timer.start();
    std::size_t deletedVoxelCount = 0;
    const auto background = selectedBackground();
    dataType::SegmentsStorage::LabelStatistics selected;
    for (const auto &[label, statistics] : graphBase->pSelectedSegmentation->labelStatistics())
        if (label != background && labels.count(label)) selected.include(statistics.bounds, statistics.voxelCount);
    if (!selected.voxelCount) return 0;
    auto slice = selected.bounds;
    const auto firstZ = slice.GetIndex()[2];
    const auto endZ = firstZ + static_cast<itk::IndexValueType>(slice.GetSize()[2]);
    std::vector<dataType::SegmentsStorage::VoxelRange> ranges;
    for (auto z = firstZ; z < endZ; z += 16) {
        slice.SetIndex(2, z);
        slice.SetSize(2, std::min<itk::IndexValueType>(16, endZ - z));
        graphBase->pSelectedSegmentation->forEachRun(slice, [&](const auto &index, std::size_t length, SegmentIdType label) {
            if (label != background && labels.count(label)) {
                ranges.push_back({index, length, background});
                deletedVoxelCount += length;
            }
        });
        graphBase->pSelectedSegmentation->fillVoxelRanges(ranges);
        ranges.clear();
    }

    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("operation=delete_segmentation_labels labels=%1 deleted_voxels=%2 elapsed_ms=%3")
            .arg(labels.size())
            .arg(deletedVoxelCount)
            .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000.0, 0, 'f', 3));
    return deletedVoxelCount;
}

std::vector<Graph::SegmentIdType> Graph::selectedSegmentationLabelsBelowVoxelCount(
        std::size_t exclusiveVoxelThreshold) const {
    std::vector<SegmentIdType> labels;
    if (exclusiveVoxelThreshold == 0 || graphBase == nullptr ||
        graphBase->pSelectedSegmentation == nullptr) {
        return labels;
    }

    QElapsedTimer timer;
    timer.start();
    std::unordered_set<SegmentIdType> ignoredLabels{selectedBackground()};
    if (pIgnoredSegmentLabels) {
        for (const auto label : *pIgnoredSegmentLabels)
            if (label != backgroundId) ignoredLabels.insert(label);
    }

    for (const auto &[label, statistics] : graphBase->pSelectedSegmentation->labelStatistics())
        if (!ignoredLabels.count(label) && statistics.voxelCount < exclusiveVoxelThreshold)
            labels.push_back(label);
    std::sort(labels.begin(), labels.end());

    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("operation=merge_small_segments phase=threshold_scan threshold=%1 "
                       "matching_labels=%2 elapsed_ms=%3")
            .arg(exclusiveVoxelThreshold)
            .arg(labels.size())
            .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000.0, 0, 'f', 3));
    return labels;
}

Graph::SegmentationNeighborMergeResult
Graph::mergeSelectedSegmentationLabelsWithNeighbors(
        const std::vector<SegmentIdType> &requestedLabels,
        const SegmentationNeighborMergeOptions &options) {
    using MergeResult = SegmentationNeighborMergeResult;
    using MergeStatus = MergeResult::Status;

    const char *operationName = __func__;
    const char *selectionName = segmentationNeighborSelectionName(options.neighborSelection);
    const QString selectionNameForLog =
        QString::fromLatin1(selectionName != nullptr ? selectionName : "unknown");
    ScopedGraphTimer timer(
        verbose, operationName, QStringLiteral("Merging selected segmentation labels with size-selected neighbors"));
    MergeResult result;
    const auto finish = [this, operationName, &requestedLabels, &options, &result, selectionNameForLog](
                            MergeStatus status, QString message) {
        result.status = status;
        result.message = std::move(message);
        LogLevel level = LogLevel::Info;
        if (status == MergeStatus::Failed) {
            level = result.dataChanged ? LogLevel::Error : LogLevel::Warning;
        }
        logGraph(
            level,
            operationName,
            QStringLiteral("operation=merge_with_neighbor status=%1 requested=[%2] neighbor_selection=%3 "
                           "allow_insertion=%4 allow_component_split=%5 selected=%6 mergeable=%7 "
                           "skipped=%8 insertions=%9 disconnected_labels=%10 disconnected_regions=%11 "
                           "consumed=%12 groups=%13 data_changed=%14 message=\"%15\"")
                .arg(QString::fromLatin1(neighborMergeStatusName(status)))
                .arg(joinIds(requestedLabels))
                .arg(selectionNameForLog)
                .arg(options.allowInsertion)
                .arg(options.allowConnectedComponentSplit)
                .arg(result.selectedLabelCount)
                .arg(result.mergeableSelectedLabelCount)
                .arg(result.skippedNoNeighborCount)
                .arg(result.requiredInsertionCount)
                .arg(result.disconnectedLabelCount)
                .arg(result.disconnectedRegionCount)
                .arg(result.consumedLabelCount)
                .arg(result.mergedGroupCount)
                .arg(result.dataChanged)
                .arg(result.message));
        return result;
    };

    logGraph(
        LogLevel::Info,
        operationName,
        QStringLiteral("operation=merge_with_neighbor phase=begin requested=[%1] neighbor_selection=%2 "
                       "allow_insertion=%3 allow_component_split=%4")
            .arg(joinIds(requestedLabels))
            .arg(selectionNameForLog)
            .arg(options.allowInsertion)
            .arg(options.allowConnectedComponentSplit));

    if (graphBase == nullptr || graphBase->pSelectedSegmentation == nullptr ||
        graphBase->pWorkingSegmentsImage == nullptr || pIgnoredSegmentLabels == nullptr) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("Selected Segmentation and Working Segments must both be available."));
    }
    if (options.allowConnectedComponentSplit && !options.allowInsertion) {
        return finish(
            MergeStatus::Failed,
            QStringLiteral("Connected-component splitting requires insertion permission."));
    }

    const auto selectedRegion = graphBase->pSelectedSegmentation->GetLargestPossibleRegion();
    const auto workingRegion = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion();
    if (!regionsMatch(selectedRegion, workingRegion)) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("Selected Segmentation and Working Segments use incompatible image regions."));
    }

    std::set<SegmentIdType> initialSelectedLabels(requestedLabels.begin(), requestedLabels.end());
    result.selectedLabelCount = initialSelectedLabels.size();
    if (initialSelectedLabels.empty()) {
        return finish(MergeStatus::NothingToMerge, QStringLiteral("No segments are selected."));
    }
    if (initialSelectedLabels.count(selectedBackground()) > 0) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("The background label cannot be merged with a neighbor."));
    }

    using segment_puzzler::connected_components::ConnectedComponentSplitOptions;
    using segment_puzzler::connected_components::ConnectedComponentSplitStats;
    using segment_puzzler::connected_components::ConnectivityStencil;
    using segment_puzzler::connected_components::countConnectedComponentsByLabelInRegions;
    using segment_puzzler::connected_components::splitDisconnectedLabelComponentsInPlace;

    std::vector<SegmentIdType> activeRequestedLabels(requestedLabels);
    std::unordered_set<SegmentIdType> ignoredLabels{selectedBackground()};
    for (const auto label : *pIgnoredSegmentLabels)
        if (label != backgroundId) ignoredLabels.insert(label);
    std::size_t totalDisconnectedLabelCount = 0;
    std::size_t totalDisconnectedRegionCount = 0;
    NeighborMergePlan plan;

    while (true) {
        QElapsedTimer planScanTimer;
        planScanTimer.start();
        plan = buildNeighborMergePlan(
            graphBase->pSelectedSegmentation,
            selectedBackground(),
            ignoredLabels,
            activeRequestedLabels,
            options.neighborSelection);
        const double planScanMs = static_cast<double>(planScanTimer.nsecsElapsed()) / 1000000.0;
        if (!plan.valid()) {
            return finish(MergeStatus::Failed, plan.error);
        }

        result.selectedLabelCount = plan.selectedLabels.size();
        result.mergeableSelectedLabelCount = plan.labelPairs.size();
        result.skippedNoNeighborCount = plan.skippedNoNeighborCount;
        result.consumedLabelCount = plan.consumedLabels.size();
        logGraphDebugIf(
            verbose,
            operationName,
            QStringLiteral("operation=merge_with_neighbor phase=plan pairs=[%1] skipped=%2 consumed=[%3] "
                           "selected=%4 scan_ms=%5")
                .arg(neighborMergePlanSummary(plan))
                .arg(plan.skippedNoNeighborCount)
                .arg(joinIds(plan.consumedLabels))
                .arg(plan.selectedLabels.size())
                .arg(planScanMs, 0, 'f', 3));

        if (plan.labelPairs.empty()) {
            return finish(MergeStatus::NothingToMerge,
                          QStringLiteral("No selected segment has an eligible face-neighbor."));
        }

        std::unordered_map<SegmentIdType, SegmentsImageType::RegionType> connectivityRegions;
        connectivityRegions.reserve(plan.consumedLabels.size());
        std::uintmax_t connectivityRoiVoxels = 0;
        for (const SegmentIdType label : plan.consumedLabels) {
            const auto region = plan.statsByLabel.at(label).boundingRegion();
            connectivityRegions.emplace(label, region);
            connectivityRoiVoxels += static_cast<std::uintmax_t>(region.GetNumberOfPixels());
        }
        QElapsedTimer connectivityTimer;
        connectivityTimer.start();
        const auto componentCounts = countConnectedComponentsByLabelInRegions(
            graphBase->pSelectedSegmentation,
            connectivityRegions,
            ConnectivityStencil::SixConnected);
        const double connectivityMs =
            static_cast<double>(connectivityTimer.nsecsElapsed()) / 1000000.0;

        std::set<SegmentIdType> disconnectedLabels;
        std::size_t disconnectedRegionCount = 0;
        QStringList componentSummary;
        for (const SegmentIdType label : plan.consumedLabels) {
            const auto count = componentCounts.find(label);
            if (count == componentCounts.end() || count->second == 0) {
                return finish(MergeStatus::Failed,
                              QStringLiteral("Could not validate the connectivity of label %1.").arg(label));
            }
            componentSummary << QStringLiteral("%1=%2").arg(label).arg(count->second);
            if (count->second > 1) {
                disconnectedLabels.insert(label);
                disconnectedRegionCount += count->second;
            }
        }
        logGraphDebugIf(
            verbose,
            operationName,
            QStringLiteral("operation=merge_with_neighbor phase=connectivity components=[%1] "
                           "roi_voxels=%2 elapsed_ms=%3")
                .arg(componentSummary.join(QStringLiteral(", ")))
                .arg(connectivityRoiVoxels)
                .arg(connectivityMs, 0, 'f', 3));

        if (disconnectedLabels.empty()) {
            break;
        }

        if (!options.allowConnectedComponentSplit) {
            result.disconnectedLabelCount = disconnectedLabels.size();
            result.disconnectedRegionCount = disconnectedRegionCount;
            return finish(
                MergeStatus::NeedsConnectedComponentConfirmation,
                QStringLiteral("At least one involved segment is disconnected (%1 regions were found).")
                    .arg(disconnectedRegionCount));
        }

        totalDisconnectedLabelCount += disconnectedLabels.size();
        totalDisconnectedRegionCount += disconnectedRegionCount;
        result.disconnectedLabelCount = totalDisconnectedLabelCount;
        result.disconnectedRegionCount = totalDisconnectedRegionCount;

        const SegmentIdType effectiveMaximum =
            std::max(selectedLabelHighWaterMark(), plan.actualMaximumLabel);
        const std::size_t newLabelCount = disconnectedRegionCount - disconnectedLabels.size();
        if (newLabelCount >
            std::numeric_limits<SegmentIdType>::max() - static_cast<std::uintmax_t>(effectiveMaximum)) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("No free Selected Segmentation labels remain for connected components."));
        }

        ConnectedComponentSplitOptions splitOptions;
        splitOptions.connectivity = ConnectivityStencil::SixConnected;
        splitOptions.nextFreeLabel = nextSelectedLabel();
        splitOptions.ignoredLabels.insert(selectedBackground());
        for (const auto &[label, stats] : plan.statsByLabel) {
            (void)stats;
            if (disconnectedLabels.count(label) == 0) {
                splitOptions.ignoredLabels.insert(label);
            }
        }

        result.dataChanged = true;
        ConnectedComponentSplitStats splitStats;
        try {
            splitStats = splitDisconnectedLabelComponentsInPlace(
                graphBase->pSelectedSegmentation, splitOptions);
        } catch (const std::exception &exception) {
            return finish(
                MergeStatus::Failed,
                QStringLiteral("Connected Components failed: %1")
                    .arg(QString::fromUtf8(exception.what())));
        } catch (...) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Connected Components failed with an unknown error."));
        }
        if (splitStats.labelsSplit != disconnectedLabels.size() ||
            splitStats.componentsCreated != newLabelCount) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Connected Components did not produce the expected regions."));
        }

        graphBase->selectedSegmentationContext->nextLabel = splitStats.nextFreeLabel;
        graphBase->pSelectedSegmentation->Modified();
        logGraphDebugIf(
            verbose,
            operationName,
            QStringLiteral("operation=merge_with_neighbor phase=component_split mapping={%1} relabeled_voxels=%2")
                .arg(connectedComponentMappingSummary(splitStats.finalLabelsByOriginalLabel))
                .arg(splitStats.voxelsRelabeled));

        std::vector<SegmentIdType> expandedRequestedLabels;
        for (const SegmentIdType requestedLabel : plan.selectedLabels) {
            const auto splitLabels = splitStats.finalLabelsByOriginalLabel.find(requestedLabel);
            if (splitLabels == splitStats.finalLabelsByOriginalLabel.end()) {
                expandedRequestedLabels.push_back(requestedLabel);
                continue;
            }
            expandedRequestedLabels.insert(
                expandedRequestedLabels.end(), splitLabels->second.begin(), splitLabels->second.end());
        }
        activeRequestedLabels = std::move(expandedRequestedLabels);
    }

    result.disconnectedLabelCount = totalDisconnectedLabelCount;
    result.disconnectedRegionCount = totalDisconnectedRegionCount;
    const auto &statsByLabel = plan.statsByLabel;
    const auto &plannedLabelPairs = plan.labelPairs;
    const auto &consumedLabels = plan.consumedLabels;


    QElapsedTimer preflightTimer;
    preflightTimer.start();
    std::vector<SegmentIdType> labelsNeedingInsertion;
    std::unordered_map<SegmentIdType, SegmentIdType> exactWorkingLabelBySelectedLabel;
    exactWorkingLabelBySelectedLabel.reserve(consumedLabels.size());
    std::size_t preflightVoxelCount = 0;
    for (const SegmentIdType label : consumedLabels) {
        const std::size_t selectedLabelVoxelCount = statsByLabel.at(label).voxelCount;
        preflightVoxelCount += selectedLabelVoxelCount;
        const auto &seed = statsByLabel.at(label).seed;
        const SegmentIdType workingLabel = graphBase->pWorkingSegmentsImage->GetPixel(seed);
        if (workingLabel == backgroundId) {
            labelsNeedingInsertion.push_back(label);
            continue;
        }
        const auto workingNode = workingNodes.find(workingLabel);
        if (workingNode == workingNodes.end() || workingNode->second == nullptr) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Working label %1 is missing from the WorkingGraph.")
                              .arg(workingLabel));
        }
        const ComponentMatch match = selectedComponentMatchesWorkingNode(
            graphBase->pSelectedSegmentation,
            label,
            seed,
            *workingNode->second,
            selectedLabelVoxelCount);
        if (match == ComponentMatch::Invalid) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Working node %1 contains invalid voxel metadata.")
                              .arg(workingLabel));
        }
        if (match != ComponentMatch::Exact) {
            labelsNeedingInsertion.push_back(label);
        } else {
            exactWorkingLabelBySelectedLabel.emplace(label, workingLabel);
        }
    }
    const double preflightMs =
        static_cast<double>(preflightTimer.nsecsElapsed()) / 1000000.0;

    result.requiredInsertionCount = labelsNeedingInsertion.size();
    logGraphDebugIf(
        verbose,
        operationName,
        QStringLiteral("operation=merge_with_neighbor phase=insertion_preflight reused=%1 "
                       "insertions=%2 validation=count_based validated_voxels=%3 "
                       "elapsed_ms=%4 labels=[%5]")
            .arg(exactWorkingLabelBySelectedLabel.size())
            .arg(labelsNeedingInsertion.size())
            .arg(preflightVoxelCount)
            .arg(preflightMs, 0, 'f', 3)
            .arg(joinIds(labelsNeedingInsertion)));
    if (!labelsNeedingInsertion.empty() && !options.allowInsertion) {
        return finish(
            MergeStatus::NeedsInsertionConfirmation,
            QStringLiteral("%1 involved segment(s) must be inserted into Working Segments before merging.")
                .arg(labelsNeedingInsertion.size()));
    }

    // Reuse the exact preflight resolutions. Only labels that actually need
    // insertion go through the generic, potentially mutating resolver.
    QElapsedTimer resolutionTimer;
    resolutionTimer.start();
    std::unordered_map<SegmentIdType, SegmentIdType> workingLabelBySelectedLabel =
        exactWorkingLabelBySelectedLabel;
    const auto seedFitsWorkingCoordinates = [](const SegmentsImageType::IndexType &seed) {
        return seed[0] >= std::numeric_limits<int>::min() && seed[0] <= std::numeric_limits<int>::max() &&
               seed[1] >= std::numeric_limits<int>::min() && seed[1] <= std::numeric_limits<int>::max() &&
               seed[2] >= std::numeric_limits<int>::min() && seed[2] <= std::numeric_limits<int>::max();
    };
    std::size_t insertedLabelCount = 0;
    for (const SegmentIdType label : labelsNeedingInsertion) {
        const auto &seed = statsByLabel.at(label).seed;
        if (!seedFitsWorkingCoordinates(seed)) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Label %1 lies outside the coordinate range supported by Working Segments.")
                              .arg(label));
        }
        // Conservatively report a changed graph even if an insertion fails midway.
        result.dataChanged = true;
        const auto resolution = ensureSelectedSegmentationComponentInWorkingGraph(
            static_cast<int>(seed[0]), static_cast<int>(seed[1]), static_cast<int>(seed[2]));
        if (resolution.status != WorkingSegmentResolution::Status::ReusedExisting &&
            resolution.status != WorkingSegmentResolution::Status::Inserted) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Could not resolve selected label %1 in Working Segments.")
                              .arg(label));
        }
        result.dataChanged = result.dataChanged ||
                             resolution.status == WorkingSegmentResolution::Status::Inserted;
        insertedLabelCount +=
            resolution.status == WorkingSegmentResolution::Status::Inserted ? 1U : 0U;
        workingLabelBySelectedLabel[label] = resolution.workingLabel;
    }
    const double resolutionMs =
        static_cast<double>(resolutionTimer.nsecsElapsed()) / 1000000.0;

    QElapsedTimer postInsertionValidationTimer;
    postInsertionValidationTimer.start();
    std::vector<SegmentIdType> repairedLabels;
    if (insertedLabelCount > 0) {
        // Refinement can split exact neighboring WorkingNodes back into their
        // InitialNodes. Resolve only those stale mappings after all planned
        // insertions have finished.
        for (const SegmentIdType label : consumedLabels) {
            const auto &seed = statsByLabel.at(label).seed;
            const SegmentIdType workingLabel = graphBase->pWorkingSegmentsImage->GetPixel(seed);
            const auto workingNode = workingNodes.find(workingLabel);
            ComponentMatch match = ComponentMatch::Different;
            if (workingLabel != backgroundId) {
                if (workingNode == workingNodes.end() || workingNode->second == nullptr) {
                    return finish(MergeStatus::Failed,
                                  QStringLiteral("Working label %1 is missing from the WorkingGraph.")
                                      .arg(workingLabel));
                }
                match = selectedComponentMatchesWorkingNode(
                    graphBase->pSelectedSegmentation,
                    label,
                    seed,
                    *workingNode->second,
                    statsByLabel.at(label).voxelCount);
                if (match == ComponentMatch::Invalid) {
                    return finish(MergeStatus::Failed,
                                  QStringLiteral("Working node %1 contains invalid voxel metadata.")
                                      .arg(workingLabel));
                }
            }
            if (match == ComponentMatch::Exact) {
                workingLabelBySelectedLabel[label] = workingLabel;
                continue;
            }

            if (!seedFitsWorkingCoordinates(seed)) {
                return finish(
                    MergeStatus::Failed,
                    QStringLiteral("Label %1 lies outside the coordinate range supported by Working Segments.")
                        .arg(label));
            }
            const SegmentIdType previousWorkingLabel = workingLabelBySelectedLabel.at(label);
            result.dataChanged = true;
            const auto resolution = ensureSelectedSegmentationComponentInWorkingGraph(
                static_cast<int>(seed[0]), static_cast<int>(seed[1]), static_cast<int>(seed[2]));
            if (resolution.status != WorkingSegmentResolution::Status::ReusedExisting &&
                resolution.status != WorkingSegmentResolution::Status::Inserted) {
                return finish(MergeStatus::Failed,
                              QStringLiteral("Could not restore selected label %1 after insertion.")
                                  .arg(label));
            }
            insertedLabelCount +=
                resolution.status == WorkingSegmentResolution::Status::Inserted ? 1U : 0U;
            repairedLabels.push_back(label);
            workingLabelBySelectedLabel[label] = resolution.workingLabel;
            logGraphDebugIf(
                verbose,
                operationName,
                QStringLiteral("operation=merge_with_neighbor phase=post_insertion_repair "
                               "selected=%1 previous_working=%2 current_working=%3 resolved_working=%4 action=%5")
                    .arg(label)
                    .arg(previousWorkingLabel)
                    .arg(workingLabel)
                    .arg(resolution.workingLabel)
                    .arg(resolution.status == WorkingSegmentResolution::Status::Inserted
                             ? QStringLiteral("inserted")
                             : QStringLiteral("reused")));
        }

    }

    // Resolve every mapping once more after all insertions. From this point on,
    // each consumed label must map to one distinct, exact WorkingNode.
    std::set<SegmentIdType> resolvedWorkingLabels;
    for (const SegmentIdType label : consumedLabels) {
        const auto &seed = statsByLabel.at(label).seed;
        const SegmentIdType workingLabel = graphBase->pWorkingSegmentsImage->GetPixel(seed);
        const auto workingNode = workingNodes.find(workingLabel);
        if (workingLabel == backgroundId || workingNode == workingNodes.end()
            || workingNode->second == nullptr
            || selectedComponentMatchesWorkingNode(
                   graphBase->pSelectedSegmentation,
                   label,
                   seed,
                   *workingNode->second,
                   statsByLabel.at(label).voxelCount) != ComponentMatch::Exact) {
            return finish(
                MergeStatus::Failed,
                QStringLiteral("Selected label %1 does not exactly match a WorkingNode after resolution.")
                    .arg(label));
        }
        if (!resolvedWorkingLabels.insert(workingLabel).second) {
            return finish(
                MergeStatus::Failed,
                QStringLiteral("Multiple selected labels resolve to WorkingNode %1.")
                    .arg(workingLabel));
        }
        workingLabelBySelectedLabel[label] = workingLabel;
    }
    const double postInsertionValidationMs =
        static_cast<double>(postInsertionValidationTimer.nsecsElapsed()) / 1000000.0;
    logGraphDebugIf(
        verbose,
        operationName,
        QStringLiteral("operation=merge_with_neighbor phase=working_resolution reused=%1 "
                       "resolved=%2 inserted=%3 repaired=%4 resolution_ms=%5 post_validation_ms=%6 "
                       "repaired_labels=[%7]")
            .arg(exactWorkingLabelBySelectedLabel.size())
            .arg(labelsNeedingInsertion.size())
            .arg(insertedLabelCount)
            .arg(repairedLabels.size())
            .arg(resolutionMs, 0, 'f', 3)
            .arg(postInsertionValidationMs, 0, 'f', 3)
            .arg(joinIds(repairedLabels)));

    // Translate selected-segmentation adjacency into registered graph edges.
    std::vector<std::pair<SegmentIdType, SegmentIdType>> plannedWorkingPairs;
    plannedWorkingPairs.reserve(plannedLabelPairs.size());
    std::set<EdgeNumIdType> edgeIdsToMerge;
    std::set<SegmentIdType> involvedWorkingLabels;
    for (const auto &[sourceLabel, targetLabel] : plannedLabelPairs) {
        const SegmentIdType sourceWorkingLabel = workingLabelBySelectedLabel.at(sourceLabel);
        const SegmentIdType targetWorkingLabel = workingLabelBySelectedLabel.at(targetLabel);
        if (sourceWorkingLabel == targetWorkingLabel) {
            continue;
        }

        const auto sourceWorkingNode = workingNodes.find(sourceWorkingLabel);
        if (sourceWorkingNode == workingNodes.end() || sourceWorkingNode->second == nullptr) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("Working node %1 is missing before merge.").arg(sourceWorkingLabel));
        }
        const auto workingEdge = sourceWorkingNode->second->neighborLabelToWorkingEdge.find(targetWorkingLabel);
        if (workingEdge == sourceWorkingNode->second->neighborLabelToWorkingEdge.end() ||
            workingEdge->second == nullptr || workingEdge->second->constituentTwoSidedInitialEdges.empty()) {
            return finish(
                MergeStatus::Failed,
                QStringLiteral("No WorkingEdge exists between selected labels %1 and %2.")
                    .arg(sourceLabel)
                    .arg(targetLabel));
        }

        bool foundRegisteredEdge = false;
        EdgeNumIdType registeredInitialEdgeId = 0;
        for (const auto &initialEdge : workingEdge->second->constituentTwoSidedInitialEdges) {
            if (initialEdge == nullptr) {
                continue;
            }
            const auto lookup = initialEdgeIdLookup.find(initialEdge->numId);
            if (lookup == initialEdgeIdLookup.end()) {
                continue;
            }
            const auto registeredEdge = initialLabelPairToTwoSidedInitialEdge.find(lookup->second);
            if (registeredEdge == initialLabelPairToTwoSidedInitialEdge.end() || registeredEdge->second == nullptr) {
                continue;
            }
            edgeIdsToMerge.insert(initialEdge->numId);
            registeredInitialEdgeId = initialEdge->numId;
            foundRegisteredEdge = true;
            break;
        }
        if (!foundRegisteredEdge) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("The WorkingEdge between labels %1 and %2 has no registered TwoSidedInitialEdge.")
                              .arg(sourceLabel)
                              .arg(targetLabel));
        }

        plannedWorkingPairs.emplace_back(sourceWorkingLabel, targetWorkingLabel);
        involvedWorkingLabels.insert(sourceWorkingLabel);
        involvedWorkingLabels.insert(targetWorkingLabel);
        logGraphDebugIf(
            verbose,
            operationName,
            QStringLiteral("operation=merge_with_neighbor phase=edge_resolution selected_pair=%1,%2 "
                           "working_pair=%3,%4 initial_edge=%5")
                .arg(sourceLabel)
                .arg(targetLabel)
                .arg(sourceWorkingLabel)
                .arg(targetWorkingLabel)
                .arg(registeredInitialEdgeId));
    }

    if (plannedWorkingPairs.empty() || edgeIdsToMerge.empty()) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("The selected labels resolved to no distinct WorkingNodes to merge."));
    }

    // Count connected merge groups before consuming graph or segmentation labels.
    std::unordered_map<SegmentIdType, SegmentIdType> parentByWorkingLabel;
    parentByWorkingLabel.reserve(involvedWorkingLabels.size());
    for (const SegmentIdType label : involvedWorkingLabels) {
        parentByWorkingLabel[label] = label;
    }
    const auto findRoot = [&parentByWorkingLabel](SegmentIdType label) {
        SegmentIdType root = label;
        while (parentByWorkingLabel.at(root) != root) {
            root = parentByWorkingLabel.at(root);
        }
        while (parentByWorkingLabel.at(label) != label) {
            const SegmentIdType next = parentByWorkingLabel.at(label);
            parentByWorkingLabel[label] = root;
            label = next;
        }
        return root;
    };
    for (const auto &[firstLabel, secondLabel] : plannedWorkingPairs) {
        const SegmentIdType firstRoot = findRoot(firstLabel);
        const SegmentIdType secondRoot = findRoot(secondLabel);
        if (firstRoot != secondRoot) {
            parentByWorkingLabel[std::max(firstRoot, secondRoot)] = std::min(firstRoot, secondRoot);
        }
    }
    std::set<SegmentIdType> plannedRoots;
    for (const SegmentIdType label : involvedWorkingLabels) {
        plannedRoots.insert(findRoot(label));
    }
    const std::size_t effectiveWorkingMergeCount = involvedWorkingLabels.size() - plannedRoots.size();
    const std::uintmax_t maximumLabel = std::numeric_limits<SegmentIdType>::max();
    if (effectiveWorkingMergeCount >
        maximumLabel - static_cast<std::uintmax_t>(nextFreeId)) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("No free Working Segment labels remain for this merge."));
    }

    try {
        volume_io::firstLabelForReservation(*graphBase->selectedSegmentationContext, plannedRoots.size());
    } catch (const std::exception &) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("No free Selected Segmentation labels remain for the merge results."));
    }

    std::size_t expectedVoxelCount = 0;
    for (const SegmentIdType label : consumedLabels) {
        const std::size_t labelVoxelCount = statsByLabel.at(label).voxelCount;
        if (labelVoxelCount > std::numeric_limits<std::size_t>::max() - expectedVoxelCount) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("The consumed segmentation labels contain too many voxels."));
        }
        expectedVoxelCount += labelVoxelCount;
    }
    logGraphDebugIf(
        verbose,
        operationName,
        QStringLiteral("operation=merge_with_neighbor phase=apply_preflight consumed_voxels=%1 "
                       "working_nodes=%2 merge_groups=%3")
            .arg(expectedVoxelCount)
            .arg(resolvedWorkingLabels.size())
            .arg(plannedRoots.size()));

    result.dataChanged = true;
    logGraphDebugIf(
        verbose,
        operationName,
        QStringLiteral("operation=merge_with_neighbor phase=merge_edges working_pairs=[%1] initial_edges=[%2]")
            .arg([&plannedWorkingPairs]() {
                QStringList pairs;
                for (const auto &[first, second] : plannedWorkingPairs) {
                    pairs << QStringLiteral("%1,%2").arg(first).arg(second);
                }
                return pairs.join(QStringLiteral("; "));
            }())
            .arg(joinIds(edgeIdsToMerge)));
    const std::set<SegmentIdType> finalWorkingLabels = mergeEdges(edgeIdsToMerge);

    if (finalWorkingLabels.size() != plannedRoots.size()) {
        throw std::logic_error(
            "WorkingGraph merge groups differ from the validated neighbor-merge plan.");
    }

    std::unordered_map<SegmentIdType, SegmentIdType> finalWorkingLabelByConsumedLabel;
    finalWorkingLabelByConsumedLabel.reserve(consumedLabels.size());
    for (const SegmentIdType label : consumedLabels) {
        const SegmentIdType finalWorkingLabel =
            graphBase->pWorkingSegmentsImage->GetPixel(statsByLabel.at(label).seed);
        if (finalWorkingLabels.count(finalWorkingLabel) == 0
            || workingNodes.count(finalWorkingLabel) == 0) {
            throw std::logic_error(
                "A validated neighbor-merge result is missing from Working Segments.");
        }
        finalWorkingLabelByConsumedLabel[label] = finalWorkingLabel;
    }

    std::vector<SegmentIdType> transferWorkingLabels(
        finalWorkingLabels.begin(), finalWorkingLabels.end());
    const auto assignedLabels = transferWorkingNodesToSegmentation(transferWorkingLabels);
    if (assignedLabels.size() != transferWorkingLabels.size()) {
        throw std::logic_error(
            "Validated neighbor-merge results could not be transferred to the Selected Segmentation.");
    }

    std::map<SegmentIdType, SegmentIdType> assignedLabelByWorkingLabel;
    for (std::size_t index = 0; index < transferWorkingLabels.size(); ++index) {
        assignedLabelByWorkingLabel.emplace(transferWorkingLabels[index], assignedLabels[index]);
    }
    for (const auto &[consumedLabel, finalWorkingLabel] : finalWorkingLabelByConsumedLabel) {
        const SegmentIdType newLabel = assignedLabelByWorkingLabel.at(finalWorkingLabel);
        const std::size_t consumedVoxelCount = statsByLabel.at(consumedLabel).voxelCount;
        result.newLabelByConsumedLabel.emplace(consumedLabel, newLabel);
        result.voxelCountByConsumedLabel.emplace(consumedLabel, consumedVoxelCount);

        auto &newLabelVoxelCount = result.voxelCountByNewLabel[newLabel];
        if (consumedVoxelCount >
            std::numeric_limits<std::size_t>::max() - newLabelVoxelCount) {
            throw std::logic_error(
                "A merged Selected Segmentation label contains too many voxels.");
        }
        newLabelVoxelCount += consumedVoxelCount;
    }

    graphBase->pSelectedSegmentation->Modified();
    result.mergedGroupCount = transferWorkingLabels.size();
    logGraphDebugIf(
        verbose,
        operationName,
        QStringLiteral("operation=merge_with_neighbor phase=transfer working=[%1] segmentation=[%2]")
            .arg(joinIds(transferWorkingLabels))
            .arg(joinIds(assignedLabels)));
    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("Merged selected labels=%1 consumed labels=[%2] result working labels=[%3]")
            .arg(result.mergeableSelectedLabelCount)
            .arg(joinIds(consumedLabels))
            .arg(joinIds(transferWorkingLabels)));
    return finish(MergeStatus::Merged,
                  QStringLiteral("Merged %1 selected segment(s) into %2 result segment(s).")
                      .arg(result.mergeableSelectedLabelCount)
                      .arg(result.mergedGroupCount));
}


Graph::WorkingSegmentResolution Graph::inspectSelectedSegmentationComponentInWorkingGraph(int x, int y, int z) {
    WorkingSegmentResolution resolution;
    if (graphBase->pSelectedSegmentation == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No selected segmentation is loaded"));
        return resolution;
    }
    if (graphBase->pWorkingSegmentsImage == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No working segments image is loaded"));
        return resolution;
    }

    const auto selectedRegion = graphBase->pSelectedSegmentation->GetLargestPossibleRegion();
    const auto workingRegion = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion();
    if (!regionsMatch(selectedRegion, workingRegion)) {
        logGraph(LogLevel::Error,
                 __func__,
                 QStringLiteral("Selected segmentation and working segments use incompatible image regions"));
        return resolution;
    }

    SegmentsImageType::IndexType seed{{x, y, z}};
    if (!selectedRegion.IsInside(seed)) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("Clicked point lies outside the segmentation image"));
        return resolution;
    }

    const SegmentIdType selectedLabel = graphBase->pSelectedSegmentation->GetPixel(seed);
    if (selectedLabel == selectedBackground()) {
        resolution.status = WorkingSegmentResolution::Status::NoForeground;
        return resolution;
    }

    const SegmentIdType workingLabel = graphBase->pWorkingSegmentsImage->GetPixel(seed);
    if (workingLabel != backgroundId) {
        const auto workingNodeIt = workingNodes.find(workingLabel);
        if (workingNodeIt == workingNodes.end() || workingNodeIt->second == nullptr) {
            logGraph(LogLevel::Error,
                     __func__,
                     QStringLiteral("Working label %1 is missing from workingNodes").arg(workingLabel));
            return resolution;
        }

        const ComponentMatch componentMatch =
            selectedComponentMatchesWorkingNode(graphBase->pSelectedSegmentation,
                                                selectedLabel,
                                                seed,
                                                *workingNodeIt->second);
        if (componentMatch == ComponentMatch::Invalid) {
            logGraph(LogLevel::Error,
                     __func__,
                     QStringLiteral("Working label %1 has invalid voxel metadata").arg(workingLabel));
            return resolution;
        }
        if (componentMatch == ComponentMatch::Exact) {
            resolution.status = WorkingSegmentResolution::Status::ReusedExisting;
            resolution.workingLabel = workingLabel;
            logGraphDebugIf(
                verbose,
                __func__,
                QStringLiteral("Selected label %1 reuses exact working label %2")
                    .arg(selectedLabel)
                    .arg(workingLabel));
            return resolution;
        }
    }

    resolution.status = WorkingSegmentResolution::Status::NeedsInsertion;
    return resolution;
}

Graph::WorkingSegmentResolution Graph::ensureSelectedSegmentationComponentInWorkingGraph(int x, int y, int z) {
    WorkingSegmentResolution resolution =
        inspectSelectedSegmentationComponentInWorkingGraph(x, y, z);
    if (resolution.status != WorkingSegmentResolution::Status::NeedsInsertion) {
        return resolution;
    }

    const SegmentIdType selectedLabel = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
    const auto insertedLabel = transferSegmentationSegmentToInitialSegment(x, y, z);
    if (!insertedLabel.has_value()) {
        return resolution;
    }
    resolution.status = WorkingSegmentResolution::Status::Inserted;
    resolution.workingLabel = *insertedLabel;
    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("Selected label %1 inserted as working label %2")
            .arg(selectedLabel)
            .arg(*insertedLabel));
    return resolution;
}

Graph::SegmentsImageType::Pointer Graph::initialLabelsInRegion(const SegmentsImageType::RegionType &region) {
    auto labels = SegmentsImageType::New();
    labels->SetRegions(region);
    labels->Allocate();
    std::set<SegmentIdType> merged;
    graphBase->pWorkingSegmentsImage->forEachRun(region, [&](const auto &start, std::size_t length, SegmentIdType owner) {
        auto value = backgroundId;
        if (!isIgnoredId(owner)) {
            const auto &group = workingNodes.at(owner);
            if (!group || group->subInitialNodes.empty()) throw std::logic_error("Missing working node during refinement.");
            if (group->subInitialNodes.size() == 1) {
                const auto &node = initialNodes.at(group->subInitialNodes.begin()->first);
                if (!node) throw std::logic_error("Missing initial node during refinement.");
                value = node->getLabel();
            } else {
                merged.insert(owner);
            }
        }
        std::fill_n(labels->GetBufferPointer() + labels->ComputeOffset(start), length, value);
    });
    for (const auto owner : merged) {
        for (const auto &[id, member] : workingNodes.at(owner)->subInitialNodes) {
            const auto &node = initialNodes.at(id);
            if (!node) throw std::logic_error("Missing initial node during refinement.");
            for (const auto &run : node->voxels.getRuns()) {
                SegmentsImageType::RegionType row({run.start.x, run.start.y, run.start.z}, {run.length, 1, 1});
                if (row.Crop(region))
                    std::fill_n(labels->GetBufferPointer() + labels->ComputeOffset(row.GetIndex()), row.GetSize()[0], id);
            }
        }
    }
    return labels;
}

namespace {
void appendBoundaryOutside(const VoxelList &boundary, const Graph::SegmentsImageType::RegionType &region,
                           std::vector<VoxelList::Run> &result) {
    for (const auto &run : boundary.getRuns()) {
        Graph::SegmentsImageType::RegionType row({run.start.x, run.start.y, run.start.z}, {run.length, 1, 1});
        if (!row.Crop(region)) {
            result.push_back(run);
            continue;
        }
        const auto first = row.GetIndex()[0];
        const auto end = first + static_cast<itk::IndexValueType>(row.GetSize()[0]);
        if (first > run.start.x) {
            result.push_back({run.start, static_cast<std::uint32_t>(first - run.start.x)});
        }
        const auto runEnd = std::int64_t(run.start.x) + run.length;
        if (end < runEnd) {
            result.push_back({Voxel(end, run.start.y, run.start.z), static_cast<std::uint32_t>(runEnd - end)});
        }
    }
}
}

std::optional<Graph::SegmentIdType> Graph::refineFromImageAtPosition(
        const dataType::SegmentsStorage::Pointer &sourceImage,
        SegmentIdType sourceBackground,
        const SegmentsImageType::IndexType &seed,
        const std::optional<SegmentsImageType::RegionType> &permittedSeedRegion) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Refining working graph from image component"));
    if (graphBase == nullptr || sourceImage == nullptr ||
        graphBase->pWorkingSegmentsImage == nullptr || pIgnoredSegmentLabels == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("Refinement and working images must be initialized"));
        return std::nullopt;
    }


    const auto sourceRegion = sourceImage->GetLargestPossibleRegion();
    const auto workingRegion = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion();
    if (!regionsMatch(sourceRegion, workingRegion) || !sourceRegion.IsInside(seed)) {
        logGraph(
            LogLevel::Error,
            __func__,
            QStringLiteral("Refinement and working segments use incompatible image regions or coordinates"));
        return std::nullopt;
    }
    if (permittedSeedRegion.has_value() && !permittedSeedRegion->IsInside(seed)) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("Clicked point lies outside the selected refinement ROI"));
        return std::nullopt;
    }

    const SegmentIdType sourceLabel = sourceImage->GetPixel(seed);
    if (sourceLabel == sourceBackground) {
        logGraph(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Refinement label matches background label %1; insertion skipped")
                .arg(sourceBackground));
        return std::nullopt;
    }
    auto nextLabel = nextFreeId;
    const auto labelToInsert = segmentManager.takeNextSegmentId(nextLabel);

    std::unique_ptr<InitialNode> pendingInitialNode;
    {
        ScopedGraphTimer componentTimer(verbose, __func__, QStringLiteral("Collecting refinement component"));
        pendingInitialNode = std::make_unique<InitialNode>(
            graphBase,
            sourceImage,
            labelToInsert,
            static_cast<int>(seed[0]),
            static_cast<int>(seed[1]),
            static_cast<int>(seed[2]));
        pendingInitialNode->setSegmentPointer(graphBase->pWorkingSegmentsImage);
        pendingInitialNode->roi.updateBoundingRoi(pendingInitialNode->voxels);
    }
    // Only contacts within one face of an inserted voxel can change. The extra
    // halo supplies the other endpoint of every contact in that region.
    const auto editRegion = getDilatedRegionFromRoi(pendingInitialNode->roi, workingRegion, 1);
    const auto readRegion = getDilatedRegionFromRoi(pendingInitialNode->roi, workingRegion, 2);
    auto labels = initialLabelsInRegion(readRegion);
    std::set<SegmentIdType> overwritten, ownersToSplit;
    for (const auto &v : pendingInitialNode->voxels) {
        const SegmentsImageType::IndexType index{v.x, v.y, v.z};
        const auto id = labels->GetPixel(index);
        if (id != backgroundId) overwritten.insert(id);
        labels->SetPixel(index, labelToInsert);
    }

    BoundaryRuns contacts;
    std::map<SegmentIdType, VoxelList> survivors;
    for (const auto id : overwritten) {
        const auto &node = initialNodes.at(id);
        const auto owner = node->getCurrentWorkingNodeLabel();
        const auto &members = workingNodes.at(owner)->subInitialNodes;
        if (members.size() > 1 || owner != id) ownersToSplit.insert(owner);

        auto &remaining = survivors[id];
        const auto keepRun = [&](const SegmentsImageType::IndexType &start, std::size_t length) {
            SegmentsImageType::RegionType row(start, {length, 1, 1});
            if (!row.Crop(editRegion)) {
                remaining.appendRun(Voxel(start[0], start[1], start[2]), length);
                return;
            }
            auto index = start;
            for (std::size_t x = 0; x < length; ++x, ++index[0]) {
                if (!editRegion.IsInside(index) || labels->GetPixel(index) != labelToInsert)
                    remaining.emplace_back(index[0], index[1], index[2]);
            }
        };
        if (members.size() == 1) {
            const auto bounds = getDilatedRegionFromRoi(node->roi, workingRegion);
            graphBase->pWorkingSegmentsImage->forEachRun(bounds, [&](const auto &start, std::size_t length, auto value) {
                if (value == owner) keepRun(start, length);
            });
        } else {
            for (const auto &run : node->voxels.getRuns()) keepRun({run.start.x, run.start.y, run.start.z}, run.length);
        }

        for (const auto &[neighbor, side] : node->neighborLabelToOneSidedInitialEdge) {
            if (overwritten.count(neighbor) && neighbor < id) continue;
            const auto &other = initialNodes.at(neighbor);
            if (!other || !side) throw std::logic_error("Missing initial contact during refinement.");
            const auto &opposite = other->neighborLabelToOneSidedInitialEdge.at(id);
            if (!opposite) throw std::logic_error("Missing opposite contact during refinement.");
            auto &sides = contacts[std::minmax(id, neighbor)];
            appendBoundaryOutside(side->voxels, editRegion, sides[id > neighbor]);
            appendBoundaryOutside(opposite->voxels, editRegion, sides[id < neighbor]);
        }
    }
    collectInitialContacts(labels, editRegion, backgroundId,
                           [&](auto id) { return id == labelToInsert || overwritten.count(id); }, contacts);
    auto edges = prepareInitialEdges(contacts);
    pendingInitialNode->calculateNodeFeatures();

    nextFreeId = nextLabel;
    for (const auto owner : ownersToSplit) segmentManager.splitWorkingNodeIntoInitialNodes(owner);
    segment_puzzler::setVoxelLists(*graphBase->pWorkingSegmentsImage, {&pendingInitialNode->voxels}, labelToInsert);
    for (auto &[id, remaining] : survivors) {
        if (remaining.empty()) {
            segmentManager.removeWorkingNode(workingNodes.at(id).get());
            segmentManager.removeInitialNode(id);
        } else {
            const auto &node = initialNodes.at(id);
            segmentManager.removeEdgePropertiesOnInitialNode(node.get());
            node->voxels = std::move(remaining);
            node->roi.updateBoundingRoi(node->voxels);
            node->calculateNodeFeatures();
            workingNodes.at(id)->roi = node->roi;
        }
    }
    segmentManager.addInitialNode(pendingInitialNode.release());
    segmentManager.addWorkingNode(new WorkingNode(initialNodes.at(labelToInsert).get(), labelToInsert, initialNodes));
    for (auto &edge : edges) edge.install(*this, segmentManager);
    for (const auto id : overwritten) {
        if (workingNodes.count(id)) segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(id).get());
    }
    segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(labelToInsert).get());
    return labelToInsert;
}

std::optional<Graph::SegmentIdType> Graph::transferSegmentationSegmentToInitialSegment(
        int x,
        int y,
        int z) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring segmentation component"));
    if (graphBase == nullptr || graphBase->pSelectedSegmentation == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No selected segmentation is loaded"));
        return std::nullopt;
    }

    const SegmentsImageType::IndexType seed{{x, y, z}};
    try {
        return refineFromImageAtPosition(graphBase->pSelectedSegmentation, selectedBackground(), seed);
    } catch (const std::exception &exception) {
        logGraph(
            LogLevel::Error,
            __func__,
            QStringLiteral("Failed to insert segmentation component: %1")
                .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        logGraph(LogLevel::Error, __func__, QStringLiteral("Failed to insert segmentation component"));
    }
    return std::nullopt;
}

void Graph::refineWithSelectedRefinementAtPosition(int x, int y, int z) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Refining working graph from selected refinement"));
    if (graphBase == nullptr || graphBase->pSelectedRefinement == nullptr ||
        graphBase->pSelectedRefinementSignal == nullptr) {
        logGraph(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Selected refinement is not initialized; load a refinement before refining"));
        return;
    }

    std::optional<SegmentsImageType::RegionType> permittedSeedRegion;
    const auto *signal = graphBase->pSelectedRefinementSignal;
    if (signal->ROI_set) {
        const std::array<long long, 3> minimum{{signal->ROI_fx, signal->ROI_fy, signal->ROI_fz}};
        const std::array<long long, 3> maximum{{signal->ROI_tx, signal->ROI_ty, signal->ROI_tz}};
        SegmentsImageType::IndexType start;
        SegmentsImageType::SizeType size;
        for (unsigned int axis = 0; axis < 3; ++axis) {
            if (minimum[axis] > maximum[axis]) {
                logGraph(LogLevel::Error, __func__, QStringLiteral("Selected refinement ROI is invalid"));
                return;
            }
            start[axis] = static_cast<SegmentsImageType::IndexType::IndexValueType>(minimum[axis]);
            size[axis] = static_cast<SegmentsImageType::SizeType::SizeValueType>(
                maximum[axis] - minimum[axis] + 1);
        }
        permittedSeedRegion = SegmentsImageType::RegionType(start, size);
    }

    try {
        static_cast<void>(refineFromImageAtPosition(
            graphBase->pSelectedRefinement,
            signal->volumeContext ? signal->volumeContext->background.value_or(0) : 0,
            SegmentsImageType::IndexType{{x, y, z}},
            permittedSeedRegion));
    } catch (const std::exception &exception) {
        logGraph(
            LogLevel::Error,
            __func__,
            QStringLiteral("Failed to refine selected component: %1")
                .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        logGraph(LogLevel::Error, __func__, QStringLiteral("Failed to refine selected component"));
    }
}

std::optional<Graph::SegmentIdType> Graph::transferWorkingNodeToSegmentation(int x, int y, int z) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring working node to segmentation"));
    if (graphBase == nullptr || graphBase->pWorkingSegmentsImage == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No working segments image is loaded"));
        return std::nullopt;
    }

    SegmentsImageType::IndexType index{{x, y, z}};
    if (!graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion().IsInside(index)) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("Clicked point lies outside the working segments image"));
        return std::nullopt;
    }

    const SegmentIdType labelOfTargetedWorkingNode = graphBase->pWorkingSegmentsImage->GetPixel(index);
    return transferWorkingNodeToSegmentation(labelOfTargetedWorkingNode);
}

std::optional<Graph::SegmentIdType>
Graph::transferWorkingNodeToSegmentation(SegmentIdType labelOfNodeToTransfer) {
    const auto assignedLabels = transferWorkingNodesToSegmentation({labelOfNodeToTransfer});
    if (assignedLabels.size() != 1) {
        return std::nullopt;
    }
    return assignedLabels.front();
}

std::vector<Graph::SegmentIdType>
Graph::transferWorkingNodesToSegmentation(
        const std::vector<SegmentIdType> &workingLabelsToTransfer,
        bool preserveExistingSegmentation) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring working nodes to segmentation"));
    std::vector<SegmentIdType> assignedSegmentationLabels;
    if (workingLabelsToTransfer.empty()) {
        return assignedSegmentationLabels;
    }
    if (graphBase == nullptr || graphBase->pSelectedSegmentation == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("No selected segmentation is loaded"));
        return assignedSegmentationLabels;
    }
    if (graphBase->pWorkingSegmentsImage == nullptr ||
        !regionsMatch(graphBase->pSelectedSegmentation->GetLargestPossibleRegion(),
                      graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion())) {
        logGraph(LogLevel::Warning,
                 __func__,
                 QStringLiteral("Selected segmentation and working segments use incompatible image regions"));
        return assignedSegmentationLabels;
    }

    std::vector<std::shared_ptr<WorkingNode>> nodesToTransfer;
    nodesToTransfer.reserve(workingLabelsToTransfer.size());
    std::unordered_set<SegmentIdType> seenWorkingLabels;
    seenWorkingLabels.reserve(workingLabelsToTransfer.size());
    const auto selectedRegion = graphBase->pSelectedSegmentation->GetBufferedRegion();
    dataType::SegmentsStorage::LabelStatistics transferBounds;

    for (const SegmentIdType workingLabel : workingLabelsToTransfer) {
        if (!seenWorkingLabels.insert(workingLabel).second) {
            logGraph(LogLevel::Warning,
                     __func__,
                     QStringLiteral("Working label %1 occurs more than once in the transfer batch")
                         .arg(workingLabel));
            return {};
        }
        if (workingLabel == backgroundId ||
            (pIgnoredSegmentLabels != nullptr && isIgnoredId(workingLabel))) {
            logGraph(LogLevel::Warning,
                     __func__,
                     QStringLiteral("Working label %1 is ignored and cannot be transferred").arg(workingLabel));
            return {};
        }

        const auto workingNodeIt = workingNodes.find(workingLabel);
        if (workingNodeIt == workingNodes.end() || workingNodeIt->second == nullptr) {
            logGraph(LogLevel::Warning,
                     __func__,
                     QStringLiteral("Working label %1 is missing from workingNodes").arg(workingLabel));
            return {};
        }
        const auto lists = workingNodeIt->second->getVoxelLists();
        dataType::SegmentsStorage::LabelStatistics nodeBounds;
        for (const auto *list : lists) {
            if (!list) return {};
            for (const auto &run : list->getRuns()) {
                const SegmentsImageType::RegionType region({run.start.x, run.start.y, run.start.z}, {run.length, 1, 1});
                if (!selectedRegion.IsInside(region)) return {};
                nodeBounds.include(region, run.length);
            }
        }
        bool hasVoxelsToTransfer = !preserveExistingSegmentation;
        if (preserveExistingSegmentation) {
            for (const auto *list : lists) {
                if (hasVoxelsToTransfer) break;
                for (const auto &run : list->getRuns()) {
                    if (hasVoxelsToTransfer) break;
                    const SegmentsImageType::RegionType region({run.start.x, run.start.y, run.start.z}, {run.length, 1, 1});
                    graphBase->pSelectedSegmentation->forEachRun(region, [&](const auto &, std::size_t, SegmentIdType value) {
                        hasVoxelsToTransfer |= value == selectedBackground();
                    });
                }
            }
        }
        if (hasVoxelsToTransfer) {
            nodesToTransfer.push_back(workingNodeIt->second);
            transferBounds.include(nodeBounds.bounds, nodeBounds.voxelCount);
        }
    }

    if (nodesToTransfer.empty()) {
        return assignedSegmentationLabels;
    }

    assignedSegmentationLabels.reserve(nodesToTransfer.size());
    SegmentIdType firstLabel;
    try {
        firstLabel = reserveSelectedLabelIds(nodesToTransfer.size());
    } catch (const std::overflow_error &error) {
        logGraph(LogLevel::Error, __func__, QString::fromUtf8(error.what()));
        return {};
    }
    for (std::size_t nodeIndex = 0; nodeIndex < nodesToTransfer.size(); ++nodeIndex) {
        assignedSegmentationLabels.push_back(
            static_cast<SegmentIdType>(std::uint64_t(firstLabel) + nodeIndex));
    }

    std::unordered_map<SegmentIdType, SegmentIdType> targetLabels;
    for (std::size_t i = 0; i < nodesToTransfer.size(); ++i)
        targetLabels.emplace(nodesToTransfer[i]->getLabel(), assignedSegmentationLabels[i]);
    if (transferBounds.voxelCount) {
        auto region = transferBounds.bounds;
        const auto firstZ = region.GetIndex()[2], endZ = region.GetUpperIndex()[2] + 1;
        std::vector<dataType::SegmentsStorage::VoxelRange> ranges;
        for (auto z = firstZ; z < endZ; z += 16) {
            region.SetIndex(2, z);
            region.SetSize(2, std::min<itk::IndexValueType>(16, endZ - z));
            const auto target = preserveExistingSegmentation ? graphBase->pSelectedSegmentation->denseRegion(region) : nullptr;
            graphBase->pWorkingSegmentsImage->forEachRun(region, [&](const auto &index, std::size_t length, SegmentIdType label) {
                const auto found = targetLabels.find(label);
                if (found == targetLabels.end()) return;
                if (!target) { ranges.push_back({index, length, found->second}); return; }
                const auto *pixels = target->GetBufferPointer() + target->ComputeOffset(index);
                for (std::size_t begin = 0; begin < length;) {
                    auto end = begin + 1;
                    while (end < length && pixels[end] == pixels[begin]) ++end;
                    if (pixels[begin] == selectedBackground()) {
                        auto start = index; start[0] += begin;
                        ranges.push_back({start, end - begin, found->second});
                    }
                    begin = end;
                }
            });
            graphBase->pSelectedSegmentation->fillVoxelRanges(ranges);
            ranges.clear();
        }
    }

    graphBase->pSelectedSegmentation->Modified();
    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("Requested working labels=[%1], assigned segmentation labels=[%2]")
            .arg(joinIds(workingLabelsToTransfer))
            .arg(joinIds(assignedSegmentationLabels)));
    return assignedSegmentationLabels;
}




// === print functions ===

namespace {
template<class Object>
void printGraphEntry(const std::shared_ptr<Object> &entry, int indentation, std::ostream &out) {
    if (entry) entry->print(indentation, out);
    else out << "\t<missing>\n";
}

} // namespace

void Graph::printInitialNodes(std::ostream &outStream) {
    outStream << "=== initialNodes ===\n";
    int nodeIndentationLevel = 1;
    for (auto &node : initialNodes) {
        outStream << "key: " << node.first << "\n";
        printGraphEntry(node.second, nodeIndentationLevel, outStream);
    }
}

void Graph::printInitialOneSidedEdges(std::ostream &outStream) {
    outStream << "=== initialOneSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (const auto &[sourceLabel, initialNode] : initialNodes) {
        if (!initialNode) {
            outStream << "key: " << sourceLabel << "\t<missing initial node>\n";
            continue;
        }
        for (const auto &[neighborLabel, edge] : initialNode->neighborLabelToOneSidedInitialEdge) {
            outStream << "key: " << sourceLabel << "," << neighborLabel << "\n";
            printGraphEntry(edge, nodeIndentationLevel, outStream);
        }
    }
}

void Graph::printInitialTwoSidedEdges(std::ostream &outStream) {
    outStream << "=== initialTwoSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : initialLabelPairToTwoSidedInitialEdge) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        printGraphEntry(edge.second, nodeIndentationLevel, outStream);
    }
}

void Graph::printWorkingNodes(std::ostream &outStream) {
    outStream << "=== workingNodes ===\n";
    int nodeIndentationLevel = 1;
    for (auto &node : workingNodes) {
        outStream << "key: " << node.first << "\n";
        printGraphEntry(node.second, nodeIndentationLevel, outStream);
    }
}

void Graph::printEdgeIdLookUp(std::ostream &outStream) {
    outStream << "=== edgeIdLookup ===\n";
    for (auto &id : initialEdgeIdLookup) {
        outStream << "numId: " << id.first << "\n";
        outStream << "pairId: " << id.second.first << "," << id.second.second << "\n\n";
    }
}

void Graph::printWorkingEdges(std::ostream &outStream) {
    outStream << "=== workingEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : workingLabelPairToWorkingEdge) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        printGraphEntry(edge.second, nodeIndentationLevel, outStream);
    }
}

void Graph::exportDebugInformation(const QDir &directory) {
    if (!graphBase || !graphBase->pEdgesInitialSegmentsImage || !graphBase->pWorkingSegmentsImage ||
        !graphBase->pEdgesInitialSegmentsImage->GetLargestPossibleRegion().GetNumberOfPixels() ||
        !graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion().GetNumberOfPixels()) {
        throw std::runtime_error("Debug export requires a working segmentation and an edge image.");
    }
    logGraph(LogLevel::Info, __func__, QStringLiteral("Exporting graph debug information"), kIoCategory);
    const auto writeReport = [&](const QString &name, void (Graph::*print)(std::ostream &)) {
        const QString path = directory.filePath(name);
        std::ofstream out(std::filesystem::path{path.toStdU16String()});
        if (out) (this->*print)(out);
        out.close();
        if (!out) throw std::runtime_error(QStringLiteral("Cannot export %1").arg(path).toStdString());
    };
    writeReport(QStringLiteral("edgeIdLookup.txt"), &Graph::printEdgeIdLookUp);
    writeReport(QStringLiteral("workingNodes.txt"), &Graph::printWorkingNodes);
    writeReport(QStringLiteral("workingEdges.txt"), &Graph::printWorkingEdges);
    writeReport(QStringLiteral("initialNodes.txt"), &Graph::printInitialNodes);
    writeReport(QStringLiteral("initialTwoSidedEdges.txt"), &Graph::printInitialTwoSidedEdges);
    writeReport(QStringLiteral("initialOneSidedEdges.txt"), &Graph::printInitialOneSidedEdges);
    const auto writeImage = [&](const dataType::SegmentsStorage &image, const QString &name) {
        const QString path = directory.filePath(name);
        try {
            const auto context = volume_io::imageContext(image.GetMetaDataDictionary());
            volume_io::writeLabelVolume(image, path, context.get());
        } catch (const std::exception &error) {
            throw std::runtime_error(QStringLiteral("Cannot export %1: %2")
                .arg(QFileInfo(path).absoluteFilePath(), QString::fromUtf8(error.what())).toStdString());
        }
    };
    writeImage(*graphBase->pEdgesInitialSegmentsImage, QStringLiteral("initialEdges.nrrd"));
    writeImage(*graphBase->pWorkingSegmentsImage, QStringLiteral("workingSegments.nrrd"));
}
