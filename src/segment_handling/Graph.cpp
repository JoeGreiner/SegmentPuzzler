#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include "Graph.h"
#include <itkImageFileWriter.h>
#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_set>
#include <vector>
#include <itkNeighborhoodIterator.h>
#include <itkBinaryThresholdImageFunction.h>
#include <QElapsedTimer>
#include <chrono>
#include <QStringList>
#include "src/utils/AppLogger.h"
#include "src/utils/systemStats.h"
#include "src/utils/utils.h"
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

template<typename MapType>
QString joinPairs(const MapType &values) {
    QStringList parts;
    for (const auto &entry : values) {
        parts << QStringLiteral("%1=%2")
                     .arg(static_cast<qulonglong>(entry.first))
                     .arg(QString::number(entry.second, 'g', 6));
    }
    return parts.join(QStringLiteral(", "));
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
        const Graph::SegmentsImageType::Pointer &selectedSegmentation,
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
    const auto *selectedBuffer = selectedSegmentation->GetBufferPointer();
    if (selectedBuffer == nullptr || !region.IsInside(seed) ||
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
                if (selectedBuffer[linearIndex(index)] != selectedLabel) {
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
    const Graph::SegmentsImageType::Pointer &selectedSegmentation,
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
    plan.actualMaximumLabel = backgroundId;
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
    const auto *selectedBuffer = selectedSegmentation->GetBufferPointer();
    if (selectedBuffer == nullptr) {
        plan.error = QStringLiteral("The selected segmentation has no voxel buffer.");
        return plan;
    }

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
        for (std::size_t y = 0; y < dimY; ++y) {
            for (std::size_t x = 0; x < dimX; ++x) {
                const std::size_t index = x + y * dimX + z * sliceStride;
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
                if (z + 1 < dimZ) recordAdjacency(label, selectedBuffer[index + sliceStride]);
            }
        }
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
    std::size_t initialNodeVoxelCapacity = 0;
    std::size_t oneSidedInitialEdgeCount = 0;
    std::size_t oneSidedInitialEdgeVoxelCount = 0;
    std::size_t oneSidedInitialEdgeVoxelCapacity = 0;
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
        summary.initialNodeVoxelCapacity += initialNode->voxels.capacity();
        for (const auto &[neighborLabel, oneSidedInitialEdge] : initialNode->neighborLabelToOneSidedInitialEdge) {
            if (oneSidedInitialEdge == nullptr) {
                continue;
            }
            ++summary.oneSidedInitialEdgeCount;
            summary.oneSidedInitialEdgeVoxelCount += oneSidedInitialEdge->voxels.size();
            summary.oneSidedInitialEdgeVoxelCapacity += oneSidedInitialEdge->voxels.capacity();
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
        (static_cast<long double>(summary.initialNodeVoxelCapacity) +
         static_cast<long double>(summary.oneSidedInitialEdgeVoxelCapacity) +
         static_cast<long double>(summary.twoSidedInitialEdgeOwnedVoxelCapacity)) * sizeof(Voxel) +
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
            "initial_nodes=%5 initial_node_voxels=%6 initial_node_voxel_capacity=%7 "
            "one_sided_initial_edges=%8 one_sided_edge_voxels=%9 one_sided_edge_voxel_capacity=%10 "
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
            .arg(static_cast<qulonglong>(summary.initialNodeVoxelCapacity))
            .arg(static_cast<qulonglong>(summary.oneSidedInitialEdgeCount))
            .arg(static_cast<qulonglong>(summary.oneSidedInitialEdgeVoxelCount))
            .arg(static_cast<qulonglong>(summary.oneSidedInitialEdgeVoxelCapacity))
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

struct NeighborWorkingGroup {
    Graph::SegmentIdType workingLabel = 0;
    std::vector<Graph::SegmentIdType> initialLabels;
};

} // namespace


void Graph::constructFromVolume(itk::Image<SegmentIdType, 3>::Pointer pImage,
                                int graphBuildThreadCount) {
    const bool logGraphStorage = isGraphDebugLoggingEnabled(verbose);
    initializeEdgeVolumeAndEdgeStatus();
    updateBackgroundIdFromVolume(pImage);
    pIgnoredSegmentLabels->push_back(backgroundId);

    nextFreeId = getNextFreeId(pImage);
    constructInitialNodes(pImage);
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("initial_nodes"));
    }
    segmentManager.computeOneSidedEdgesOnAllInitialNodes(graphBuildThreadCount);
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("one_sided_initial_edges"));
    }
    segmentManager.buildTwoSidedInitialEdgesFromOneSidedInitialEdges(graphBuildThreadCount);
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("two_sided_initial_edges"));
    }
    segmentManager.buildWorkingGraphFromInitialGraph();
    if (logGraphStorage) {
        logStorageAfterGraphPhase(*this, QStringLiteral("working_graph"));
    }
}

