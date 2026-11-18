#include "src/image_storage/LabelImageStorage.h"
#include "src/utils/WorkerThreads.h"


#include "SegmentManager.h"
#include "LabelContacts.h"
#include <itkMultiThreaderBase.h>
#include <QElapsedTimer>
#include "src/utils/AppLogger.h"
#include <src/utils/utils.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#ifdef USE_OMP
#include <omp.h>
#endif

namespace {

using segment_puzzler::app_logging::AppLogger;
using segment_puzzler::app_logging::LogLevel;

const QString kSegmentationCategory = QStringLiteral("segmentation");
const QString kIoCategory = QStringLiteral("io");

void logSegmentManager(LogLevel level, const char *functionName, const QString &message, const QString &category = kSegmentationCategory) {
    AppLogger::log(level, category, message, functionName);
}

void logSegmentManagerDebugIf(bool enabled, const char *functionName, const QString &message) {
    if (!enabled) {
        return;
    }
    logSegmentManager(LogLevel::Debug, functionName, message);
}

struct TwoSidedInitialEdgeSources {
    dataType::EdgePairIdType labelPair;
    std::shared_ptr<OneSidedInitialEdge> smallerLabelOneSidedInitialEdge;
    std::shared_ptr<OneSidedInitialEdge> largerLabelOneSidedInitialEdge;
};

std::unique_ptr<TwoSidedInitialEdge> buildTwoSidedInitialEdge(
        const TwoSidedInitialEdgeSources &sources) {
    auto twoSidedInitialEdge = std::make_unique<TwoSidedInitialEdge>(
        sources.smallerLabelOneSidedInitialEdge,
        sources.largerLabelOneSidedInitialEdge);
    if (twoSidedInitialEdge->pairId != sources.labelPair) {
        throw std::logic_error("Two-sided initial-edge sources do not match their label pair.");
    }
    return twoSidedInitialEdge;
}

class ScopedSegmentManagerTimer {
public:
    ScopedSegmentManagerTimer(bool enabled, const char *functionName, QString operation, QString category = kSegmentationCategory)
        : enabled_(enabled), functionName_(functionName), operation_(std::move(operation)), category_(std::move(category)) {
        if (!enabled_) {
            return;
        }
        AppLogger::log(LogLevel::Debug, category_, operation_ + QStringLiteral(" started"), functionName_);
        timer_.start();
    }

    ~ScopedSegmentManagerTimer() {
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

} // namespace

// === init ===


void SegmentManager::setPointerToIgnoredSegmentsLabels(std::vector<SegmentIdType> *pIgnoredSegmentLabelsIn) {
    pIgnoredSegmentLabels = pIgnoredSegmentLabelsIn;
}




// ==== add =====


void SegmentManager::addInitialNode(InitialNode *pInitialNodeToAdd) {
    (*pInitialNodes)[pInitialNodeToAdd->getLabel()] = std::shared_ptr<InitialNode>(pInitialNodeToAdd);

}

void SegmentManager::addInitialNode(SegmentIdType labelOfNewNode, std::size_t runCapacity) {
    auto pNewInitialNode = std::shared_ptr<InitialNode>(new InitialNode(graphBase, *ppWorkingSegmentsImage, labelOfNewNode));
    if (runCapacity > 0) {
        pNewInitialNode->voxels.reserveRuns(runCapacity);
    }
    (*pInitialNodes)[labelOfNewNode] = pNewInitialNode;

}


void SegmentManager::registerTwoSidedInitialEdge(
        std::unique_ptr<TwoSidedInitialEdge> edgeToRegister, bool updateImage) {
    if (edgeToRegister == nullptr) {
        throw std::invalid_argument("Cannot register a null two-sided initial edge.");
    }

    graphBase->edgeCounter++;
    size_t newEdgeId = graphBase->edgeCounter;
    edgeToRegister->setIdAndRegister(newEdgeId, *pInitialEdgeIdLookUp);
    auto registeredEdge = std::shared_ptr<TwoSidedInitialEdge>(std::move(edgeToRegister));
    pInitialNodes->at(registeredEdge->getLabelSmaller())->addTwoSidedInitialEdge(registeredEdge);
    pInitialNodes->at(registeredEdge->getLabelBigger())->addTwoSidedInitialEdge(registeredEdge);
    (*pInitialLabelPairToTwoSidedInitialEdge)[registeredEdge->pairId] = registeredEdge;

    pEdgeStatus->emplace(registeredEdge->numId, registeredEdge->getShouldMerge());
    if (updateImage) segment_puzzler::setVoxelLists(*graphBase->pEdgesInitialSegmentsImage,
        registeredEdge->getVoxelLists(), registeredEdge->numId);
}

void SegmentManager::addWorkingNode(WorkingNode *workingNodeToAdd) {
    (*pWorkingNodes)[workingNodeToAdd->getLabel()] = std::shared_ptr<WorkingNode>(workingNodeToAdd);
}


void SegmentManager::addWorkingEdge(WorkingEdge *pWorkingEdgeToAdd) {
    EdgePairIdType pairId = pWorkingEdgeToAdd->pairId;
    SegmentIdType labelA = pairId.first;
    SegmentIdType labelB = pairId.second;

    auto spWorkingEdgeToAdd = std::shared_ptr<WorkingEdge>(pWorkingEdgeToAdd);
    (*pWorkingLabelPairToWorkingEdge)[pairId] = spWorkingEdgeToAdd;


    pWorkingNodes->at(labelA)->neighborLabelToWorkingEdge[labelB] = spWorkingEdgeToAdd;
    pWorkingNodes->at(labelB)->neighborLabelToWorkingEdge[labelA] = spWorkingEdgeToAdd;
}


// ==== remove =====

void SegmentManager::removeInitialNode(SegmentManager::SegmentIdType labelOfNodeToRemove) {
    InitialNode *nodeToRemove = pInitialNodes->at(labelOfNodeToRemove).get();
    if (!nodeToRemove) throw std::logic_error("Cannot remove a null initial node.");
    ScopedSegmentManagerTimer timer(verbose,
                                    __func__,
                                    QStringLiteral("Removing initial node %1").arg(nodeToRemove->getLabel()));
    // take care of: initialtwosided edges, initial onesided edges, edgeidlookup, initialnodes, initialEdgeVolume, edgeStatus

    removeEdgePropertiesOnInitialNode(nodeToRemove);
    pInitialNodes->erase(nodeToRemove->getLabel());
}

void SegmentManager::removeEdgePropertiesOnInitialNode(InitialNode *pInitialNode) {

    int backgroundValueEdges = 0;
    for (auto initialEdge : pInitialNode->neighborLabelToTwoSidedInitialEdge) {
        // set voxels in the initial edge volume to background
        logSegmentManagerDebugIf(verbose,
                                 __func__,
                                 QStringLiteral("Removing voxels for initial edge %1").arg(initialEdge.second->numId));
        segment_puzzler::setVoxelLists(**ppEdgesInitialSegmentsImage,
            initialEdge.second->getVoxelLists(), backgroundValueEdges);

        // delete edge status
        if (pEdgeStatus->count(initialEdge.second->numId) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing edge status for initial edge %1").arg(initialEdge.second->numId));
            pEdgeStatus->erase(initialEdge.second->numId);
        }

        // take care of twosided edges
        if (pInitialLabelPairToTwoSidedInitialEdge->count(initialEdge.second->pairId) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing two-sided initial edge %1 -> %2")
                                         .arg(initialEdge.second->pairId.first)
                                         .arg(initialEdge.second->pairId.second));
            pInitialLabelPairToTwoSidedInitialEdge->erase(initialEdge.second->pairId);
        }
        // also delete from the paired node
        if (pInitialNodes->count(initialEdge.first) > 0) {
            if ((*pInitialNodes)[initialEdge.first]->neighborLabelToTwoSidedInitialEdge.count(pInitialNode->getLabel()) > 0) {
                logSegmentManagerDebugIf(verbose,
                                         __func__,
                                         QStringLiteral("Removing paired two-sided initial edge %1 -> %2")
                                             .arg(initialEdge.first)
                                             .arg(pInitialNode->getLabel()));
                (*pInitialNodes)[initialEdge.first]->neighborLabelToTwoSidedInitialEdge.erase(pInitialNode->getLabel());
            }
        }

        // delete corrosponding edgeidlookup
        if (pInitialEdgeIdLookUp->count(initialEdge.second->numId) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing edge-id lookup entry %1").arg(initialEdge.second->numId));
            pInitialEdgeIdLookUp->erase(initialEdge.second->numId);
        }
    }
    pInitialNode->neighborLabelToTwoSidedInitialEdge.clear();

    // delete onesided edges
    for (auto edge : pInitialNode->neighborLabelToOneSidedInitialEdge) {
        const auto &neighbor = pInitialNodes->at(edge.first);
        if (!neighbor) throw std::logic_error("Cannot remove an initial edge with a null neighbor.");
        if (neighbor->neighborLabelToOneSidedInitialEdge.count(pInitialNode->getLabel()) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing mirrored one-sided edge %1 -> %2")
                                         .arg(edge.first)
                                         .arg(pInitialNode->getLabel()));
            neighbor->neighborLabelToOneSidedInitialEdge.erase(pInitialNode->getLabel());
        }
    }
    pInitialNode->neighborLabelToOneSidedInitialEdge.clear();
}