void Graph::constructInitialNodes(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    ScopedGraphTimer totalTimer(verbose, __func__, QStringLiteral("Constructing initial nodes"));
    std::vector<std::size_t> voxelCountByLabel(nextFreeId, 0);
    {   ScopedGraphTimer histogramTimer(verbose, __func__, QStringLiteral("Building segment histogram"));
        // get histogram of label ids to preallocate voxel array sizes
        itk::ImageRegionConstIterator<SegmentsImageType> it(pImage, pImage->GetLargestPossibleRegion());
        it.GoToBegin();
        while (!it.IsAtEnd()) {
            ++voxelCountByLabel[it.Get()];
            ++it;
        }
    }

    std::size_t initialNodeCount = 0;
    std::size_t ignoredLabelCount = 0;
    std::size_t ignoredVoxelCount = 0;
    std::size_t backgroundVoxelCount = 0;
    std::size_t largestForegroundLabelVoxelCount = 0;
    for (std::size_t labelIndex = 0; labelIndex < voxelCountByLabel.size(); ++labelIndex) {
        const std::size_t voxelCount = voxelCountByLabel[labelIndex];
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
        std::vector<InitialNode *> initialNodeByLabel;
        {
            ScopedGraphTimer initialNodeTimer(verbose, __func__, QStringLiteral("Allocating initial nodes"));
            QElapsedTimer phaseTimer;
            phaseTimer.start();
            initialNodeByLabel.resize(nextFreeId, nullptr);
            const double resizeDenseNodeIndexMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

            phaseTimer.restart();
            segmentManager.clearGraphAndReserveInitialNodes(initialNodeCount);
            const double resetPreviousGraphMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

            phaseTimer.restart();
            for (std::size_t labelIndex = 0; labelIndex < voxelCountByLabel.size(); ++labelIndex) {
                const std::size_t voxelCount = voxelCountByLabel[labelIndex];
                const auto label = static_cast<SegmentIdType>(labelIndex);
                if (voxelCount == 0 || isIgnoredId(label)) {
                    continue;
                }
                segmentManager.addInitialNode(label, voxelCount);
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
            const SegmentIdType *segmentLabelBuffer = pImage->GetBufferPointer();
            const itk::Size<3> &size = pImage->GetLargestPossibleRegion().GetSize();

            for (unsigned int z = 0; z < size[2]; ++z) {
                for (unsigned int y = 0; y < size[1]; ++y) {
                    for (unsigned int x = 0; x < size[0]; ++x) {
                        const std::size_t voxelIndex =
                            static_cast<std::size_t>(z) * size[1] * size[0] +
                            static_cast<std::size_t>(y) * size[0] + x;
                        InitialNode *initialNode = initialNodeByLabel[segmentLabelBuffer[voxelIndex]];
                        if (initialNode != nullptr) {
                            initialNode->voxels.emplace_back(x, y, z);
                        }
                    }
                }
            }
        }

        {
            ScopedGraphTimer finalizeNodesTimer(verbose, __func__, QStringLiteral("Finalizing initial node ROIs"));
            for (InitialNode *initialNode : initialNodeByLabel) {
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
    graphBase->pEdgesInitialSegmentsImage = GraphBase::EdgeImageType::New();
    graphBase->pEdgesInitialSegmentsImage->SetRegions(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
    graphBase->pEdgesInitialSegmentsImage->Allocate(true);
    graphBase->pEdgesInitialSegmentsImage->FillBuffer(0);


    graphBase->colorLookUpEdgesStatus.emplace(0, qRgb(255, 255, 255));
    graphBase->colorLookUpEdgesStatus.emplace(1, qRgb(0, 0, 255));
    graphBase->colorLookUpEdgesStatus.emplace(-1, qRgb(255, 255, 0));
    graphBase->colorLookUpEdgesStatus.emplace(-2, qRgb(255, 0, 0));
    graphBase->colorLookUpEdgesStatus.emplace(2, qRgb(0, 255, 0));


    if (ownedEdgesSignal == nullptr) {
        ownedEdgesSignal = std::make_unique<itkSignal<dataType::MappedEdgeIdType>>(
                graphBase->pEdgesInitialSegmentsImage);
    } else {
        ownedEdgesSignal->updateImage(graphBase->pEdgesInitialSegmentsImage);
    }
    graphBase->pEdgesInitialSegmentsITKSignal = ownedEdgesSignal.get();
}


Graph::SegmentIdType Graph::getSmallestSegmentId(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Finding smallest segment id"));
    SegmentIdType smallestId = std::numeric_limits<SegmentIdType>::max(), currentId;
    itk::ImageRegionConstIterator<SegmentsImageType> it(pImage, pImage->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        currentId = it.Get();
        if (currentId < smallestId) {
            smallestId = currentId;
        }
        ++it;
    }
    logGraphDebugIf(verbose, __func__, QStringLiteral("Smallest segment id=%1").arg(smallestId));
    return smallestId;
}

Graph::SegmentIdType Graph::getLargestSegmentId(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Finding largest segment id"));
    SegmentIdType largestId = std::numeric_limits<SegmentIdType>::min(), currentId;
    itk::ImageRegionConstIterator<SegmentsImageType> it(pImage, pImage->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        currentId = it.Get();
        if (currentId > largestId) {
            largestId = currentId;
        }
        ++it;
    }
    logGraphDebugIf(verbose, __func__, QStringLiteral("Largest segment id=%1").arg(largestId));
    return largestId;
}


Graph::SegmentIdType Graph::getNextFreeId(itk::Image<Graph::SegmentIdType, 3>::Pointer pImage) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Finding next free segment id"));

    SegmentIdType largestId = 0, currentId;
    itk::ImageRegionConstIterator<SegmentsImageType> it(pImage, pImage->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        currentId = it.Get();
        if (currentId > largestId) {
            largestId = currentId;
        }
        ++it;
    }
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

    EdgePairIdType pairId;
    bool updateSegmentImage = false;     // optimization: only update the resulting segments at the end, do not update incrementally
    for (const auto numId : edgeIdsToMerge) {
        pairId = initialEdgeIdLookup[numId];
        mergeEdge(initialLabelPairToTwoSidedInitialEdge.at(pairId).get(), updateSegmentImage);
    }

    std::set<SegmentIdType> newWorkingNodeIds;
    for (const auto numId : edgeIdsToMerge) {
        pairId = initialEdgeIdLookup[numId];
        SegmentIdType initialNodeLabel = initialLabelPairToTwoSidedInitialEdge[pairId]->getLabelSmaller();
        SegmentIdType workingNodeLabel = initialNodes[initialNodeLabel]->getCurrentWorkingNodeLabel();
        newWorkingNodeIds.insert(workingNodeLabel);
    }

    for (auto &label : newWorkingNodeIds) {
        insertWorkingNodeInSegmentImage(*workingNodes[label]);
    }
    return newWorkingNodeIds;
}

void Graph::mergeEdge(TwoSidedInitialEdge *edge, bool updateSegmentImage) {
    double t = 0;
//    if (verbose) { t = utils::tic("Graph::mergeEdge called"); }

    SegmentIdType idWorkingNodeA = initialNodes[edge->getLabelSmaller()]->getCurrentWorkingNodeLabel();
    SegmentIdType idWorkingNodeB = initialNodes[edge->getLabelBigger()]->getCurrentWorkingNodeLabel();

    if (idWorkingNodeA != idWorkingNodeB) {
        SegmentIdType idOfNewNode = nextFreeId;
        nextFreeId++;
        logGraphDebugIf(
            verbose,
            __func__,
            QStringLiteral("Working nodes %1,%2 -> %3 via initial edge %4")
                .arg(idWorkingNodeA)
                .arg(idWorkingNodeB)
                .arg(idOfNewNode)
                .arg(edge->numId));

        // remove old nodes and edges
        WorkingNode nodeA = *workingNodes[idWorkingNodeA];
        WorkingNode nodeB = *workingNodes[idWorkingNodeB];

        std::vector<WorkingNode> vecOfWorkingNodes{nodeA, nodeB};
        WorkingNode *newNode = new WorkingNode(vecOfWorkingNodes, idOfNewNode, initialNodes);

        segmentManager.removeWorkingNode(workingNodes[idWorkingNodeA].get());
        segmentManager.removeWorkingNode(workingNodes[idWorkingNodeB].get());

        segmentManager.addWorkingNode(newNode);
        segmentManager.recalculateEdgesOnWorkingNode(workingNodes[idOfNewNode].get());


        if (updateSegmentImage) {
            insertWorkingNodeInSegmentImage(*workingNodes[idOfNewNode]);
        }

        edge->setShouldMergeYes();
        auto numId = edge->numId;
        graphBase->edgeStatus[numId] = 2;

    }
//    if (verbose) { utils::toc(t, "Graph::mergeEdge finished"); }
}

void Graph::insertWorkingNodeInSegmentImage(WorkingNode &pWorkingNode) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Writing working node into segment image"));

//    auto voxelList = pWorkingNode.getVoxelArray();
    auto voxelListPtr = pWorkingNode.getVoxelLists();
    auto pBuffer = graphBase->pWorkingSegmentsImage->GetBufferPointer();
    auto pRegion = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion();
    auto pSize = pRegion.GetSize();
    auto label = pWorkingNode.getLabel();
    const unsigned long strideZ = pSize[1] * pSize[0];
    const unsigned long strideY = pSize[0];

//    for (long long i = 0; i < static_cast<long long>(voxelList.size()); i++) {
//        for (auto &voxel : *voxelList[i]) {
//            graphBase->pWorkingSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, pWorkingNode.getLabel());
//            write directly on the internal buffer for more speed
//        }
//    }
//#pragma omp parallel for schedule(dynamic)
    for (long long i = 0; i < static_cast<long long>(voxelListPtr.size()); i++) {
        for (auto &voxel: *voxelListPtr[i]) {
            unsigned long linearIndex = (voxel.z * strideZ) + (voxel.y * strideY) + voxel.x;
            pBuffer[linearIndex] = label;
        }
    }
//    for (auto &voxelListEntry : voxelList) {
//        unsigned long linearIndex = (voxelListEntry.z * strideZ) + (voxelListEntry.y * strideY) + voxelListEntry.x;
//        pBuffer[linearIndex] = label;
//    }

}


void Graph::unmergeEdges(std::set<EdgeNumIdType> &vecOfEdgeIdsToUnMerge) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Unmerging selected edges"));
    logGraphDebugIf(verbose, __func__, QStringLiteral("Edge ids=%1").arg(joinIds(vecOfEdgeIdsToUnMerge)));

    EdgePairIdType pairId;
    SegmentIdType labelSmaller, labelBigger, currentWorkingNodeIdA, currentWorkingNodeIdB;
    for (auto &numId : vecOfEdgeIdsToUnMerge) {
        pairId = initialEdgeIdLookup[numId];

//         set status to no
        graphBase->edgeStatus[numId] = -2;
        initialLabelPairToTwoSidedInitialEdge[pairId]->setShouldMergeNo();
        labelSmaller = initialLabelPairToTwoSidedInitialEdge[pairId]->getLabelSmaller();
        labelBigger = initialLabelPairToTwoSidedInitialEdge[pairId]->getLabelBigger();
        currentWorkingNodeIdA = initialNodes[labelSmaller]->getCurrentWorkingNodeLabel();
        currentWorkingNodeIdB = initialNodes[labelBigger]->getCurrentWorkingNodeLabel();


        if (currentWorkingNodeIdA == currentWorkingNodeIdB) {

            auto splitPair = calculateGraphDistancesFromEdge(workingNodes[currentWorkingNodeIdA].get(),
                                                             initialLabelPairToTwoSidedInitialEdge[pairId].get());

            std::vector<SegmentIdType> initialNodeIdsSplitA = splitPair.first;
            std::vector<SegmentIdType> initialNodeIdsSplitB = splitPair.second;
            unmergeEdge(workingNodes[currentWorkingNodeIdA].get(), initialNodeIdsSplitA, initialNodeIdsSplitB);
        }
//        graphBase->edgeStatus[numId] = -2;

    }
}


std::pair<std::vector<Graph::SegmentIdType>, std::vector<Graph::SegmentIdType>>
Graph::calculateGraphDistancesFromEdge(WorkingNode *nodeToCalculateDistanceOn,
                                       TwoSidedInitialEdge *edgeToCalculateDistanceFrom) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Calculating graph distances from edge"));
    std::map<SegmentIdType, float> distA;
    std::map<SegmentIdType, float> distB;

    EdgeNumIdType forbiddenEdgeId = edgeToCalculateDistanceFrom->numId;

    SegmentIdType labelInitialNodeA = edgeToCalculateDistanceFrom->getLabelSmaller();
    SegmentIdType labelInitialNodeB = edgeToCalculateDistanceFrom->getLabelBigger();
    OneSidedInitialEdge *oneSidedEdgeFromA =
        initialNodes.at(labelInitialNodeA)->neighborLabelToOneSidedInitialEdge.at(labelInitialNodeB).get();

    using distLabelPair = std::pair<SegmentIdType, float>;

    std::priority_queue<distLabelPair, std::vector<distLabelPair>, std::greater<>> priorityQueue;


    // initialize source distance to 0, everything to infty
    // add all to queue
    for (auto initialNode : nodeToCalculateDistanceOn->subInitialNodes) {
        distA[initialNode.second->getLabel()] = std::numeric_limits<float>::max();
    }
    distA[labelInitialNodeA] = 0;
    priorityQueue.push(distLabelPair{distA[labelInitialNodeA], labelInitialNodeA});

    while (!priorityQueue.empty()) {
        SegmentIdType label = priorityQueue.top().second;
        priorityQueue.pop();
        CenterOfMass comSource;
        if (label == labelInitialNodeA) {
            // use different starting point: center of mass of initial edge + 1 unit vector in direction of center of mass of the node
            CenterOfMass comSourceNode = CenterOfMass(initialNodes[label].get());
            comSource = CenterOfMass(oneSidedEdgeFromA);
            float dX = comSourceNode.x - comSource.x;
            float dY = comSourceNode.y - comSource.y;
            float dZ = comSourceNode.z - comSource.z;
            float length = std::sqrt(dX * dX + dY * dY + dZ * dZ);
            comSource.x = comSource.x + dX / length;
            comSource.y = comSource.y + dY / length;
            comSource.z = comSource.z + dZ / length;
        } else {
            comSource = CenterOfMass(initialNodes[label].get());
        }
        //TODO: Add check for 1vx volumes, may actually crash simulations
        for (auto edge : initialNodes[label]->neighborLabelToTwoSidedInitialEdge) {
//            std::cout << edge.second->pairId.first << " -> " << edge.second->pairId.second << "\n";
            if (edge.second->numId != forbiddenEdgeId) {
//                std::cout << "not forbidden!\n";
                if (nodeToCalculateDistanceOn->subInitialNodes.count(edge.first) > 0) { // if edge is inside WNode
//                    std::cout << "inside workingnode!\n";

                    CenterOfMass comTargetNode(initialNodes[edge.first].get());
                    float distSourceTarget = comSource.distTo(comTargetNode);
                    float tmpDist = distA[label] + distSourceTarget;
//                    std::cout << "distA[label] " << distA[label] << " dist source->target: " << distSourceTarget << "  distA[target]: " <<   distA[edge.first] << "\n";
                    if (tmpDist < distA[edge.first]) {
                        distA[edge.first] = tmpDist;
//                        std::cout << "distA[target]: " <<   distA[edge.first] << "\n";
                        priorityQueue.push(distLabelPair{distA[edge.first], edge.first});
                    }


                }
            }
        }
    }

    // repeat for second node
    for (auto initialNode : nodeToCalculateDistanceOn->subInitialNodes) {
        distB[initialNode.second->getLabel()] = std::numeric_limits<float>::max();
    }
    distB[labelInitialNodeB] = 0;
    priorityQueue.push(distLabelPair{distB[labelInitialNodeB], labelInitialNodeB});

    while (!priorityQueue.empty()) {
        SegmentIdType label = priorityQueue.top().second;
        priorityQueue.pop();
        CenterOfMass comSource;
        if (label == labelInitialNodeB) {
            CenterOfMass comSourceNode = CenterOfMass(initialNodes[label].get());
            comSource = CenterOfMass(oneSidedEdgeFromA);
            float dX = comSourceNode.x - comSource.x;
            float dY = comSourceNode.y - comSource.y;
            float dZ = comSourceNode.z - comSource.z;
            float length = std::sqrt(dX * dX + dY * dY + dZ * dZ);
            comSource.x = comSource.x + dX / length;
            comSource.y = comSource.y + dY / length;
            comSource.z = comSource.z + dZ / length;
        } else {
            comSource = CenterOfMass(initialNodes[label].get());
        }
        for (auto edge : initialNodes[label]->neighborLabelToTwoSidedInitialEdge) {
            if (edge.second->numId != forbiddenEdgeId) {
                if (nodeToCalculateDistanceOn->subInitialNodes.count(edge.first) > 0) { // if edge is inside WNode

                    CenterOfMass comTargetNode(initialNodes[edge.first].get());
                    float tmpDist = distB[label] + comSource.distTo(comTargetNode);
                    if (tmpDist < distB[edge.first]) {
                        distB[edge.first] = tmpDist;
                        priorityQueue.push(distLabelPair{distB[edge.first], edge.first});
                    }

                }
            }
        }
    }

    std::vector<SegmentIdType> initialNodeIdsSplitA;
    std::vector<SegmentIdType> initialNodeIdsSplitB;

    for (auto &elem : distA) {
        if (distA[elem.first] < distB[elem.first]) {
            initialNodeIdsSplitA.push_back(elem.first);
        } else if (distA[elem.first] > distB[elem.first]) {
            initialNodeIdsSplitB.push_back(elem.first);
        } else {
            logGraph(LogLevel::Warning,
                     __func__,
                     QStringLiteral("Distance partitions tie for initial node %1 at value %2")
                         .arg(elem.first)
                         .arg(QString::number(distA[elem.first], 'g', 6)));
            initialNodeIdsSplitB.push_back(elem.first);
        }
    }

    logGraphDebugIf(verbose, __func__, QStringLiteral("Distances A: %1").arg(joinPairs(distA)));
    logGraphDebugIf(verbose, __func__, QStringLiteral("Distances B: %1").arg(joinPairs(distB)));
    logGraphDebugIf(verbose,
                    __func__,
                    QStringLiteral("Split labels A=[%1], B=[%2]")
                        .arg(joinIds(initialNodeIdsSplitA))
                        .arg(joinIds(initialNodeIdsSplitB)));
    return {initialNodeIdsSplitA, initialNodeIdsSplitB};
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

    for (auto &voxel : initialEdge.voxels) {
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
    //TODO: Handle cases where distance is inf/max, and assign them a own label (as they are not connected to the rest of the label at all)
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Splitting working node into two groups"));


    SegmentIdType labelOfNodeA = nextFreeId;
    nextFreeId++;
    SegmentIdType labelOfNodeB = nextFreeId;
    nextFreeId++;


    WorkingNode *workingNodeA = new WorkingNode(initialNodeIdsA, labelOfNodeA, initialNodes);
    segmentManager.addWorkingNode(workingNodeA);
    WorkingNode *workingNodeB = new WorkingNode(initialNodeIdsB, labelOfNodeB, initialNodes);
    segmentManager.addWorkingNode(workingNodeB);

    segmentManager.recalculateEdgesOnWorkingNode(workingNodeA);
    segmentManager.recalculateEdgesOnWorkingNode(workingNodeB);

    std::vector<SegmentIdType> idsOfConnectedNodes = workingNodeA->getVectorOfConnectedNodeIds();
    for (auto &id : idsOfConnectedNodes) {
        segmentManager.recalculateEdgesOnWorkingNode(workingNodes[id].get());
    }

    idsOfConnectedNodes = workingNodeB->getVectorOfConnectedNodeIds();
    for (auto &id : idsOfConnectedNodes) {
        segmentManager.recalculateEdgesOnWorkingNode(workingNodes[id].get());
    }

    segmentManager.removeWorkingNode(workingNodeToSplit);

    insertWorkingNodeInSegmentImage(*workingNodes[labelOfNodeA]);
    insertWorkingNodeInSegmentImage(*workingNodes[labelOfNodeB]);

    splitIntoConnectedComponentsOfWorkingNode(*workingNodes[labelOfNodeA]);
    splitIntoConnectedComponentsOfWorkingNode(*workingNodes[labelOfNodeB]);

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

                for (auto &voxel : initialNode.second->voxels) {
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
        // crude idea:
        // first, unmerge all underlying nodes to workingNode(initialNode)
        // then, merge subsetA -> nodeA, subsetB -> nodeB

        SegmentIdType labelOfNodeA = nextFreeId;
        nextFreeId++;
        SegmentIdType labelOfNodeB = nextFreeId;
        nextFreeId++;


        WorkingNode *workingNodeA = new WorkingNode(labelsOfInitialNodesNearSmaller, labelOfNodeA, initialNodes);
        segmentManager.addWorkingNode(workingNodeA);
        WorkingNode *workingNodeB = new WorkingNode(labelsOfInitialNodesNearBigger, labelOfNodeB, initialNodes);
        segmentManager.addWorkingNode(workingNodeB);
        segmentManager.recalculateEdgesOnWorkingNode(workingNodeA);
        segmentManager.recalculateEdgesOnWorkingNode(workingNodeB);

        std::vector<SegmentIdType> idsOfConnectedNodes = workingNodeA->getVectorOfConnectedNodeIds();
        for (auto &id : idsOfConnectedNodes) {
            segmentManager.recalculateEdgesOnWorkingNode(workingNodes[id].get());
        }

        idsOfConnectedNodes = workingNodeB->getVectorOfConnectedNodeIds();
        for (auto &id : idsOfConnectedNodes) {
            segmentManager.recalculateEdgesOnWorkingNode(workingNodes[id].get());
        }

        segmentManager.removeWorkingNode(workingNodes[currentWorkingNodeLabel].get());


        insertWorkingNodeInSegmentImage(*workingNodes[labelOfNodeA]);
        insertWorkingNodeInSegmentImage(*workingNodes[labelOfNodeB]);

        splitIntoConnectedComponentsOfWorkingNode(*workingNodes[labelOfNodeA]);
        splitIntoConnectedComponentsOfWorkingNode(*workingNodes[labelOfNodeB]);

//        for (auto node : connectedWorkingNodes){
//            std::cout << "CC A: " << node->getLabel() << "\n";
//        }
//        connectedWorkingNodes = getConnectedConnectedComponentsOfWorkingNode(*workingNodes[labelOfNodeB]);
//        for (auto node : connectedWorkingNodes){
//            std::cout << "CC A: " << node->getLabel() << "\n";
//        }

    }
}

void Graph::splitIntoConnectedComponentsOfWorkingNode(
        WorkingNode &workingNodeToAnalyze) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Splitting working node into connected components"));


    std::set<SegmentIdType> visitedWorkingNodeLabels, initialNodesOfWorkingNode;
    std::queue<SegmentIdType> openWorkingNodeLabels;
    std::set<std::shared_ptr<WorkingNode>> connectedGroupOfInitialNodesAsWorkingNodes;

//    std::cout << "SubInitialNodes : ";
    for (auto &initialNode : workingNodeToAnalyze.subInitialNodes) {
        initialNodesOfWorkingNode.insert(initialNode.first);
//        std::cout << initialNode.first << " ";
    }
//    std::cout << "\n";



    // while not all initial nodes of the working node were visited
    while (visitedWorkingNodeLabels.size() != workingNodeToAnalyze.subInitialNodes.size()) {
        // push the first initial node that was not visited before in openworkingnodes
        for (auto &nodeId : initialNodesOfWorkingNode) {
            if (visitedWorkingNodeLabels.count(nodeId) == 0) {
                openWorkingNodeLabels.push(nodeId);
                break;
            }
        }


        std::set<SegmentIdType> visitedWorkingNodeLabelsThisRun;

        // visit all connected components of nodes that are in openworkingnodelabels
        while (!openWorkingNodeLabels.empty()) {
            SegmentIdType activeNode = openWorkingNodeLabels.front();
            openWorkingNodeLabels.pop();

            // go through all connections
            for (auto &twosidedEdge : initialNodes[activeNode]->neighborLabelToTwoSidedInitialEdge) {
                SegmentIdType neighboringInitialNodeId = twosidedEdge.first;
                if (workingNodeToAnalyze.subInitialNodes.find(neighboringInitialNodeId) !=
                    workingNodeToAnalyze.subInitialNodes.end()) { // if it is part of the working node
                    if (!visitedWorkingNodeLabelsThisRun.count(neighboringInitialNodeId)) { // if not visited before
                        openWorkingNodeLabels.push(neighboringInitialNodeId); // push it into list
                    }
                }
            }
            visitedWorkingNodeLabelsThisRun.insert(activeNode);
        }

        logGraphDebugIf(verbose,
                        __func__,
                        QStringLiteral("Connected component labels=[%1]").arg(joinIds(visitedWorkingNodeLabelsThisRun)));

        SegmentIdType labelOfNewNode = nextFreeId;
        nextFreeId++;
        WorkingNode *newWorkingNode = new WorkingNode(visitedWorkingNodeLabelsThisRun, labelOfNewNode, initialNodes);
        segmentManager.addWorkingNode(newWorkingNode);
        segmentManager.recalculateEdgesOnWorkingNode(newWorkingNode);

        //TODO: export this as a function
        std::vector<SegmentIdType> idsOfConnectedNodes = newWorkingNode->getVectorOfConnectedNodeIds();
        for (auto &id : idsOfConnectedNodes) {
            segmentManager.recalculateEdgesOnWorkingNode(workingNodes[id].get());
        }

        for (auto id : visitedWorkingNodeLabelsThisRun) {
            visitedWorkingNodeLabels.insert(id);
        }

        insertWorkingNodeInSegmentImage(*newWorkingNode);
        // end todo


    }
    segmentManager.removeWorkingNode(&workingNodeToAnalyze);
}


Graph::SegmentIdType Graph::getLargestIdInSegmentVolume(Graph::SegmentsImageType::Pointer pSegment) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Finding largest id in segment volume"));
    SegmentIdType backgroundLabel = 0;
    itk::ImageRegionConstIterator<SegmentsImageType>
            it(pSegment, pSegment->GetLargestPossibleRegion());

    it.GoToBegin();
    while (!it.IsAtEnd()) {
        if (it.Get() > backgroundLabel) {
            backgroundLabel = it.Get();
        }
        ++it;
    }
    logGraphDebugIf(verbose, __func__, QStringLiteral("Largest id in segment volume=%1").arg(backgroundLabel));
    return backgroundLabel;
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
//    // as initialnodes are not saved explicitly, workaround:
//    // unsplit working node into all initialnode
//    // get initialnode at (x,y,z)
//    // create working segments with all other initialnodes but the selected one
//    // split new working segment into connected components
//    SegmentIdType labelOfWorkingNode = GraphBase::pWorkingSegmentsImage->GetPixel({x, y, z});
//    if (!isIgnoredId(labelOfWorkingNode)) {
//        std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> initialNodesOfWorkingSegment = workingNodes.at(
//                labelOfWorkingNode)->subInitialNodes;
//        splitWorkingNodeIntoInitialNodes(labelOfWorkingNode);
//        SegmentIdType initialNodeAtPosition = GraphBase::pWorkingSegmentsImage->GetPixel({x, y, z});
//        initialNodesOfWorkingSegment.erase(initialNodeAtPosition);
//        SegmentIdType labelOfNewNode = nextFreeId;
//        nextFreeId++;
//        std::vector<SegmentIdType> newInitialNodesOfWorkingSegment = utils::getKeyVecOfSharedPtrMap<SegmentIdType, InitialNode>(
//                initialNodesOfWorkingSegment);
//        if (!newInitialNodesOfWorkingSegment.empty()) {
//            for (auto nodeLabel : newInitialNodesOfWorkingSegment) {
//                // special case: initialNode == workingNode
//                segmentManager.removeWorkingNode(workingNodes[nodeLabel].get());
//            }
//            WorkingNode *newWorkingNode = new WorkingNode(newInitialNodesOfWorkingSegment, labelOfNewNode,
//                                                          initialNodes);
//            segmentManager.addWorkingNode(newWorkingNode);
//            segmentManager.recalculateEdgesOnWorkingNode(newWorkingNode);
//            insertWorkingNodeInSegmentImage(*newWorkingNode);
//            splitIntoConnectedComponentsOfWorkingNode(*newWorkingNode);
//        }
//    }
}