void SegmentManager::clearGraphAndReserveInitialNodes(std::size_t initialNodeCapacity) {
    const std::size_t previousInitialNodeCount = pInitialNodes->size();
    std::size_t previousOneSidedEdgeCount = 0;
    for (const auto &initialNodeEntry : *pInitialNodes) {
        previousOneSidedEdgeCount += initialNodeEntry.second->neighborLabelToOneSidedInitialEdge.size();
    }
    const std::size_t previousTwoSidedEdgeCount = pInitialLabelPairToTwoSidedInitialEdge->size();
    const std::size_t previousWorkingNodeCount = pWorkingNodes->size();
    const std::size_t previousWorkingEdgeCount = pWorkingLabelPairToWorkingEdge->size();

    QElapsedTimer phaseTimer;
    phaseTimer.start();
    pInitialNodes->clear();
    const double clearInitialNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pInitialLabelPairToTwoSidedInitialEdge->clear();
    const double clearTwoSidedEdgesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pInitialEdgeIdLookUp->clear();
    const double clearEdgeIdLookupMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pWorkingNodes->clear();
    const double clearWorkingNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pWorkingLabelPairToWorkingEdge->clear();
    const double clearWorkingEdgesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    if (initialNodeCapacity > 0) {
        pInitialNodes->reserve(initialNodeCapacity);
    }
    const double reserveInitialNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    logSegmentManagerDebugIf(
        verbose,
        __func__,
        QStringLiteral(
            "Graph container reset previous_initial_nodes=%1 previous_one_sided_edges=%2 "
            "previous_two_sided_edges=%3 previous_working_nodes=%4 previous_working_edges=%5 "
            "clear_initial_nodes_ms=%6 clear_two_sided_edges_ms=%7 "
            "clear_edge_lookup_ms=%8 clear_working_nodes_ms=%9 clear_working_edges_ms=%10 "
            "requested_initial_node_capacity=%11 reserve_initial_nodes_ms=%12")
            .arg(static_cast<qulonglong>(previousInitialNodeCount))
            .arg(static_cast<qulonglong>(previousOneSidedEdgeCount))
            .arg(static_cast<qulonglong>(previousTwoSidedEdgeCount))
            .arg(static_cast<qulonglong>(previousWorkingNodeCount))
            .arg(static_cast<qulonglong>(previousWorkingEdgeCount))
            .arg(clearInitialNodesMs, 0, 'f', 3)
            .arg(clearTwoSidedEdgesMs, 0, 'f', 3)
            .arg(clearEdgeIdLookupMs, 0, 'f', 3)
            .arg(clearWorkingNodesMs, 0, 'f', 3)
            .arg(clearWorkingEdgesMs, 0, 'f', 3)
            .arg(static_cast<qulonglong>(initialNodeCapacity))
            .arg(reserveInitialNodesMs, 0, 'f', 3));
}


void SegmentManager::removeWorkingEdge(WorkingEdge *workingEdgeToRemove) {
    EdgePairIdType pairId = workingEdgeToRemove->pairId;
    SegmentIdType labelA = pairId.first;
    SegmentIdType labelB = pairId.second;

    if (pWorkingNodes->count(labelA) > 0) { // if workingNode exist
        if (pWorkingNodes->at(labelA)->neighborLabelToWorkingEdge.count(labelB) > 0) { // if twosided edge exists
            pWorkingNodes->at(labelA)->neighborLabelToWorkingEdge.erase(labelB); // delete it
        } else {
            logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Working node %1 has a dangling edge to %2").arg(labelA).arg(labelB));
        }
    } else {
        logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Removing edge with missing working node %1").arg(labelA));
    }

    if (pWorkingNodes->count(labelB) > 0) { // if workingNode exist
        if (pWorkingNodes->at(labelB)->neighborLabelToWorkingEdge.count(labelA) > 0) { // if twosided edge exists
            pWorkingNodes->at(labelB)->neighborLabelToWorkingEdge.erase(labelA); // delete it
        } else {
            logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Working node %1 has a dangling edge to %2").arg(labelB).arg(labelA));
        }
    } else {
        logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Removing edge with missing working node %1").arg(labelB));
    }

    pWorkingLabelPairToWorkingEdge->erase(pairId); // also delete it from working edges
}


void SegmentManager::removeInitialNodeFromWorkingNodeAtPosition(int x, int y, int z) {
    const auto owner = graphBase->pWorkingSegmentsImage->GetPixel({x, y, z});
    if (isIgnoredId(owner)) return;

    const auto original = pWorkingNodes->at(owner);
    if (original->subInitialNodes.size() == 1 && original->subInitialNodes.count(owner)) return;
    InitialNodeGroup remaining;
    std::optional<SegmentIdType> detached;
    for (const auto &[id, node] : original->subInitialNodes) {
        remaining.insert(id);
        if (detached) continue;
        for (const auto &run : node->voxels.getRuns()) {
            if (run.start.y == y && run.start.z == z && x >= run.start.x &&
                x < std::int64_t(run.start.x) + run.length) {
                detached = id;
                break;
            }
        }
    }
    if (!detached) throw std::logic_error("The clicked voxel has no initial node.");
    remaining.erase(*detached);
    WorkingNodeSplits splits;
    auto &groups = splits[owner];
    appendConnectedComponents(std::move(remaining), groups);
    groups.push_back({*detached});
    replaceWorkingGroups(splits, detached);

    for (const auto &[neighbor, edge] : pInitialNodes->at(*detached)->neighborLabelToTwoSidedInitialEdge) {
        if (original->subInitialNodes.count(neighbor) && static_cast<signed char>(pEdgeStatus->at(edge->numId)) > 0) {
            edge->setShouldMergeNotInformed();
            pEdgeStatus->at(edge->numId) = 0;
        }
    }
}