void Graph::splitWorkingNodeIntoInitialNodes(SegmentIdType workingNodeIdToSplit) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Splitting working node into initial nodes"));
    if (!isIgnoredId(workingNodeIdToSplit)) {

        auto pToWorkingNode = workingNodes.at(workingNodeIdToSplit);

        std::vector<SegmentIdType> idsOfSubInitialNodes;
        for (auto &initialNode : pToWorkingNode->subInitialNodes) {
            idsOfSubInitialNodes.push_back(initialNode.first);
        }
        logGraphDebugIf(verbose,
                        __func__,
                        QStringLiteral("Working node %1 splits into initial nodes [%2]")
                            .arg(workingNodeIdToSplit)
                            .arg(joinIds(idsOfSubInitialNodes)));

        // remove corrosponding workingedges and workingNode
        segmentManager.removeWorkingNode(pToWorkingNode.get());


        std::set<SegmentIdType> setOfInsertedWorkingNodes;
        // insert initialnodes into workingnodes
        for (auto &subInitialNodeId : idsOfSubInitialNodes) {

            // create new working node based on inital node
            WorkingNode *newWorkingNode = new WorkingNode(initialNodes[subInitialNodeId].get(), subInitialNodeId,
                                                          initialNodes);
            segmentManager.addWorkingNode(newWorkingNode);
            setOfInsertedWorkingNodes.insert(subInitialNodeId);

            for (auto &voxelArray : newWorkingNode->getVoxelLists()) {
                for (auto voxel : *voxelArray) {
                    graphBase->pWorkingSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, subInitialNodeId);
                }
            }
        }

        // put neighboring nodes in the set
        std::set<SegmentIdType> setOfInsertedWorkingNodesAndNeighbors;
        for (auto id : setOfInsertedWorkingNodes) {
            setOfInsertedWorkingNodesAndNeighbors.insert(id);
            std::vector<SegmentIdType> idsOfConnectedNodes = workingNodes.at(id)->getVectorOfConnectedNodeIds();
            for (auto neighborId : idsOfConnectedNodes) {
                setOfInsertedWorkingNodesAndNeighbors.insert(neighborId);
            }
        }

        // update workingedges of new workingnodes and their neighbors
        for (auto id : setOfInsertedWorkingNodesAndNeighbors) {
            segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(id).get());
        }

        // reset merge status
        char defaultEdgeStatus = 0;
        for (auto id : setOfInsertedWorkingNodes) {
            for (auto &edge : workingNodes.at(id)->neighborLabelToWorkingEdge) {
                for (auto pInitialEdge : edge.second->constituentTwoSidedInitialEdges) {
                    graphBase->edgeStatus.at(pInitialEdge->numId) = defaultEdgeStatus;
                }
            }
        }
    }

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

    struct SavedWorkingGroup {
        SegmentIdType workingLabel = 0;
        std::vector<SegmentIdType> initialLabels;
    };

    std::vector<SavedWorkingGroup> savedWorkingGroups;
    savedWorkingGroups.reserve(workingNodes.size());
    for (const auto &workingEntry : workingNodes) {
        if (workingEntry.second == nullptr) {
            continue;
        }

        SavedWorkingGroup group;
        group.workingLabel = workingEntry.first;
        group.initialLabels.reserve(workingEntry.second->subInitialNodes.size());
        for (const auto &initialEntry : workingEntry.second->subInitialNodes) {
            group.initialLabels.push_back(initialEntry.first);
        }
        std::sort(group.initialLabels.begin(), group.initialLabels.end());
        savedWorkingGroups.push_back(std::move(group));
    }

    auto rewriteInitialLabelsToWorkingImage = [this]() {
        graphBase->pWorkingSegmentsImage->FillBuffer(backgroundId);
        for (const auto &initialEntry : initialNodes) {
            if (initialEntry.second == nullptr || isIgnoredId(initialEntry.first)) {
                continue;
            }
            for (const Voxel &voxel : initialEntry.second->voxels) {
                graphBase->pWorkingSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, initialEntry.first);
            }
        }
    };

    auto rewriteWorkingLabelsToWorkingImage = [this]() {
        graphBase->pWorkingSegmentsImage->FillBuffer(backgroundId);
        for (const auto &workingEntry : workingNodes) {
            if (workingEntry.second != nullptr && !isIgnoredId(workingEntry.first)) {
                insertWorkingNodeInSegmentImage(*workingEntry.second);
            }
        }
    };

    rewriteInitialLabelsToWorkingImage();

    std::unordered_set<SegmentIdType> ignoredLabels;
    ignoredLabels.insert(0);
    ignoredLabels.insert(backgroundId);
    ignoredLabels.insert(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end());

    ConnectedComponentSplitOptions options;
    options.connectivity = connectivity;
    options.ignoredLabels = std::move(ignoredLabels);
    options.nextFreeLabel = nextFreeId;
    stats = splitDisconnectedLabelComponentsInPlace(graphBase->pWorkingSegmentsImage, options);
    nextFreeId = std::max(nextFreeId, stats.nextFreeLabel);

    if (!stats.changed()) {
        rewriteWorkingLabelsToWorkingImage();
        return stats;
    }

    initializeEdgeVolumeAndEdgeStatus();
    constructInitialNodes(graphBase->pWorkingSegmentsImage);
    segmentManager.computeOneSidedEdgesOnAllInitialNodes(1);
    segmentManager.buildTwoSidedInitialEdgesFromOneSidedInitialEdges();

    std::unordered_set<SegmentIdType> assignedInitialLabels;
    assignedInitialLabels.reserve(initialNodes.size());
    std::unordered_set<SegmentIdType> usedWorkingLabels;
    usedWorkingLabels.reserve(savedWorkingGroups.size() + initialNodes.size());

    auto nextUnusedWorkingLabel = [this, &usedWorkingLabels](SegmentIdType preferredLabel) {
        SegmentIdType label = preferredLabel;
        while (usedWorkingLabels.count(label) > 0) {
            label = nextFreeId++;
        }
        usedWorkingLabels.insert(label);
        return label;
    };

    for (const SavedWorkingGroup &savedGroup : savedWorkingGroups) {
        std::vector<SegmentIdType> restoredInitialLabels;
        for (const SegmentIdType oldInitialLabel : savedGroup.initialLabels) {
            const auto splitIt = stats.finalLabelsByOriginalLabel.find(oldInitialLabel);
            if (splitIt != stats.finalLabelsByOriginalLabel.end()) {
                restoredInitialLabels.insert(restoredInitialLabels.end(),
                                             splitIt->second.begin(),
                                             splitIt->second.end());
            } else if (initialNodes.count(oldInitialLabel) > 0) {
                restoredInitialLabels.push_back(oldInitialLabel);
            }
        }

        std::sort(restoredInitialLabels.begin(), restoredInitialLabels.end());
        restoredInitialLabels.erase(std::unique(restoredInitialLabels.begin(), restoredInitialLabels.end()),
                                    restoredInitialLabels.end());
        restoredInitialLabels.erase(
            std::remove_if(restoredInitialLabels.begin(),
                           restoredInitialLabels.end(),
                           [this](SegmentIdType label) { return initialNodes.count(label) == 0 || isIgnoredId(label); }),
            restoredInitialLabels.end());

        if (restoredInitialLabels.empty()) {
            continue;
        }

        for (const SegmentIdType initialLabel : restoredInitialLabels) {
            assignedInitialLabels.insert(initialLabel);
        }

        const SegmentIdType workingLabel = nextUnusedWorkingLabel(savedGroup.workingLabel);
        auto *workingNode = new WorkingNode(restoredInitialLabels, workingLabel, initialNodes);
        segmentManager.addWorkingNode(workingNode);
    }

    for (const auto &initialEntry : initialNodes) {
        if (initialEntry.second == nullptr || assignedInitialLabels.count(initialEntry.first) > 0 ||
            isIgnoredId(initialEntry.first)) {
            continue;
        }

        assignedInitialLabels.insert(initialEntry.first);
        const SegmentIdType workingLabel = nextUnusedWorkingLabel(initialEntry.first);
        auto *workingNode = new WorkingNode(initialEntry.second.get(), workingLabel, initialNodes);
        segmentManager.addWorkingNode(workingNode);
    }

    auto recalculateAllWorkingEdges = [this]() {
        std::vector<SegmentIdType> labels;
        labels.reserve(workingNodes.size());
        for (const auto &workingEntry : workingNodes) {
            labels.push_back(workingEntry.first);
        }

        for (const SegmentIdType label : labels) {
            if (workingNodes.count(label) > 0) {
                segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(label).get());
            }
        }
    };

    auto splitWorkingNodeByVoxelConnectivity = [this, connectivity](SegmentIdType workingLabel) {
        const auto workingIt = workingNodes.find(workingLabel);
        if (workingIt == workingNodes.end() || workingIt->second == nullptr ||
            workingIt->second->subInitialNodes.size() <= 1) {
            return false;
        }

        WorkingNode *workingNode = workingIt->second.get();
        std::vector<SegmentIdType> labels;
        labels.reserve(workingNode->subInitialNodes.size());
        std::unordered_map<SegmentIdType, std::size_t> labelToIndex;
        labelToIndex.reserve(workingNode->subInitialNodes.size());
        std::vector<std::size_t> voxelCounts;
        voxelCounts.reserve(workingNode->subInitialNodes.size());

        for (const auto &initialEntry : workingNode->subInitialNodes) {
            labelToIndex[initialEntry.first] = labels.size();
            labels.push_back(initialEntry.first);
            voxelCounts.push_back(initialEntry.second != nullptr ? initialEntry.second->voxels.size() : 0);
        }

        std::vector<std::size_t> parent(labels.size());
        for (std::size_t index = 0; index < parent.size(); ++index) {
            parent[index] = index;
        }

        auto findRoot = [&parent](std::size_t index) {
            while (parent[index] != index) {
                parent[index] = parent[parent[index]];
                index = parent[index];
            }
            return index;
        };

        auto unionLabels = [&parent, &findRoot](std::size_t lhs, std::size_t rhs) {
            lhs = findRoot(lhs);
            rhs = findRoot(rhs);
            if (lhs != rhs) {
                parent[rhs] = lhs;
            }
        };

        const auto image = graphBase->pWorkingSegmentsImage;
        const auto size = image->GetLargestPossibleRegion().GetSize();
        const int dimX = static_cast<int>(size[0]);
        const int dimY = static_cast<int>(size[1]);
        const int dimZ = static_cast<int>(size[2]);

        const auto useOffset = [connectivity](int dx, int dy, int dz) {
            if (dx == 0 && dy == 0 && dz == 0) {
                return false;
            }
            if (connectivity == segment_puzzler::connected_components::ConnectivityStencil::Full) {
                return true;
            }
            return std::abs(dx) + std::abs(dy) + std::abs(dz) == 1;
        };

        for (const auto &initialEntry : workingNode->subInitialNodes) {
            if (initialEntry.second == nullptr) {
                continue;
            }
            const auto ownIndex = labelToIndex.at(initialEntry.first);
            for (const Voxel &voxel : initialEntry.second->voxels) {
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!useOffset(dx, dy, dz)) {
                                continue;
                            }

                            const int nx = voxel.x + dx;
                            const int ny = voxel.y + dy;
                            const int nz = voxel.z + dz;
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ) {
                                continue;
                            }

                            const SegmentIdType neighborLabel = image->GetPixel({nx, ny, nz});
                            const auto neighborIt = labelToIndex.find(neighborLabel);
                            if (neighborIt == labelToIndex.end()) {
                                continue;
                            }
                            unionLabels(ownIndex, neighborIt->second);
                        }
                    }
                }
            }
        }

        std::unordered_map<std::size_t, std::vector<SegmentIdType>> labelsByRoot;
        std::unordered_map<std::size_t, std::size_t> voxelsByRoot;
        labelsByRoot.reserve(labels.size());
        voxelsByRoot.reserve(labels.size());
        for (std::size_t labelIndex = 0; labelIndex < labels.size(); ++labelIndex) {
            const std::size_t root = findRoot(labelIndex);
            labelsByRoot[root].push_back(labels[labelIndex]);
            voxelsByRoot[root] += voxelCounts[labelIndex];
        }

        if (labelsByRoot.size() <= 1) {
            return false;
        }

        struct WorkingComponent {
            std::vector<SegmentIdType> initialLabels;
            std::size_t voxelCount = 0;
        };
        std::vector<WorkingComponent> components;
        components.reserve(labelsByRoot.size());
        for (auto &rootEntry : labelsByRoot) {
            std::sort(rootEntry.second.begin(), rootEntry.second.end());
            components.push_back({rootEntry.second, voxelsByRoot[rootEntry.first]});
        }
        std::sort(components.begin(), components.end(), [](const WorkingComponent &lhs, const WorkingComponent &rhs) {
            if (lhs.voxelCount != rhs.voxelCount) {
                return lhs.voxelCount > rhs.voxelCount;
            }
            return lhs.initialLabels.front() < rhs.initialLabels.front();
        });

        segmentManager.removeWorkingNode(workingNode);
        for (std::size_t componentIndex = 0; componentIndex < components.size(); ++componentIndex) {
            const SegmentIdType componentWorkingLabel =
                componentIndex == 0 ? workingLabel : nextFreeId++;
            auto *componentWorkingNode =
                new WorkingNode(components[componentIndex].initialLabels, componentWorkingLabel, initialNodes);
            segmentManager.addWorkingNode(componentWorkingNode);
        }
        return true;
    };

    recalculateAllWorkingEdges();

    std::vector<SegmentIdType> labelsToCheck;
    labelsToCheck.reserve(workingNodes.size());
    for (const auto &workingEntry : workingNodes) {
        labelsToCheck.push_back(workingEntry.first);
    }
    for (const SegmentIdType workingLabel : labelsToCheck) {
        splitWorkingNodeByVoxelConnectivity(workingLabel);
    }

    recalculateAllWorkingEdges();

    graphBase->pWorkingSegmentsImage->FillBuffer(backgroundId);
    for (const auto &workingEntry : workingNodes) {
        if (workingEntry.second != nullptr && !isIgnoredId(workingEntry.first)) {
            insertWorkingNodeInSegmentImage(*workingEntry.second);
        }
    }

    for (const auto &workingEntry : workingNodes) {
        if (workingEntry.second == nullptr) {
            continue;
        }
        const auto &subInitialNodes = workingEntry.second->subInitialNodes;
        for (const auto &initialEntry : subInitialNodes) {
            if (initialEntry.second == nullptr) {
                continue;
            }
            for (const auto &edgeEntry : initialEntry.second->neighborLabelToTwoSidedInitialEdge) {
                if (subInitialNodes.count(edgeEntry.first) == 0 || edgeEntry.second == nullptr) {
                    continue;
                }
                edgeEntry.second->setShouldMergeYes();
                graphBase->edgeStatus[edgeEntry.second->numId] = 2;
            }
        }
    }

    stats.nextFreeLabel = nextFreeId;
    stats.maxLabel = getLargestIdInSegmentVolume(graphBase->pWorkingSegmentsImage);
    return stats;
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
    if (request.targetWorkingLabel == 0 || request.strokePixels.size() < 2 ||
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
    if (maximumComponentLabel < 2 || assignedPartitionVoxelCount != targetVoxelCount) {
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

    std::vector<SegmentIdType> originalInitialLabels;
    originalInitialLabels.reserve(targetWorkingNode->subInitialNodes.size());
    for (const auto &[initialLabel, initialNode] : targetWorkingNode->subInitialNodes) {
        if (initialNode == nullptr) {
            return false;
        }
        originalInitialLabels.push_back(initialLabel);
    }

    std::unordered_map<SegmentIdType, std::vector<int>> voxelIndicesByInitialLabel;
    voxelIndicesByInitialLabel.reserve(originalInitialLabels.size());
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

    auto *workingSegmentsBuffer = graphBase->pWorkingSegmentsImage->GetBufferPointer();
    const auto workingSegmentsSize = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion().GetSize();
    const unsigned long strideZ = workingSegmentsSize[1] * workingSegmentsSize[0];
    const unsigned long strideY = workingSegmentsSize[0];
    const auto workingSegmentsLinearIndex = [strideY, strideZ](const Voxel &voxel) {
        return static_cast<unsigned long>(voxel.z) * strideZ +
               static_cast<unsigned long>(voxel.y) * strideY +
               static_cast<unsigned long>(voxel.x);
    };

    std::vector<ReplacementInitialComponent> replacementInitialComponents;
    replacementInitialComponents.reserve(targetVoxels.size());
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

    std::vector<NeighborWorkingGroup> neighborGroups;
    neighborGroups.reserve(targetWorkingNode->neighborLabelToWorkingEdge.size());
    const auto collectNeighborGroupsStart = Clock::now();
    for (const auto &edgeEntry : targetWorkingNode->neighborLabelToWorkingEdge) {
        const auto neighborIt = workingNodes.find(edgeEntry.first);
        if (neighborIt == workingNodes.end() || neighborIt->second == nullptr) {
            continue;
        }

        NeighborWorkingGroup group;
        group.workingLabel = edgeEntry.first;
        group.initialLabels.reserve(neighborIt->second->subInitialNodes.size());
        for (const auto &initialEntry : neighborIt->second->subInitialNodes) {
            group.initialLabels.push_back(initialEntry.first);
        }
        neighborGroups.push_back(std::move(group));
    }
    if (profileOut != nullptr) {
        profileOut->collectNeighborGroupsMs = durationMs(collectNeighborGroupsStart, Clock::now());
    }

    const auto shouldSplitNeighborIntoInitialNodes = [](const NeighborWorkingGroup &neighborGroup) {
        if (neighborGroup.initialLabels.size() > 1) {
            return true;
        }
        return neighborGroup.initialLabels.size() == 1 &&
               neighborGroup.workingLabel != neighborGroup.initialLabels.front();
    };

    const auto splitWorkingNodesStart = Clock::now();
    splitWorkingNodeIntoInitialNodes(targetWorkingLabel);
    for (const auto &neighborGroup : neighborGroups) {
        // Initial-edge recomputation must see neighboring labels in the working image as initial-node labels,
        // not stale synthetic working labels left behind by an earlier projected cut.
        if (shouldSplitNeighborIntoInitialNodes(neighborGroup) &&
            workingNodes.count(neighborGroup.workingLabel) > 0) {
            splitWorkingNodeIntoInitialNodes(neighborGroup.workingLabel);
        }
    }
    if (profileOut != nullptr) {
        profileOut->splitWorkingNodesMs = durationMs(splitWorkingNodesStart, Clock::now());
    }

    const auto removeOriginalNodesStart = Clock::now();
    for (const SegmentIdType initialLabel : originalInitialLabels) {
        if (workingNodes.count(initialLabel) > 0) {
            segmentManager.removeWorkingNode(workingNodes.at(initialLabel).get());
        }
        if (initialNodes.count(initialLabel) > 0) {
            segmentManager.removeInitialNode(initialLabel);
        }
    }
    if (profileOut != nullptr) {
        profileOut->removeOriginalNodesMs = durationMs(removeOriginalNodesStart, Clock::now());
    }

    const auto clearTargetRegionStart = Clock::now();
    for (const auto &voxel : targetVoxels) {
        workingSegmentsBuffer[workingSegmentsLinearIndex(voxel)] = backgroundId;
    }
    if (profileOut != nullptr) {
        profileOut->clearTargetRegionMs = durationMs(clearTargetRegionStart, Clock::now());
    }

    std::vector<std::vector<SegmentIdType>> replacementInitialLabelsByComponent(
        static_cast<std::size_t>(componentCount));
    std::vector<std::size_t> replacementVoxelCountsByComponent(
        static_cast<std::size_t>(componentCount), 0);
    std::vector<SegmentIdType> replacementInitialLabels;
    replacementInitialLabels.reserve(replacementInitialComponents.size());

    const auto createReplacementInitialsStart = Clock::now();
    double materializeReplacementVoxelListsMs = 0.0;
    for (auto &replacementComponent : replacementInitialComponents) {
        const SegmentIdType replacementInitialLabel = nextFreeId;
        ++nextFreeId;

        auto *replacementInitialNode =
            new InitialNode(graphBase, graphBase->pWorkingSegmentsImage, replacementInitialLabel);
        replacementInitialNode->voxels.reserve(replacementComponent.voxelIndices.size());
        const auto materializeReplacementVoxelListStart = Clock::now();
        for (const int voxelIndex : replacementComponent.voxelIndices) {
            replacementInitialNode->voxels.push_back(targetVoxels[static_cast<std::size_t>(voxelIndex)]);
        }
        materializeReplacementVoxelListsMs +=
            durationMs(materializeReplacementVoxelListStart, Clock::now());
        replacementInitialNode->roi.updateBoundingRoi(replacementInitialNode->voxels);
        segmentManager.addInitialNode(replacementInitialNode);

        replacementInitialLabelsByComponent[static_cast<std::size_t>(replacementComponent.finalComponentId)].push_back(
            replacementInitialLabel);
        replacementVoxelCountsByComponent[static_cast<std::size_t>(replacementComponent.finalComponentId)] +=
            replacementComponent.voxelIndices.size();
        replacementInitialLabels.push_back(replacementInitialLabel);

        for (const auto &voxel : replacementInitialNode->voxels) {
            workingSegmentsBuffer[workingSegmentsLinearIndex(voxel)] = replacementInitialLabel;
        }
    }
    if (profileOut != nullptr) {
        profileOut->createReplacementInitialsMs = durationMs(createReplacementInitialsStart, Clock::now());
        profileOut->materializeReplacementVoxelListsMs = materializeReplacementVoxelListsMs;
    }

    const auto recomputeInitialEdgesStart = Clock::now();
    for (const SegmentIdType initialLabel : replacementInitialLabels) {
        segmentManager.computeOneSidedEdgesOnInitialNode(initialNodes.at(initialLabel).get());
    }
    for (const SegmentIdType initialLabel : replacementInitialLabels) {
        segmentManager.computeCorrospondingOneSidedInitialEdges(initialNodes.at(initialLabel).get());
    }
    segmentManager.buildTwoSidedInitialEdgesFromOneSidedInitialEdges();
    if (profileOut != nullptr) {
        profileOut->recomputeInitialEdgesMs = durationMs(recomputeInitialEdgesStart, Clock::now());
    }

    std::unordered_set<SegmentIdType> touchedWorkingLabels;
    touchedWorkingLabels.reserve(neighborGroups.size() + replacementInitialLabelsByComponent.size() + 4);

    const auto rebuildWorkingNodesStart = Clock::now();
    for (const auto &neighborGroup : neighborGroups) {
        if (neighborGroup.initialLabels.size() > 1) {
            for (const SegmentIdType initialLabel : neighborGroup.initialLabels) {
                if (workingNodes.count(initialLabel) > 0) {
                    segmentManager.removeWorkingNode(workingNodes.at(initialLabel).get());
                }
            }
            auto *rebuiltNeighborNode =
                new WorkingNode(neighborGroup.initialLabels, neighborGroup.workingLabel, initialNodes);
            segmentManager.addWorkingNode(rebuiltNeighborNode);
        }
        touchedWorkingLabels.insert(neighborGroup.workingLabel);
    }

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
    resultingWorkingLabels.reserve(componentOrder.size());
    for (std::size_t orderIndex = 0; orderIndex < componentOrder.size(); ++orderIndex) {
        const int componentId = componentOrder[orderIndex];
        auto &componentLabels = replacementInitialLabelsByComponent[static_cast<std::size_t>(componentId)];
        if (componentLabels.empty()) {
            continue;
        }

        const SegmentIdType workingLabel =
            orderIndex == 0 ? targetWorkingLabel : nextFreeId++;
        auto *replacementWorkingNode =
            new WorkingNode(componentLabels, workingLabel, initialNodes);
        segmentManager.addWorkingNode(replacementWorkingNode);
        touchedWorkingLabels.insert(workingLabel);
        resultingWorkingLabels.push_back(workingLabel);
    }
    if (profileOut != nullptr) {
        profileOut->rebuildWorkingNodesMs = durationMs(rebuildWorkingNodesStart, Clock::now());
    }

    const auto recalculateWorkingEdgesStart = Clock::now();
    for (const SegmentIdType workingLabel : touchedWorkingLabels) {
        if (workingNodes.count(workingLabel) == 0) {
            continue;
        }
        segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(workingLabel).get());
    }
    if (profileOut != nullptr) {
        profileOut->recalculateWorkingEdgesMs = durationMs(recalculateWorkingEdgesStart, Clock::now());
    }

    const auto rewriteWorkingImageStart = Clock::now();
    for (const SegmentIdType workingLabel : touchedWorkingLabels) {
        if (workingNodes.count(workingLabel) == 0) {
            continue;
        }
        insertWorkingNodeInSegmentImage(*workingNodes.at(workingLabel));
    }
    if (profileOut != nullptr) {
        profileOut->rewriteWorkingImageMs = durationMs(rewriteWorkingImageStart, Clock::now());
    }

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
            overlapMap[node.first].compute(node.second->voxels);
//            std::cout << "label: " << overlapMap[node.first].corrospondingLabelWithMostOverlap << " " <<
//                      overlapMap[node.first].overlapPercentage << "\n";
        }

        ScopedGraphTimer mergeTimer(verbose, __func__, QStringLiteral("Collecting refinement-driven edge merges"));