void SegmentManager::insertWorkingNodeIntoITKImage(WorkingNode *newWorkingNode) {
    segment_puzzler::setVoxelLists(**ppWorkingSegmentsImage, newWorkingNode->getVoxelLists(), newWorkingNode->getLabel());
}

void SegmentManager::splitWorkingNodeIntoInitialNodes(SegmentIdType workingNodeIdToSplit) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Splitting working node into initial nodes"));
    if (isIgnoredId(workingNodeIdToSplit)) return;
    const auto original = pWorkingNodes->at(workingNodeIdToSplit);
    removeWorkingNode(original.get());
    for (const auto &[id, initial] : original->subInitialNodes) {
        auto *node = new WorkingNode(initial.get(), id, *pInitialNodes);
        addWorkingNode(node);
        if (original->subInitialNodes.size() != 1 || id != workingNodeIdToSplit) insertWorkingNodeIntoITKImage(node);
    }
    for (const auto &[id, initial] : original->subInitialNodes) {
        recalculateEdgesOnWorkingNode(pWorkingNodes->at(id).get());
        for (const auto &[neighbor, edge] : initial->neighborLabelToTwoSidedInitialEdge) {
            pEdgeStatus->at(edge->numId) = 0;
            edge->setShouldMergeNotInformed();
        }
    }
}

void SegmentManager::appendConnectedComponents(InitialNodeGroup remaining, std::vector<InitialNodeGroup> &groups) const {
    while (!remaining.empty()) {
        std::vector<SegmentIdType> queue{*remaining.begin()};
        remaining.erase(remaining.begin());
        for (std::size_t i = 0; i < queue.size(); ++i) {
            for (const auto &[neighbor, edge] : pInitialNodes->at(queue[i])->neighborLabelToTwoSidedInitialEdge) {
                if (remaining.erase(neighbor)) queue.push_back(neighbor);
            }
        }
        groups.emplace_back(queue.begin(), queue.end());
    }
}

void SegmentManager::replaceWorkingGroups(const WorkingNodeSplits &splits,
                                          std::optional<SegmentIdType> detachedInitial) {
    auto cursor = *nextFreeId;
    std::vector<std::pair<SegmentIdType, const InitialNodeGroup *>> outputs;
    for (const auto &[oldLabel, groups] : splits) {
        for (const auto &group : groups) {
            const auto id = detachedInitial && group.size() == 1 && *group.begin() == *detachedInitial
                ? *detachedInitial : takeNextSegmentId(cursor);
            outputs.emplace_back(id, &group);
        }
    }

    *nextFreeId = cursor;
    for (const auto &[oldLabel, groups] : splits) removeWorkingNode(pWorkingNodes->at(oldLabel).get());
    for (const auto &[label, group] : outputs) {
        addWorkingNode(new WorkingNode(*group, label, *pInitialNodes));
    }
    for (const auto &[label, group] : outputs) {
        auto *node = pWorkingNodes->at(label).get();
        recalculateEdgesOnWorkingNode(node);
        insertWorkingNodeIntoITKImage(node);
    }
}

void SegmentManager::splitIntoConnectedComponentsOfWorkingNode(WorkingNode &node) {
    InitialNodeGroup members;
    for (const auto &[id, initial] : node.subInitialNodes) members.insert(id);
    WorkingNodeSplits splits;
    appendConnectedComponents(std::move(members), splits[node.getLabel()]);
    replaceWorkingGroups(splits);
}

void SegmentManager::computeOneSidedEdgesOnInitialNode(InitialNode *pInitialNode) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Computing one-sided edges for one initial node"));
    pInitialNode->computeOneSidedEdges(*pIgnoredSegmentLabels);
}

void SegmentManager::computeCorrospondingOneSidedInitialEdges(InitialNode *pInitialNode) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Computing corresponding one-sided initial edges"));
    const auto label = pInitialNode->getLabel();
    for (const auto &[neighborLabel, edge] : pInitialNode->neighborLabelToOneSidedInitialEdge) {
        const auto neighbor = pInitialNodes->find(neighborLabel);
        if (neighbor == pInitialNodes->end()) {
            const QString message = QStringLiteral("Connected initial node %1 is missing while rebuilding the edge from %2")
                                        .arg(neighborLabel).arg(label);
            logSegmentManager(LogLevel::Error, __func__, message);
            throw std::logic_error(message.toStdString());
        }
        if (!neighbor->second || !edge) {
            throw std::logic_error("Cannot rebuild an initial edge with a null endpoint or side.");
        }
        neighbor->second->neighborLabelToOneSidedInitialEdge[label] =
            std::shared_ptr<OneSidedInitialEdge>(pInitialNode->computeCorrospondingOneSidedEdge(edge.get()));
    }
}


void SegmentManager::computeOneSidedEdgesOnAllInitialNodes(const dataType::SegmentsStorage &image) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Computing one-sided edges on all initial nodes"));
    if (pInitialNodes->empty()) return;
    const auto full = image.GetLargestPossibleRegion();
    const auto depth = full.GetSize()[2];
    const auto workers = std::min<itk::SizeValueType>(depth, segment_puzzler::workerThreadCount(segment_puzzler::ThreadBackend::ITK));
    using ContactRuns = std::map<EdgePairIdType, std::vector<VoxelList::Run>>;
    std::vector<ContactRuns> contacts(workers);
    auto parallel = itk::MultiThreaderBase::New();
    parallel->SetMaximumNumberOfThreads(workers);
    parallel->SetNumberOfWorkUnits(workers);
    parallel->ParallelizeArray(0, workers, [&](itk::SizeValueType worker) {
        const auto first = full.GetIndex()[2] + static_cast<itk::IndexValueType>(depth * worker / workers);
        const auto count = depth * (worker + 1) / workers - depth * worker / workers;
        auto region = full;
        // The preceding plane supplies Z contacts across the worker boundary.
        region.SetIndex(2, first - (worker != 0));
        region.SetSize(2, count + (worker != 0));
        auto &local = contacts[worker];
        segment_puzzler::visitLabelContacts(image, region, first,
            [&](SegmentIdType a, SegmentIdType b, const itk::Index<3> &left, const itk::Index<3> &right, std::size_t length) {
                if (a == b || isIgnoredId(a) || isIgnoredId(b)) return;
                local[{a, b}].push_back({Voxel(left[0], left[1], left[2]), static_cast<std::uint32_t>(length)});
                local[{b, a}].push_back({Voxel(right[0], right[1], right[2]), static_cast<std::uint32_t>(length)});
            });
    }, nullptr);

    ContactRuns combined;
    for (auto &local : contacts) {
        combined.merge(local);
        for (auto &[pair, runs] : local) {
            auto &all = combined.at(pair);
            all.insert(all.end(), runs.begin(), runs.end());
        }
        local.clear();
    }
    for (auto &[label, node] : *pInitialNodes) node->neighborLabelToOneSidedInitialEdge.clear();
    for (auto &[pair, runs] : combined) {
        auto edge = segment_puzzler::makeContactEdge(pair.first, pair.second, runs);
        pInitialNodes->at(pair.first)->neighborLabelToOneSidedInitialEdge.emplace(pair.second, std::move(edge));
    }
}