//        get vec of edges to merge to then call mergeEdges on all of them
        std::set<EdgeNumIdType> edgesToMerge;
        for (auto &edge : initialLabelPairToTwoSidedInitialEdge) {
            dataType::SegmentIdType labelA = edge.first.first;
            dataType::SegmentIdType labelB = edge.first.second;
            double overlapLabelA = overlapMap[labelA].overlapPercentage;
            double overlapLabelB = overlapMap[labelB].overlapPercentage;
            long int corrospondingLabelA = overlapMap[labelA].corrospondingLabelWithMostOverlap;
            long int corrospondingLabelB = overlapMap[labelB].corrospondingLabelWithMostOverlap;

            bool bothPointToSameLabel = corrospondingLabelA == corrospondingLabelB;
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

void Graph::transferSegmentsWithRefinementOverlap() {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring segments by refinement overlap"));

    if (graphBase->pSelectedSegmentation != nullptr) {
        if (graphBase->pSelectedRefinement != nullptr) {
            double overlapThreshold = 0.7;
            for (auto &node : workingNodes) {
                LabelOverlap overlapFeature = LabelOverlap();
                overlapFeature.setPToOtherLabelImagePointer(graphBase->pSelectedRefinement);
                overlapFeature.compute(node.second->getVoxelLists());
                logGraphDebugIf(verbose,
                                __func__,
                                QStringLiteral("Working node %1 overlap=%2")
                                    .arg(node.first)
                                    .arg(QString::number(overlapFeature.values[0], 'g', 6)));
                if (overlapFeature.values[0] > overlapThreshold) {
                    if (overlapFeature.corrospondingLabelWithMostOverlap != backgroundId) {
                        transferWorkingNodeToSegmentation(node.first);
                    }
                }
            }
        }
    }
}

void Graph::transferSegmentsWithVolumeCriterion(double volumeThreshold) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Transferring segments by volume threshold"));
    if (graphBase->pSelectedSegmentation != nullptr) {
        NumberOfVoxels volumeFeature = NumberOfVoxels();
        for (auto &node : workingNodes) {
            volumeFeature.compute(node.second->getVoxelLists());
            logGraphDebugIf(verbose,
                            __func__,
                            QStringLiteral("Working node %1 volume=%2")
                                .arg(node.first)
                                .arg(QString::number(volumeFeature.values[0], 'g', 6)));
            if (volumeFeature.values[0] > volumeThreshold) {
                transferWorkingNodeToSegmentation(node.first);
            }
        }
    }
}