void SegmentManager::buildTwoSidedInitialEdgesFromOneSidedInitialEdges(bool veryVerbose) {
    const bool logTiming = verbose || veryVerbose;
    ScopedSegmentManagerTimer timer(
        logTiming,
        __func__,
        QStringLiteral("Building two-sided initial edges from one-sided initial edges"));

    std::vector<TwoSidedInitialEdgeSources> twoSidedInitialEdgeSources;
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    std::size_t inspectedOneSidedEdgeCount = 0;
    for (const auto &[sourceLabel, sourceNode] : *pInitialNodes) {
        for (const auto &[neighborLabel, sourceOneSidedInitialEdge] :
             sourceNode->neighborLabelToOneSidedInitialEdge) {
            ++inspectedOneSidedEdgeCount;
            if (sourceLabel == neighborLabel) {
                throw std::logic_error("A one-sided initial edge cannot connect a label to itself.");
            }
            if (sourceLabel > neighborLabel) {
                continue;
            }

            const EdgePairIdType canonicalLabelPair{sourceLabel, neighborLabel};
            if (pInitialLabelPairToTwoSidedInitialEdge->find(canonicalLabelPair) != pInitialLabelPairToTwoSidedInitialEdge->end()) {
                continue;
            }

            const auto neighborNode = pInitialNodes->find(neighborLabel);
            if (neighborNode == pInitialNodes->end()) {
                throw std::logic_error(
                    "Cannot build a two-sided initial edge because the neighboring initial node is missing.");
            }
            const auto oppositeOneSidedInitialEdge =
                neighborNode->second->neighborLabelToOneSidedInitialEdge.find(sourceLabel);
            if (oppositeOneSidedInitialEdge ==
                neighborNode->second->neighborLabelToOneSidedInitialEdge.end()) {
                throw std::logic_error(
                    "Cannot build a two-sided initial edge because the opposite one-sided initial edge is missing.");
            }
            if (sourceOneSidedInitialEdge == nullptr ||
                oppositeOneSidedInitialEdge->second == nullptr) {
                throw std::logic_error("Cannot build a two-sided initial edge from a null one-sided initial edge.");
            }

            twoSidedInitialEdgeSources.push_back(
                {canonicalLabelPair,
                 sourceOneSidedInitialEdge,
                 oppositeOneSidedInitialEdge->second});
        }
    }
    const double collectEdgePairsMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    phaseTimer.restart();
    std::sort(
        twoSidedInitialEdgeSources.begin(),
        twoSidedInitialEdgeSources.end(),
        [](const TwoSidedInitialEdgeSources &left, const TwoSidedInitialEdgeSources &right) {
            return left.labelPair < right.labelPair;
        });
    const double sortEdgePairsMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    const int requestedThreadCount = segment_puzzler::workerThreadCount();
    const bool edgeFeaturesEnabled = !FeatureList::edgeFeaturesList.empty();
    const int executionThreadCount = edgeFeaturesEnabled ? 1 : requestedThreadCount;

    if (requestedThreadCount > 1 && edgeFeaturesEnabled) {
        logSegmentManager(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Using serial two-sided initial-edge construction because edge feature prototypes are active"));
    }
    std::vector<std::unique_ptr<TwoSidedInitialEdge>> twoSidedInitialEdgesToRegister(
        twoSidedInitialEdgeSources.size());
    phaseTimer.restart();
    int usedThreadCount = 1;
    std::exception_ptr firstException;
    std::mutex exceptionMutex;
    std::atomic<bool> stopRequested{false};
#ifdef USE_OMP
#pragma omp parallel num_threads(executionThreadCount) default(none) \
    shared(twoSidedInitialEdgeSources, twoSidedInitialEdgesToRegister, firstException, \
           exceptionMutex, stopRequested, usedThreadCount)
#endif
    {
#ifdef USE_OMP
#pragma omp single
        usedThreadCount = omp_get_num_threads();
#pragma omp for schedule(guided)
#endif
        for (long long index = 0;
             index < static_cast<long long>(twoSidedInitialEdgeSources.size()); ++index) {
            if (stopRequested.load(std::memory_order_relaxed)) {
                continue;
            }
            try {
                twoSidedInitialEdgesToRegister[static_cast<std::size_t>(index)] =
                    buildTwoSidedInitialEdge(twoSidedInitialEdgeSources[static_cast<std::size_t>(index)]);
            } catch (...) {
                std::lock_guard<std::mutex> guard(exceptionMutex);
                if (!firstException) {
                    firstException = std::current_exception();
                }
                stopRequested.store(true, std::memory_order_relaxed);
            }
        }
    }
    if (firstException) {
        std::rethrow_exception(firstException);
    }
    const double createTwoSidedEdgesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    phaseTimer.restart();
    if (edgeFeaturesEnabled) {
        for (auto &twoSidedInitialEdge : twoSidedInitialEdgesToRegister) {
            twoSidedInitialEdge->calculateEdgeFeatures();
        }
    }
    const double featureCalculationMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    phaseTimer.restart();
    const auto size = graphBase->pEdgesInitialSegmentsImage->GetLargestPossibleRegion().GetSize();
    const auto origin = graphBase->pEdgesInitialSegmentsImage->GetLargestPossibleRegion().GetIndex();
    const bool updatingExistingEdges = !pInitialLabelPairToTwoSidedInitialEdge->empty();
    using EdgePixel = std::pair<std::size_t, dataType::MappedEdgeIdType>;
    std::vector<std::vector<EdgePixel>> planes(updatingExistingEdges ? 0 : size[2]);
    for (auto &twoSidedInitialEdge : twoSidedInitialEdgesToRegister) {
        auto *edge = twoSidedInitialEdge.get();
        registerTwoSidedInitialEdge(std::move(twoSidedInitialEdge), updatingExistingEdges);
        if (updatingExistingEdges) continue;
        for (const auto *list : edge->getVoxelLists())
            for (const auto &voxel : *list)
                planes.at(voxel.z - origin[2]).emplace_back((voxel.y - origin[1]) * size[0] + (voxel.x - origin[0]), edge->numId);
    }
    std::vector<dataType::MappedEdgeIdType> pixels;
    constexpr std::size_t planesPerBatch = 16;
    const std::size_t planePixels = size[0] * size[1];
    for (std::size_t z = 0; z < planes.size(); z += planesPerBatch) {
        const auto end = std::min(z + planesPerBatch, planes.size());
        if (std::all_of(planes.begin() + z, planes.begin() + end,
                        [](const auto &plane) { return plane.empty(); })) continue;
        auto region = graphBase->pEdgesInitialSegmentsImage->GetLargestPossibleRegion();
        region.SetIndex(2, region.GetIndex()[2] + z);
        region.SetSize(2, end - z);
        pixels.resize(region.GetNumberOfPixels());
        graphBase->pEdgesInitialSegmentsImage->readRegion(region, pixels.data(), pixels.size());
        for (auto plane = z; plane < end; ++plane) {
            for (const auto &[offset, value] : planes[plane])
                pixels[(plane - z) * planePixels + offset] = value;
            std::vector<EdgePixel>().swap(planes[plane]);
        }
        graphBase->pEdgesInitialSegmentsImage->writeRegion(region, pixels.data(), pixels.size());
    }
    const double registerTwoSidedEdgesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    logSegmentManagerDebugIf(
        logTiming,
        __func__,
        QStringLiteral(
            "Two-sided initial-edge build phases inspected_one_sided_edges=%1 edge_pairs_to_merge=%2 "
            "requested_threads=%3 used_threads=%4 parallel=%5 edge_features_enabled=%6 "
            "collect_edge_pairs_ms=%7 sort_edge_pairs_ms=%8 create_two_sided_edges_ms=%9 "
            "feature_calculation_ms=%10 register_two_sided_edges_ms=%11")
            .arg(static_cast<qulonglong>(inspectedOneSidedEdgeCount))
            .arg(static_cast<qulonglong>(twoSidedInitialEdgeSources.size()))
            .arg(requestedThreadCount)
            .arg(usedThreadCount)
            .arg(usedThreadCount > 1 ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(edgeFeaturesEnabled ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(collectEdgePairsMs, 0, 'f', 3)
            .arg(sortEdgePairsMs, 0, 'f', 3)
            .arg(createTwoSidedEdgesMs, 0, 'f', 3)
            .arg(featureCalculationMs, 0, 'f', 3)
            .arg(registerTwoSidedEdgesMs, 0, 'f', 3));
}


void
SegmentManager::constructWorkingNodeFromInitialNode(InitialNode *baseInitialNode, bool useSameIdAsInitialNode,
                                                    bool veryVerbose) {
    ScopedSegmentManagerTimer timer(veryVerbose, __func__, QStringLiteral("Constructing working node from initial node"));

    SegmentIdType newLabel;
    if (useSameIdAsInitialNode) {
        newLabel = baseInitialNode->getLabel();
    } else {
        newLabel = takeNextSegmentId(*nextFreeId);
    }


    auto *newWorkingNode = new WorkingNode(baseInitialNode, newLabel, *pInitialNodes);
    addWorkingNode(newWorkingNode);
}


SegmentManager::SegmentIdType SegmentManager::takeNextSegmentId(SegmentIdType &cursor) const {
    auto label = cursor;
    const auto limit = std::numeric_limits<SegmentIdType>::max();
    while (label < limit && isIgnoredId(label)) ++label;
    // The cursor must still be representable after the last allocation.
    if (label == limit) throw std::overflow_error("No free working segment IDs remain.");
    cursor = label + 1;
    return label;
}

bool SegmentManager::isIgnoredId(SegmentIdType idToCheck) const {
    return (std::find(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end(), idToCheck) !=
            pIgnoredSegmentLabels->end());
}

void SegmentManager::recalculateEdgesOnWorkingNode(WorkingNode *pWorkingNode, bool veryVerbose) {
    ScopedSegmentManagerTimer timer(veryVerbose, __func__, QStringLiteral("Recalculating edges on working node"));
    // de-register old edges and clear twosided edges
    // also deregister the edge from the other node

    std::vector<WorkingEdge *> workingEdgesToDelete =
            utils::getTargetPointersVecOfSharedPtrMap<SegmentIdType, WorkingEdge>(pWorkingNode->neighborLabelToWorkingEdge);
    for (auto edge : workingEdgesToDelete) {
        removeWorkingEdge(edge);
    }


    SegmentIdType label = pWorkingNode->getLabel();
    // create new twosided edges, will look if its exist already
    for (auto &subInitialNode : pWorkingNode->subInitialNodes) {
        for (auto &initialEdge : subInitialNode.second->neighborLabelToTwoSidedInitialEdge) {
            SegmentIdType workingLabelOfConnectedNode = pInitialNodes->at(
                    initialEdge.first)->getCurrentWorkingNodeLabel();
            if (workingLabelOfConnectedNode != label) { // if it is not an "internal" edge
                if (pWorkingNode->neighborLabelToWorkingEdge.count(workingLabelOfConnectedNode) == 0) {
                    auto newWorkingEdge = new WorkingEdge(initialEdge.second, label, workingLabelOfConnectedNode);
                    addWorkingEdge(newWorkingEdge);
                } else {
                    pWorkingNode->neighborLabelToWorkingEdge.at(workingLabelOfConnectedNode)->addConstituentEdge(initialEdge.second);
                }
            }
        }
    }
}

void SegmentManager::buildWorkingGraphFromInitialGraph() {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Building working graph from initial graph"));

    if (!pWorkingNodes->empty() || !pWorkingLabelPairToWorkingEdge->empty()) {
        throw std::logic_error("The working graph must be empty before it is built from the initial graph.");
    }

    QElapsedTimer phaseTimer;
    phaseTimer.start();
    pWorkingNodes->reserve(pInitialNodes->size());
    const double reserveWorkingNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    phaseTimer.restart();
    for (auto &initialNode : *pInitialNodes) {
        constructWorkingNodeFromInitialNode(initialNode.second.get());
    }
    const double createWorkingNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    phaseTimer.restart();
    for (const auto &[edgePair, initialEdge] : *pInitialLabelPairToTwoSidedInitialEdge) {
        addWorkingEdge(new WorkingEdge(initialEdge, edgePair.first, edgePair.second));
    }
    const double createWorkingEdgesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    logSegmentManagerDebugIf(
        verbose,
        __func__,
        QStringLiteral(
            "Working graph build phases initial_nodes=%1 initial_edges=%2 working_nodes=%3 working_edges=%4 "
            "reserve_working_nodes_ms=%5 create_working_nodes_ms=%6 create_working_edges_ms=%7")
            .arg(static_cast<qulonglong>(pInitialNodes->size()))
            .arg(static_cast<qulonglong>(pInitialLabelPairToTwoSidedInitialEdge->size()))
            .arg(static_cast<qulonglong>(pWorkingNodes->size()))
            .arg(static_cast<qulonglong>(pWorkingLabelPairToWorkingEdge->size()))
            .arg(reserveWorkingNodesMs, 0, 'f', 3)
            .arg(createWorkingNodesMs, 0, 'f', 3)
            .arg(createWorkingEdgesMs, 0, 'f', 3));
}

void SegmentManager::removeWorkingNode(WorkingNode *workingNodeToRemove, bool veryVerbose) {
    ScopedSegmentManagerTimer timer(veryVerbose,
                                    __func__,
                                    QStringLiteral("Removing working node %1").arg(workingNodeToRemove->getLabel()));
    // deregister edges
    std::vector<WorkingEdge *> vecOfWorkingEdgesToRemove =
            utils::getTargetPointersVecOfSharedPtrMap<SegmentIdType, WorkingEdge>(workingNodeToRemove->neighborLabelToWorkingEdge);
    for (auto pWorkingEdge : vecOfWorkingEdgesToRemove) {
        removeWorkingEdge(pWorkingEdge);
    }
    // deregeister Node
    pWorkingNodes->erase(workingNodeToRemove->getLabel());
}

// debugging stuff

void SegmentManager::printInitialNodes(std::ostream &outStream) {
    outStream << "=== initialNodes ===\n";
    int nodeIndentationLevel = 1;
    for (auto &node : *pInitialNodes) {
        outStream << "key: " << node.first << "\n";
        node.second->print(nodeIndentationLevel, outStream);
    }
}

void SegmentManager::printInitialOneSidedEdges(std::ostream &outStream) {
    outStream << "=== initialOneSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (const auto &[sourceLabel, initialNode] : *pInitialNodes) {
        for (const auto &[neighborLabel, edge] : initialNode->neighborLabelToOneSidedInitialEdge) {
            outStream << "key: " << sourceLabel << "," << neighborLabel << "\n";
            edge->print(nodeIndentationLevel, outStream);
        }
    }
}

void SegmentManager::printInitialTwoSidedEdges(std::ostream &outStream) {
    outStream << "=== initialTwoSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : *pInitialLabelPairToTwoSidedInitialEdge) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        edge.second->print(nodeIndentationLevel, outStream);
    }
}

void SegmentManager::printWorkingNodes(std::ostream &outStream) {
    outStream << "=== workingNodes ===\n";
    int nodeIndentationLevel = 1;
    for (auto &node : *pWorkingNodes) {
        outStream << "key: " << node.first << "\n";
        node.second->print(nodeIndentationLevel, outStream);
    }
}

void SegmentManager::printEdgeIdLookUp(std::ostream &outStream) {
    outStream << "=== edgeIdLookup ===\n";
    for (auto &id : *pInitialEdgeIdLookUp) {
        outStream << "numId: " << id.first << "\n";
        outStream << "pairId: " << id.second.first << "," << id.second.second << "\n\n";
    }
}

void SegmentManager::printWorkingEdges(std::ostream &outStream) {
    outStream << "=== workingEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : *pWorkingLabelPairToWorkingEdge) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        edge.second->print(nodeIndentationLevel, outStream);
    }
}