void Graph::setBackgroundIdStrategy(const std::string& backgroundIdStrategyIn) {
    backgroundIdStrategy = backgroundIdStrategyIn;
    logGraphDebugIf(verbose,
                    __func__,
                    QStringLiteral("Background id strategy set to %1")
                        .arg(QString::fromStdString(backgroundIdStrategy)));
}

void Graph::updateBackgroundIdFromVolume(SegmentsImageType::Pointer pImage) {
    logGraphDebugIf(verbose,
                    __func__,
                    QStringLiteral("Resolving background id using strategy %1")
                        .arg(QString::fromStdString(backgroundIdStrategy)));
    if (backgroundIdStrategy == "backgroundIsHighestId") {
        backgroundId = getLargestSegmentId(pImage);
    } else if (backgroundIdStrategy == "backgroundIsLowestId") {
        backgroundId = getSmallestSegmentId(pImage);
    } else {
        throw std::invalid_argument("Received unknown backgroundIdStrategy in Graph::updateBackgroundIdFromVolume");
    }
    logGraphDebugIf(verbose, __func__, QStringLiteral("Background id resolved to %1").arg(backgroundId));
}

void Graph::exportDebugInformation(){
    logGraph(LogLevel::Info, __func__, QStringLiteral("Exporting graph debug information"), kIoCategory);
    printEdgeIdLookUpToFile("edgeIdLookup.txt");
    printWorkingNodesToFile("workingNodes.txt");
    printWorkingEdgesToFile("workingEdges.txt");
    printInitialNodesToFile("initialNodes.txt");
    printInitialTwoSidedEdgesToFile("initialTwoSidedEdges.txt");
    printInitialOneSidedEdgesToFile("initialOneSidedEdges.txt");
    ITKImageWriter<dataType::EdgeImageType>(graphBase->pEdgesInitialSegmentsImage,
                                                                "initialEdges.nrrd");
    ITKImageWriter<dataType::SegmentsImageType>(graphBase->pWorkingSegmentsImage,
                                                                    "workingSegments.nrrd");
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
    itk::ImageRegionIterator<SegmentsImageType> it(
        graphBase->pSelectedSegmentation,
        graphBase->pSelectedSegmentation->GetLargestPossibleRegion());

    if (labels.size() == 1) {
        const SegmentIdType label = *labels.begin();
        if (label != backgroundId) {
            for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
                if (it.Get() == label) {
                    it.Set(backgroundId);
                    ++deletedVoxelCount;
                }
            }
        }
    } else {
        for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
            const SegmentIdType label = it.Get();
            if (label != backgroundId && labels.count(label) > 0) {
                it.Set(backgroundId);
                ++deletedVoxelCount;
            }
        }
    }

    if (deletedVoxelCount > 0) {
        graphBase->pSelectedSegmentation->Modified();
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
    std::unordered_set<SegmentIdType> ignoredLabels{backgroundId};
    if (pIgnoredSegmentLabels != nullptr) {
        ignoredLabels.insert(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end());
    }

    std::unordered_map<SegmentIdType, std::size_t> voxelCountByLabel;
    const auto voxelCount = graphBase->pSelectedSegmentation
                                ->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *buffer = graphBase->pSelectedSegmentation->GetBufferPointer();
    if (buffer == nullptr) {
        return labels;
    }

    for (std::size_t index = 0; index < voxelCount; ++index) {
        const SegmentIdType label = buffer[index];
        if (ignoredLabels.count(label) > 0) {
            continue;
        }
        auto &count = voxelCountByLabel[label];
        if (count < exclusiveVoxelThreshold) {
            ++count;
        }
    }

    labels.reserve(voxelCountByLabel.size());
    for (const auto &[label, count] : voxelCountByLabel) {
        if (count < exclusiveVoxelThreshold) {
            labels.push_back(label);
        }
    }
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
    if (initialSelectedLabels.count(backgroundId) > 0) {
        return finish(MergeStatus::Failed,
                      QStringLiteral("The background label cannot be merged with a neighbor."));
    }

    using segment_puzzler::connected_components::ConnectedComponentSplitOptions;
    using segment_puzzler::connected_components::ConnectedComponentSplitStats;
    using segment_puzzler::connected_components::ConnectivityStencil;
    using segment_puzzler::connected_components::countConnectedComponentsByLabelInRegions;
    using segment_puzzler::connected_components::splitDisconnectedLabelComponentsInPlace;

    std::vector<SegmentIdType> activeRequestedLabels(requestedLabels);
    std::unordered_set<SegmentIdType> ignoredLabels(
        pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end());
    ignoredLabels.insert(backgroundId);
    std::size_t totalDisconnectedLabelCount = 0;
    std::size_t totalDisconnectedRegionCount = 0;
    NeighborMergePlan plan;

    while (true) {
        QElapsedTimer planScanTimer;
        planScanTimer.start();
        plan = buildNeighborMergePlan(
            graphBase->pSelectedSegmentation,
            backgroundId,
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
            std::max(graphBase->selectedSegmentationMaxSegmentId, plan.actualMaximumLabel);
        const std::size_t newLabelCount = disconnectedRegionCount - disconnectedLabels.size();
        if (newLabelCount >
            std::numeric_limits<SegmentIdType>::max() - static_cast<std::uintmax_t>(effectiveMaximum)) {
            return finish(MergeStatus::Failed,
                          QStringLiteral("No free Selected Segmentation labels remain for connected components."));
        }

        ConnectedComponentSplitOptions splitOptions;
        splitOptions.connectivity = ConnectivityStencil::SixConnected;
        splitOptions.nextFreeLabel = static_cast<SegmentIdType>(effectiveMaximum + 1);
        splitOptions.ignoredLabels.insert(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end());
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

        graphBase->selectedSegmentationMaxSegmentId =
            std::max(graphBase->selectedSegmentationMaxSegmentId, splitStats.maxLabel);
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
    const SegmentIdType actualMaximumLabel = plan.actualMaximumLabel;
    if (graphBase->pSelectedSegmentation->GetBufferPointer() == nullptr
        || graphBase->pWorkingSegmentsImage->GetBufferPointer() == nullptr) {
        return finish(MergeStatus::Failed, QStringLiteral("A segmentation image has no voxel buffer."));
    }

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

    const SegmentIdType effectiveSelectedMaximum =
        std::max(graphBase->selectedSegmentationMaxSegmentId, actualMaximumLabel);
    if (plannedRoots.size() >
        maximumLabel - static_cast<std::uintmax_t>(effectiveSelectedMaximum)) {
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
    graphBase->selectedSegmentationMaxSegmentId = effectiveSelectedMaximum;
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
    if (selectedLabel == backgroundId) {
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

std::set<Graph::SegmentIdType> Graph::synchronizeOverwrittenInitialNodeVoxels(
        const std::set<SegmentIdType> &overwrittenLabels) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Synchronizing overwritten initial nodes"));
    std::set<SegmentIdType> absentLabels;
    if (overwrittenLabels.empty()) {
        return absentLabels;
    }
    if (graphBase == nullptr || graphBase->pWorkingSegmentsImage == nullptr) {
        throw std::logic_error("Cannot synchronize nodes without a working segments image.");
    }

    const auto workingImage = graphBase->pWorkingSegmentsImage;
    const auto workingRegion = workingImage->GetLargestPossibleRegion();
    std::unordered_set<SegmentIdType> labelsNeedingLatticeScan;
    labelsNeedingLatticeScan.reserve(overwrittenLabels.size());

    for (const SegmentIdType label : overwrittenLabels) {
        const auto initialNodeIt = initialNodes.find(label);
        if (initialNodeIt == initialNodes.end() || initialNodeIt->second == nullptr) {
            throw std::logic_error("An overwritten working label has no matching initial node.");
        }

        InitialNode *initialNode = initialNodeIt->second.get();
        std::vector<Voxel> filteredVoxels;
        filteredVoxels.reserve(initialNode->voxels.size());
        for (const Voxel &voxel : initialNode->voxels) {
            const SegmentsImageType::IndexType index{{voxel.x, voxel.y, voxel.z}};
            if (workingRegion.IsInside(index) && workingImage->GetPixel(index) == label) {
                filteredVoxels.push_back(voxel);
            }
        }
        initialNode->voxels = std::move(filteredVoxels);
        if (initialNode->voxels.empty()) {
            labelsNeedingLatticeScan.insert(label);
        }
    }

    if (labelsNeedingLatticeScan.empty()) {
        return absentLabels;
    }

    itk::ImageRegionConstIterator<SegmentsImageType> imageIterator(workingImage, workingRegion);
    for (imageIterator.GoToBegin(); !imageIterator.IsAtEnd(); ++imageIterator) {
        const SegmentIdType label = imageIterator.Get();
        if (labelsNeedingLatticeScan.count(label) == 0) {
            continue;
        }
        const auto index = imageIterator.GetIndex();
        initialNodes.at(label)->voxels.emplace_back(
            static_cast<int>(index[0]),
            static_cast<int>(index[1]),
            static_cast<int>(index[2]));
    }

    for (const SegmentIdType label : labelsNeedingLatticeScan) {
        if (initialNodes.at(label)->voxels.empty()) {
            absentLabels.insert(label);
            continue;
        }
        logGraph(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Recovered label %1 from lattice after its voxel metadata became empty")
                .arg(label));
    }
    return absentLabels;
}

std::optional<Graph::SegmentIdType> Graph::refineFromImageAtPosition(
        const SegmentsImageType::Pointer &sourceImage,
        const SegmentsImageType::IndexType &seed,
        const std::optional<SegmentsImageType::RegionType> &permittedSeedRegion) {
    ScopedGraphTimer timer(verbose, __func__, QStringLiteral("Refining working graph from image component"));
    if (graphBase == nullptr || sourceImage == nullptr ||
        graphBase->pWorkingSegmentsImage == nullptr || pIgnoredSegmentLabels == nullptr) {
        logGraph(LogLevel::Warning, __func__, QStringLiteral("Refinement and working images must be initialized"));
        return std::nullopt;
    }
    if (graphBase->pWorkingSegmentsImage->GetBufferPointer() == nullptr) {
        logGraph(LogLevel::Error, __func__, QStringLiteral("Working segments image is not allocated"));
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
    if (sourceLabel == backgroundId) {
        logGraph(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Refinement label matches background label %1; insertion skipped")
                .arg(backgroundId));
        return std::nullopt;
    }
    if (nextFreeId == std::numeric_limits<SegmentIdType>::max()) {
        logGraph(LogLevel::Error, __func__, QStringLiteral("No fresh segment label remains for refinement"));
        return std::nullopt;
    }

    const SegmentIdType labelToInsert = nextFreeId;
    if (labelToInsert == backgroundId || isIgnoredId(labelToInsert) ||
        initialNodes.count(labelToInsert) > 0 || workingNodes.count(labelToInsert) > 0) {
        logGraph(
            LogLevel::Error,
            __func__,
            QStringLiteral("Next free label %1 is reserved or already present in the graph")
                .arg(labelToInsert));
        return std::nullopt;
    }

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
    const SegmentsImageType::RegionType dilatedRegion = getDilatedRegionFromRoi(
        pendingInitialNode->roi,
        workingRegion,
        2);

    std::set<SegmentIdType> affectedWorkingLabels;
    itk::ImageRegionConstIterator<SegmentsImageType> regionIterator(
        graphBase->pWorkingSegmentsImage,
        dilatedRegion);
    for (regionIterator.GoToBegin(); !regionIterator.IsAtEnd(); ++regionIterator) {
        const SegmentIdType label = regionIterator.Get();
        if (!isIgnoredId(label)) {
            affectedWorkingLabels.insert(label);
        }
    }

    // Preserve the existing transitive neighbor expansion, but validate the
    // entire closure before changing the graph.
    for (auto labelIt = affectedWorkingLabels.begin();
         labelIt != affectedWorkingLabels.end();
         ++labelIt) {
        const auto workingNodeIt = workingNodes.find(*labelIt);
        if (workingNodeIt == workingNodes.end() || workingNodeIt->second == nullptr) {
            logGraph(
                LogLevel::Error,
                __func__,
                QStringLiteral("Segment %1 is missing from workingNodes during refinement")
                    .arg(*labelIt));
            return std::nullopt;
        }
        if (workingNodeIt->second->subInitialNodes.empty()) {
            logGraph(
                LogLevel::Error,
                __func__,
                QStringLiteral("Working segment %1 contains no initial nodes").arg(*labelIt));
            return std::nullopt;
        }
        for (const auto &initialEntry : workingNodeIt->second->subInitialNodes) {
            const auto initialNodeIt = initialNodes.find(initialEntry.first);
            if (initialEntry.second == nullptr || initialNodeIt == initialNodes.end() ||
                initialNodeIt->second == nullptr) {
                logGraph(
                    LogLevel::Error,
                    __func__,
                    QStringLiteral("Working segment %1 references missing initial node %2")
                        .arg(*labelIt)
                        .arg(initialEntry.first));
                return std::nullopt;
            }
        }
        for (const auto &edgeEntry : workingNodeIt->second->neighborLabelToWorkingEdge) {
            if (edgeEntry.second == nullptr) {
                logGraph(
                    LogLevel::Error,
                    __func__,
                    QStringLiteral("Working segment %1 has a null edge to segment %2")
                        .arg(*labelIt)
                        .arg(edgeEntry.first));
                return std::nullopt;
            }
            if (!isIgnoredId(edgeEntry.first)) {
                affectedWorkingLabels.insert(edgeEntry.first);
            }
        }
    }

    logGraph(
        LogLevel::Info,
        __func__,
        QStringLiteral("Inserting refinement-derived initial node %1").arg(labelToInsert));
    ++nextFreeId;
    segmentManager.addInitialNode(pendingInitialNode.release());
    InitialNode *newInitialNode = initialNodes.at(labelToInsert).get();

    {
        ScopedGraphTimer splitTimer(verbose, __func__, QStringLiteral("Splitting affected working nodes"));
        for (const SegmentIdType label : affectedWorkingLabels) {
            splitWorkingNodeIntoInitialNodes(label);
        }
    }

    std::set<SegmentIdType> overwrittenInitialLabels;
    for (const Voxel &voxel : newInitialNode->voxels) {
        const SegmentsImageType::IndexType index{{voxel.x, voxel.y, voxel.z}};
        const SegmentIdType overwrittenLabel = graphBase->pWorkingSegmentsImage->GetPixel(index);
        if (!isIgnoredId(overwrittenLabel)) {
            overwrittenInitialLabels.insert(overwrittenLabel);
        }
        graphBase->pWorkingSegmentsImage->SetPixel(index, labelToInsert);
    }
    const auto labelsToDelete = synchronizeOverwrittenInitialNodeVoxels(overwrittenInitialLabels);
    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("Overwritten initial nodes=[%1], deleting absent nodes=[%2]")
            .arg(joinIds(overwrittenInitialLabels))
            .arg(joinIds(labelsToDelete)));

    for (const SegmentIdType label : labelsToDelete) {
        const auto workingNodeIt = workingNodes.find(label);
        if (workingNodeIt != workingNodes.end() && workingNodeIt->second != nullptr) {
            segmentManager.removeWorkingNode(workingNodeIt->second.get());
        } else {
            logGraph(
                LogLevel::Warning,
                __func__,
                QStringLiteral("Proven-absent label %1 has no WorkingNode to remove").arg(label));
        }
        const auto initialNodeIt = initialNodes.find(label);
        if (initialNodeIt != initialNodes.end() && initialNodeIt->second != nullptr) {
            segmentManager.removeInitialNode(label);
        } else {
            logGraph(
                LogLevel::Warning,
                __func__,
                QStringLiteral("Proven-absent label %1 has no InitialNode to remove").arg(label));
        }
    }

    {
        ScopedGraphTimer recomputeTimer(
            verbose,
            __func__,
            QStringLiteral("Recomputing affected graph neighborhoods"));

        std::vector<SegmentIdType> survivingOverwrittenLabels;
        survivingOverwrittenLabels.reserve(overwrittenInitialLabels.size());
        std::vector<InitialNode *> initialNodesToRecompute;
        initialNodesToRecompute.reserve(overwrittenInitialLabels.size() + 1);
        for (const SegmentIdType label : overwrittenInitialLabels) {
            if (labelsToDelete.count(label) > 0) {
                continue;
            }

            InitialNode *initialNode = initialNodes.at(label).get();
            segmentManager.removeEdgePropertiesOnInitialNode(initialNode);
            initialNode->roi.updateBoundingRoi(initialNode->voxels);
            initialNode->calculateNodeFeatures();
            workingNodes.at(label)->roi = initialNode->roi;
            survivingOverwrittenLabels.push_back(label);
            initialNodesToRecompute.push_back(initialNode);
        }

        newInitialNode->calculateNodeFeatures();
        initialNodesToRecompute.push_back(newInitialNode);
        for (InitialNode *initialNode : initialNodesToRecompute) {
            segmentManager.computeOneSidedEdgesOnInitialNode(initialNode);
        }
        for (InitialNode *initialNode : initialNodesToRecompute) {
            segmentManager.computeCorrospondingOneSidedInitialEdges(initialNode);
        }
        segmentManager.buildTwoSidedInitialEdgesFromOneSidedInitialEdges();

        auto *newWorkingNode = new WorkingNode(newInitialNode, labelToInsert, initialNodes);
        segmentManager.addWorkingNode(newWorkingNode);
        for (const SegmentIdType label : survivingOverwrittenLabels) {
            segmentManager.recalculateEdgesOnWorkingNode(workingNodes.at(label).get());
        }
        segmentManager.recalculateEdgesOnWorkingNode(newWorkingNode);
    }

    const auto insertedNodeIt = workingNodes.find(labelToInsert);
    if (graphBase->pWorkingSegmentsImage->GetPixel(seed) != labelToInsert ||
        insertedNodeIt == workingNodes.end() || insertedNodeIt->second == nullptr) {
        logGraph(
            LogLevel::Error,
            __func__,
            QStringLiteral("Refinement did not create expected working label %1 at the clicked point")
                .arg(labelToInsert));
        return std::nullopt;
    }
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
        return refineFromImageAtPosition(graphBase->pSelectedSegmentation, seed, std::nullopt);
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
Graph::transferWorkingNodesToSegmentation(const std::vector<SegmentIdType> &workingLabelsToTransfer) {
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
    const auto selectedRegion = graphBase->pSelectedSegmentation->GetLargestPossibleRegion();

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
        for (const auto &voxelList : workingNodeIt->second->getVoxelLists()) {
            if (voxelList == nullptr) {
                logGraph(LogLevel::Error,
                         __func__,
                         QStringLiteral("Working label %1 contains a null voxel list").arg(workingLabel));
                return {};
            }
            for (const Voxel &voxel : *voxelList) {
                SegmentsImageType::IndexType index{{voxel.x, voxel.y, voxel.z}};
                if (!selectedRegion.IsInside(index)) {
                    logGraph(LogLevel::Error,
                             __func__,
                             QStringLiteral("Working label %1 lies outside the selected segmentation")
                                 .arg(workingLabel));
                    return {};
                }
            }
        }
        nodesToTransfer.push_back(workingNodeIt->second);
    }

    if (nodesToTransfer.empty()) {
        return assignedSegmentationLabels;
    }

    const SegmentIdType currentMaxLabel = graphBase->selectedSegmentationMaxSegmentId;
    const auto availableLabels =
        static_cast<std::uintmax_t>(std::numeric_limits<SegmentIdType>::max()) -
        static_cast<std::uintmax_t>(currentMaxLabel);
    if (nodesToTransfer.size() > availableLabels) {
        logGraph(LogLevel::Error,
                 __func__,
                 QStringLiteral("Not enough free labels remain in the selected segmentation"));
        return assignedSegmentationLabels;
    }

    assignedSegmentationLabels.reserve(nodesToTransfer.size());
    for (std::size_t nodeIndex = 0; nodeIndex < nodesToTransfer.size(); ++nodeIndex) {
        assignedSegmentationLabels.push_back(
            static_cast<SegmentIdType>(currentMaxLabel + static_cast<SegmentIdType>(nodeIndex + 1)));
    }

    for (std::size_t nodeIndex = 0; nodeIndex < nodesToTransfer.size(); ++nodeIndex) {
        const SegmentIdType segmentationLabel = assignedSegmentationLabels[nodeIndex];
        for (const auto &voxelList : nodesToTransfer[nodeIndex]->getVoxelLists()) {
            for (const Voxel &voxel : *voxelList) {
                graphBase->pSelectedSegmentation->SetPixel({voxel.x, voxel.y, voxel.z}, segmentationLabel);
            }
        }
    }

    graphBase->pSelectedSegmentation->Modified();
    graphBase->selectedSegmentationMaxSegmentId = assignedSegmentationLabels.back();
    logGraphDebugIf(
        verbose,
        __func__,
        QStringLiteral("Transferred working labels=[%1] to segmentation labels=[%2]")
            .arg(joinIds(workingLabelsToTransfer))
            .arg(joinIds(assignedSegmentationLabels)));
    return assignedSegmentationLabels;
}




// === print functions ===

void Graph::printInitialNodes(std::ostream &outStream) {
    outStream << "=== initialNodes ===\n";
    int nodeIndentationLevel = 1;
    for (auto &node : initialNodes) {
        outStream << "key: " << node.first << "\n";
        node.second->print(nodeIndentationLevel, outStream);
    }
}

void Graph::printInitialOneSidedEdges(std::ostream &outStream) {
    outStream << "=== initialOneSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (const auto &[sourceLabel, initialNode] : initialNodes) {
        for (const auto &[neighborLabel, edge] : initialNode->neighborLabelToOneSidedInitialEdge) {
            outStream << "key: " << sourceLabel << "," << neighborLabel << "\n";
            edge->print(nodeIndentationLevel, outStream);
        }
    }
}

void Graph::printInitialTwoSidedEdges(std::ostream &outStream) {
    outStream << "=== initialTwoSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : initialLabelPairToTwoSidedInitialEdge) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        edge.second->print(nodeIndentationLevel, outStream);
    }
}

void Graph::printWorkingNodes(std::ostream &outStream) {
    outStream << "=== workingNodes ===\n";
    int nodeIndentationLevel = 1;
    for (auto &node : workingNodes) {
        outStream << "key: " << node.first << "\n";
        node.second->print(nodeIndentationLevel, outStream);
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
        edge.second->print(nodeIndentationLevel, outStream);
    }
}

// == wrappers for writing to file ==


void Graph::printInitialNodesToFile(const std::string &pathToOutputfile) {
    logGraph(LogLevel::Info,
             __func__,
             QStringLiteral("Writing initial nodes to %1").arg(QString::fromStdString(pathToOutputfile)),
             kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printInitialNodes(outFile);
    outFile.close();
}

void Graph::printInitialTwoSidedEdgesToFile(const std::string &pathToOutputfile) {
    logGraph(LogLevel::Info,
             __func__,
             QStringLiteral("Writing initial two-sided edges to %1").arg(QString::fromStdString(pathToOutputfile)),
             kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printInitialTwoSidedEdges(outFile);
    outFile.close();
}

void Graph::printInitialOneSidedEdgesToFile(const std::string &pathToOutputfile) {
    logGraph(LogLevel::Info,
             __func__,
             QStringLiteral("Writing initial one-sided edges to %1").arg(QString::fromStdString(pathToOutputfile)),
             kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printInitialOneSidedEdges(outFile);
    outFile.close();
}

void Graph::printWorkingNodesToFile(const std::string &pathToOutputfile) {
    logGraph(LogLevel::Info,
             __func__,
             QStringLiteral("Writing working nodes to %1").arg(QString::fromStdString(pathToOutputfile)),
             kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printWorkingNodes(outFile);
    outFile.close();
}

void Graph::printEdgeIdLookUpToFile(const std::string &pathToOutputfile) {
    logGraph(LogLevel::Info,
             __func__,
             QStringLiteral("Writing edge-id lookup to %1").arg(QString::fromStdString(pathToOutputfile)),
             kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printEdgeIdLookUp(outFile);
    outFile.close();
}

void Graph::printWorkingEdgesToFile(const std::string &pathToOutputfile) {
    logGraph(LogLevel::Info,
             __func__,
             QStringLiteral("Writing working edges to %1").arg(QString::fromStdString(pathToOutputfile)),
             kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printWorkingEdges(outFile);
    outFile.close();
}