void SegmentManager::printInitialNodesToFile(const std::string &pathToOutputfile) {
    logSegmentManager(LogLevel::Info,
                      __func__,
                      QStringLiteral("Writing initial nodes to %1").arg(QString::fromStdString(pathToOutputfile)),
                      kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printInitialNodes(outFile);
    outFile.close();
}

void SegmentManager::printInitialTwoSidedEdgesToFile(const std::string &pathToOutputfile) {
    logSegmentManager(LogLevel::Info,
                      __func__,
                      QStringLiteral("Writing initial two-sided edges to %1").arg(QString::fromStdString(pathToOutputfile)),
                      kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printInitialTwoSidedEdges(outFile);
    outFile.close();
}

void SegmentManager::printInitialOneSidedEdgesToFile(const std::string &pathToOutputfile) {
    logSegmentManager(LogLevel::Info,
                      __func__,
                      QStringLiteral("Writing initial one-sided edges to %1").arg(QString::fromStdString(pathToOutputfile)),
                      kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printInitialOneSidedEdges(outFile);
    outFile.close();
}

void SegmentManager::printWorkingNodesToFile(const std::string &pathToOutputfile) {
    logSegmentManager(LogLevel::Info,
                      __func__,
                      QStringLiteral("Writing working nodes to %1").arg(QString::fromStdString(pathToOutputfile)),
                      kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printWorkingNodes(outFile);
    outFile.close();
}

void SegmentManager::printEdgeIdLookUpToFile(const std::string &pathToOutputfile) {
    logSegmentManager(LogLevel::Info,
                      __func__,
                      QStringLiteral("Writing edge-id lookup to %1").arg(QString::fromStdString(pathToOutputfile)),
                      kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printEdgeIdLookUp(outFile);
    outFile.close();
}

void SegmentManager::printWorkingEdgesToFile(const std::string &pathToOutputfile) {
    logSegmentManager(LogLevel::Info,
                      __func__,
                      QStringLiteral("Writing working edges to %1").arg(QString::fromStdString(pathToOutputfile)),
                      kIoCategory);
    std::ofstream outFile(pathToOutputfile);
    printWorkingEdges(outFile);
    outFile.close();
}
