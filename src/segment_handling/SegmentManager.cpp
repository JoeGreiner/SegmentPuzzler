

#include "SegmentManager.h"
#include <QElapsedTimer>
#include <QStringList>
#include "src/utils/AppLogger.h"
#include <src/utils/utils.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
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

template<typename Container>
QString joinIds(const Container &values) {
    QStringList parts;
    for (const auto &value : values) {
        parts << QString::number(static_cast<qulonglong>(value));
    }
    return parts.join(QStringLiteral(" "));
}

struct OneSidedInitialEdgePair {
    dataType::EdgePairIdType labelPair;
    InitialEdge *lowerLabelSide = nullptr;
    InitialEdge *higherLabelSide = nullptr;
};

std::unique_ptr<InitialEdge> createTwoSidedInitialEdge(const OneSidedInitialEdgePair &edgePair) {
    auto twoSidedInitialEdge = std::make_unique<InitialEdge>(*edgePair.lowerLabelSide);
    twoSidedInitialEdge->mergeVoxelsAndROIwithOtherEdge(*edgePair.higherLabelSide);
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

void SegmentManager::addInitialNode(SegmentIdType labelOfNewNode, int reserveMemoryForVoxels) {
    auto pNewInitialNode = std::shared_ptr<InitialNode>(new InitialNode(graphBase, *ppWorkingSegmentsImage, labelOfNewNode));
    if (reserveMemoryForVoxels > 0) {
        pNewInitialNode->voxels.reserve(reserveMemoryForVoxels);
    }
    (*pInitialNodes)[labelOfNewNode] = pNewInitialNode;

}


void SegmentManager::addTwoSidedInitialEdge(std::unique_ptr<InitialEdge> edgeToAdd) {
    if (edgeToAdd == nullptr) {
        throw std::invalid_argument("Cannot add a null two-sided initial edge.");
    }

    graphBase->edgeCounter++;
    size_t newEdgeId = graphBase->edgeCounter;
    edgeToAdd->setIdAndRegister(newEdgeId, *pInitialEdgeIdLookUp);
    auto sharedEdgeToAdd = std::shared_ptr<InitialEdge>(std::move(edgeToAdd));
    pInitialNodes->at(sharedEdgeToAdd->getLabelSmaller())->addTwoSidedEdge(sharedEdgeToAdd);
    pInitialNodes->at(sharedEdgeToAdd->getLabelBigger())->addTwoSidedEdge(sharedEdgeToAdd);
    (*pInitialTwoSidedEdges)[sharedEdgeToAdd->pairId] = sharedEdgeToAdd;

    char defaultEdgeStatus = 0;
    pEdgeStatus->insert(std::pair<EdgeNumIdType, char>(sharedEdgeToAdd->numId, defaultEdgeStatus));
    for (auto &voxel : sharedEdgeToAdd->voxels) {
        graphBase->pEdgesInitialSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, sharedEdgeToAdd->numId);
    }
}

void SegmentManager::addWorkingNode(WorkingNode *workingNodeToAdd) {
    (*pWorkingNodes)[workingNodeToAdd->getLabel()] = std::shared_ptr<WorkingNode>(workingNodeToAdd);
}


void SegmentManager::addWorkingEdge(WorkingEdge *pWorkingEdgeToAdd) {
    EdgePairIdType pairId = pWorkingEdgeToAdd->pairId;
    SegmentIdType labelA = pairId.first;
    SegmentIdType labelB = pairId.second;

    auto spWorkingEdgeToAdd = std::shared_ptr<WorkingEdge>(pWorkingEdgeToAdd);
    (*pWorkingEdges)[pairId] = spWorkingEdgeToAdd;


    pWorkingNodes->at(labelA)->twosidedEdges[labelB] = spWorkingEdgeToAdd;
    pWorkingNodes->at(labelB)->twosidedEdges[labelA] = spWorkingEdgeToAdd;
}


// ==== remove =====

void SegmentManager::removeInitialNode(SegmentManager::SegmentIdType labelOfNodeToRemove) {
    InitialNode *nodeToRemove = (*pInitialNodes)[labelOfNodeToRemove].get();
    ScopedSegmentManagerTimer timer(verbose,
                                    __func__,
                                    QStringLiteral("Removing initial node %1").arg(nodeToRemove->getLabel()));
    // take care of: initialtwosided edges, initial onesided edges, edgeidlookup, initialnodes, initialEdgeVolume, edgeStatus

    removeEdgePropertiesOnInitialNode(nodeToRemove);
    pInitialNodes->erase(nodeToRemove->getLabel());
}

void SegmentManager::removeEdgePropertiesOnInitialNode(InitialNode *pInitialNode) {

    int backgroundValueEdges = 0;
    for (auto initialEdge : pInitialNode->twosidedEdges) {
        // set voxels in the initial edge volume to background
        logSegmentManagerDebugIf(verbose,
                                 __func__,
                                 QStringLiteral("Removing voxels for initial edge %1").arg(initialEdge.second->numId));
        for (auto &voxel : initialEdge.second->voxels) {
            (*ppEdgesInitialSegmentsImage)->SetPixel({voxel.x, voxel.y, voxel.z}, backgroundValueEdges);
        }

        // delete edge status
        if (pEdgeStatus->count(initialEdge.second->numId) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing edge status for initial edge %1").arg(initialEdge.second->numId));
            pEdgeStatus->erase(initialEdge.second->numId);
        }

        // take care of twosided edges
        if (pInitialTwoSidedEdges->count(initialEdge.second->pairId) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing two-sided initial edge %1 -> %2")
                                         .arg(initialEdge.second->pairId.first)
                                         .arg(initialEdge.second->pairId.second));
            pInitialTwoSidedEdges->erase(initialEdge.second->pairId);
        }
        // also delete from the paired node
        if (pInitialNodes->count(initialEdge.first) > 0) {
            if ((*pInitialNodes)[initialEdge.first]->twosidedEdges.count(pInitialNode->getLabel()) > 0) {
                logSegmentManagerDebugIf(verbose,
                                         __func__,
                                         QStringLiteral("Removing paired two-sided initial edge %1 -> %2")
                                             .arg(initialEdge.first)
                                             .arg(pInitialNode->getLabel()));
                (*pInitialNodes)[initialEdge.first]->twosidedEdges.erase(pInitialNode->getLabel());
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
    pInitialNode->twosidedEdges.clear();

    // delete onesided edges
    for (auto edge : pInitialNode->onesidedEdges) {
        if ((*pInitialNodes)[edge.first]->onesidedEdges.count(pInitialNode->getLabel()) > 0) {
            logSegmentManagerDebugIf(verbose,
                                     __func__,
                                     QStringLiteral("Removing mirrored one-sided edge %1 -> %2")
                                         .arg(edge.first)
                                         .arg(pInitialNode->getLabel()));
            (*pInitialNodes)[edge.first]->onesidedEdges.erase(pInitialNode->getLabel());
        }
    }
    pInitialNode->onesidedEdges.clear();
}


void SegmentManager::clearGraphAndReserveInitialNodes(std::size_t initialNodeCapacity) {
    const std::size_t previousInitialNodeCount = pInitialNodes->size();
    std::size_t previousOneSidedEdgeCount = 0;
    for (const auto &initialNodeEntry : *pInitialNodes) {
        previousOneSidedEdgeCount += initialNodeEntry.second->onesidedEdges.size();
    }
    const std::size_t previousTwoSidedEdgeCount = pInitialTwoSidedEdges->size();
    const std::size_t previousWorkingNodeCount = pWorkingNodes->size();
    const std::size_t previousWorkingEdgeCount = pWorkingEdges->size();

    QElapsedTimer phaseTimer;
    phaseTimer.start();
    pInitialNodes->clear();
    const double clearInitialNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pInitialTwoSidedEdges->clear();
    const double clearTwoSidedEdgesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pInitialEdgeIdLookUp->clear();
    const double clearEdgeIdLookupMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pWorkingNodes->clear();
    const double clearWorkingNodesMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;
    phaseTimer.restart();
    pWorkingEdges->clear();
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
        if (pWorkingNodes->at(labelA)->twosidedEdges.count(labelB) > 0) { // if twosided edge exists
            pWorkingNodes->at(labelA)->twosidedEdges.erase(labelB); // delete it
        } else {
            logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Working node %1 has a dangling edge to %2").arg(labelA).arg(labelB));
        }
    } else {
        logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Removing edge with missing working node %1").arg(labelA));
    }

    if (pWorkingNodes->count(labelB) > 0) { // if workingNode exist
        if (pWorkingNodes->at(labelB)->twosidedEdges.count(labelA) > 0) { // if twosided edge exists
            pWorkingNodes->at(labelB)->twosidedEdges.erase(labelA); // delete it
        } else {
            logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Working node %1 has a dangling edge to %2").arg(labelB).arg(labelA));
        }
    } else {
        logSegmentManager(LogLevel::Warning, __func__, QStringLiteral("Removing edge with missing working node %1").arg(labelB));
    }

    pWorkingEdges->erase(pairId); // also delete it from working edges
}


// === compute ===
void SegmentManager::recomputeVoxelListAndOneSidedEdgesIfShrinked(
        std::vector<SegmentIdType> vecOfConnectedInitialNodeIds) {
    // Attention: Here has to be true: WorkingNode == InitialNode!!


    for (auto id : vecOfConnectedInitialNodeIds) {
        InitialNode *pInitialNode = pInitialNodes->at(id).get();

        bool voxelListChanged = false;
        std::vector<Voxel> updateVoxelList;
        updateVoxelList.reserve(pInitialNode->voxels.size());

        // check if one of the labeles is replaced by the refinement ws
        for (auto voxel : (pInitialNode->voxels)) {
            if ((*ppWorkingSegmentsImage)->GetPixel({voxel.x, voxel.y, voxel.z}) == pInitialNode->getLabel()) {
                updateVoxelList.emplace_back(voxel.x, voxel.y, voxel.z);
            }
        }
        // if size changed, update voxel list
        if (updateVoxelList.size() != pInitialNode->voxels.size()) {
            voxelListChanged = true;
            pInitialNode->voxels = updateVoxelList;
        }

        if (voxelListChanged) {
            //TODO: split all neighbors into initial nodes!

            removeEdgePropertiesOnInitialNode(pInitialNode);

            for (auto edge : pInitialNode->onesidedEdges) {
                SegmentIdType connectedWorkingNode = pInitialNodes->at(edge.first)->getCurrentWorkingNodeLabel();
                //TODO: Dont split but use edge to get coordinate and only subtract initial node at edge?
                splitWorkingNodeIntoInitialNodes(connectedWorkingNode);
                //                splitWorkingNodeIntoInitialNodes()
            }


            computeSurfaceAndOneSidedEdgesOnInitialNode(pInitialNode);
            for (auto &edge : pInitialNode->onesidedEdges) {
                // TODO: this is ugly, add a function to add the initial edge!


                auto pNewEdge = std::shared_ptr<InitialEdge>(
                        pInitialNode->computeCorrospondingOneSidedEdge(edge.second.get()));
                (*pInitialNodes)[edge.first]->onesidedEdges[pInitialNode->getLabel()] = pNewEdge;
            }
        }
    }
}


void SegmentManager::removeInitialNodeFromWorkingNodeAtPosition(int x, int y, int z) {
    // as initialnodes are not saved explicitly, workaround:
    // unsplit working node into all initialnode
    // get initialnode at (x,y,z)
    // create working segments with all other initialnodes but the selected one
    // split new working segment into connected components
    SegmentIdType labelOfWorkingNode = graphBase->pWorkingSegmentsImage->GetPixel({x, y, z});
    if (!isIgnoredId(labelOfWorkingNode)) {
        std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> initialNodesOfWorkingSegment = pWorkingNodes->at(
                labelOfWorkingNode)->subInitialNodes;
        splitWorkingNodeIntoInitialNodes(labelOfWorkingNode);
        SegmentIdType initialNodeAtPosition = graphBase->pWorkingSegmentsImage->GetPixel({x, y, z});
        initialNodesOfWorkingSegment.erase(initialNodeAtPosition);
        SegmentIdType labelOfNewNode = *nextFreeId;
        (*nextFreeId)++;
        std::vector<SegmentIdType> newInitialNodesOfWorkingSegment = utils::getKeyVecOfSharedPtrMap<SegmentIdType, InitialNode>(
                initialNodesOfWorkingSegment);
        if (!newInitialNodesOfWorkingSegment.empty()) {
            for (auto nodeLabel : newInitialNodesOfWorkingSegment) {
                // special case: initialNode == workingNode
                removeWorkingNode(pWorkingNodes->at(nodeLabel).get());
            }
            WorkingNode *newWorkingNode = new WorkingNode(newInitialNodesOfWorkingSegment, labelOfNewNode,
                                                          *pInitialNodes);
            addWorkingNode(newWorkingNode);
            recalculateEdgesOnWorkingNode(newWorkingNode);

            insertWorkingNodeIntoITKImage(newWorkingNode);
            splitIntoConnectedComponentsOfWorkingNode(*newWorkingNode);
        }
    }
}

void SegmentManager::insertWorkingNodeIntoITKImage(WorkingNode *newWorkingNode) {
    for (auto &voxelArray : newWorkingNode->getVoxelPointerArray()) {
        for (auto voxel : *voxelArray) {
            (*ppWorkingSegmentsImage)->SetPixel({voxel.x, voxel.y, voxel.z}, newWorkingNode->getLabel());
        }
    }
}

void SegmentManager::splitWorkingNodeIntoInitialNodes(SegmentIdType workingNodeIdToSplit) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Splitting working node into initial nodes"));
    if (!isIgnoredId(workingNodeIdToSplit)) {

        auto pToWorkingNode = pWorkingNodes->at(workingNodeIdToSplit);

        std::vector<SegmentIdType> idsOfSubInitialNodes;
        for (auto &initialNode : pToWorkingNode->subInitialNodes) {
            idsOfSubInitialNodes.push_back(initialNode.first);
        }
        logSegmentManagerDebugIf(verbose,
                                 __func__,
                                 QStringLiteral("Working node %1 splits into initial nodes [%2]")
                                     .arg(workingNodeIdToSplit)
                                     .arg(joinIds(idsOfSubInitialNodes)));

        // remove corrosponding workingedges and workingNode
        removeWorkingNode(pToWorkingNode.get());


        std::set<SegmentIdType> setOfInsertedWorkingNodes;
        // insert initialnodes into workingnodes
        for (auto &subInitialNodeId : idsOfSubInitialNodes) {

            // create new working node based on inital node
            WorkingNode *newWorkingNode = new WorkingNode(pInitialNodes->at(subInitialNodeId).get(), subInitialNodeId,
                                                          *pInitialNodes);
            addWorkingNode(newWorkingNode);
            setOfInsertedWorkingNodes.insert(subInitialNodeId);

            for (auto &voxelArray : newWorkingNode->getVoxelPointerArray()) {
                for (auto voxel : *voxelArray) {
                    (*ppWorkingSegmentsImage)->SetPixel({voxel.x, voxel.y, voxel.z}, subInitialNodeId);
                }
            }
        }

        // put neighboring nodes in the set
        std::set<SegmentIdType> setOfInsertedWorkingNodesAndNeighbors;
        for (auto id : setOfInsertedWorkingNodes) {
            setOfInsertedWorkingNodesAndNeighbors.insert(id);
            std::vector<SegmentIdType> idsOfConnectedNodes = pWorkingNodes->at(id)->getVectorOfConnectedNodeIds();
            for (auto neighborId : idsOfConnectedNodes) {
                setOfInsertedWorkingNodesAndNeighbors.insert(neighborId);
            }
        }

        // update workingedges of new workingnodes and their neighbors
        for (auto id : setOfInsertedWorkingNodesAndNeighbors) {
            recalculateEdgesOnWorkingNode(pWorkingNodes->at(id).get());
        }

        // reset merge status
        char defaultEdgeStatus = 0;
        for (auto id : setOfInsertedWorkingNodes) {
            for (auto &edge : pWorkingNodes->at(id)->twosidedEdges) {
                for (auto pInitialEdge : edge.second->subInitialEdges) {
                    pEdgeStatus->at(pInitialEdge->numId) = defaultEdgeStatus;
                }
            }
        }
    }

}


void SegmentManager::splitIntoConnectedComponentsOfWorkingNode(
        WorkingNode &workingNodeToAnalyze) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Splitting working node into connected components"));


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
            for (auto &twosidedEdge : pInitialNodes->at(activeNode)->twosidedEdges) {
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

        logSegmentManagerDebugIf(verbose,
                                 __func__,
                                 QStringLiteral("Connected component labels=[%1]").arg(joinIds(visitedWorkingNodeLabelsThisRun)));

        SegmentIdType labelOfNewNode = *nextFreeId;
        (*nextFreeId)++;
        WorkingNode *newWorkingNode = new WorkingNode(visitedWorkingNodeLabelsThisRun, labelOfNewNode, *pInitialNodes);
        addWorkingNode(newWorkingNode);
        recalculateEdgesOnWorkingNode(newWorkingNode);

        //TODO: export this as a function
        std::vector<SegmentIdType> idsOfConnectedNodes = newWorkingNode->getVectorOfConnectedNodeIds();
        for (auto &id : idsOfConnectedNodes) {
            recalculateEdgesOnWorkingNode(pWorkingNodes->at(id).get());
        }

        for (auto id : visitedWorkingNodeLabelsThisRun) {
            visitedWorkingNodeLabels.insert(id);
        }

        insertWorkingNodeIntoITKImage(newWorkingNode);

    }
    removeWorkingNode(&workingNodeToAnalyze);
}


void SegmentManager::computeSurfaceAndOneSidedEdgesOnInitialNode(InitialNode *pInitialNode) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Computing one-sided edges for one initial node"));
    pInitialNode->computeOnesidedSurfaceAndEdges(*pIgnoredSegmentLabels);
}

void SegmentManager::computeCorrospondingOneSidedInitialEdges(InitialNode *pInitialNode) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Computing corresponding one-sided initial edges"));
    auto labelOfThisInitialNode = pInitialNode->getLabel();
    for (auto &edge : pInitialNode->onesidedEdges) {
        // TODO: this is ugly, add a function to add the initial edge!
        auto labelOfConnectedInitialNode = edge.first;

        auto pNewEdge = std::shared_ptr<InitialEdge>(pInitialNode->computeCorrospondingOneSidedEdge(edge.second.get()));

        if  ((*pInitialNodes).count(labelOfConnectedInitialNode) == 0){
            logSegmentManager(LogLevel::Error,
                              __func__,
                              QStringLiteral("Connected initial node %1 is missing while computing corresponding one-sided edges")
                                  .arg(labelOfConnectedInitialNode));
            printEdgeIdLookUpToFile("edgeIdLookup.txt");
            printWorkingNodesToFile("workingNodes.txt");
            printWorkingEdgesToFile("workingEdges.txt");
            printInitialNodesToFile("initialNodes.txt");
            printInitialTwoSidedEdgesToFile("initialTwoSidedEdges.txt");
            printInitialOneSidedEdgesToFile("initialOneSidedEdges.txt");
//            ITKImageWriter<dataType::EdgeImageType>(graphBase->pEdgesInitialSegmentsImage,
//                                                                        "initialEdges.nrrd");
//            ITKImageWriter<dataType::SegmentsImageType>(graphBase->pWorkingSegmentsImage,s
//                                                                            "workingSegments.nrrd");
        }

        (*pInitialNodes)[labelOfConnectedInitialNode]->onesidedEdges[labelOfThisInitialNode] = pNewEdge;
    }
}


void SegmentManager::computeSurfaceAndOneSidedEdgesOnAllInitialNodes(int threadCount) {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Computing one-sided edges on all initial nodes"));

    std::vector<std::shared_ptr<InitialNode>> initialNodes;
    initialNodes.reserve(pInitialNodes->size());
    long double totalRoiVoxels = 0.0L;
    long double maxNodeRoiVoxels = 0.0L;
    for (const auto &entry : *pInitialNodes) {
        initialNodes.push_back(entry.second);
        const Roi &roi = entry.second->roi;
        if (roi.minX >= 0 && roi.minY >= 0 && roi.minZ >= 0 &&
            roi.minX <= roi.maxX && roi.minY <= roi.maxY && roi.minZ <= roi.maxZ) {
            const long double roiVoxels =
                static_cast<long double>(static_cast<std::int64_t>(roi.maxX) - roi.minX + 1) *
                static_cast<long double>(static_cast<std::int64_t>(roi.maxY) - roi.minY + 1) *
                static_cast<long double>(static_cast<std::int64_t>(roi.maxZ) - roi.minZ + 1);
            totalRoiVoxels += roiVoxels;
            maxNodeRoiVoxels = std::max(maxNodeRoiVoxels, roiVoxels);
        }
    }

    const std::vector<SegmentIdType> ignoredSegmentLabels = *pIgnoredSegmentLabels;
    const int requestedThreadCount = std::max(1, threadCount);
    const bool edgeFeaturesEnabled = !FeatureList::edgeFeaturesList.empty();
#ifdef USE_OMP
    const bool runParallel = requestedThreadCount > 1 && initialNodes.size() > 1 && !edgeFeaturesEnabled;
#else
    const bool runParallel = false;
#endif

    if (requestedThreadCount > 1 && edgeFeaturesEnabled) {
        logSegmentManager(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Using the serial one-sided edge scan because edge feature prototypes are active"));
    }
#ifndef USE_OMP
    if (requestedThreadCount > 1) {
        logSegmentManagerDebugIf(
            verbose,
            __func__,
            QStringLiteral("Using the serial one-sided edge scan because OpenMP is disabled"));
    }
#endif

    QElapsedTimer scanTimer;
    scanTimer.start();
    int usedThreadCount = 1;

#ifdef USE_OMP
    if (runParallel) {
        std::exception_ptr firstException;
        std::mutex exceptionMutex;
        std::atomic<bool> stopRequested{false};

#pragma omp parallel num_threads(requestedThreadCount) default(none) \
    shared(initialNodes, ignoredSegmentLabels, firstException, exceptionMutex, stopRequested, usedThreadCount)
        {
#pragma omp single
            usedThreadCount = omp_get_num_threads();

#pragma omp for schedule(dynamic, 1)
            for (long long i = 0; i < static_cast<long long>(initialNodes.size()); ++i) {
                if (stopRequested.load(std::memory_order_relaxed)) {
                    continue;
                }
                try {
                    initialNodes[static_cast<std::size_t>(i)]->computeOnesidedSurfaceAndEdges(
                        ignoredSegmentLabels);
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
    } else
#endif
    {
        for (const auto &initialNode : initialNodes) {
            initialNode->computeOnesidedSurfaceAndEdges(ignoredSegmentLabels);
        }
    }
    const double scanMs = static_cast<double>(scanTimer.nsecsElapsed()) / 1000000.0;

    QElapsedTimer edgeCountTimer;
    edgeCountTimer.start();
    std::size_t oneSidedEdgeCount = 0;
    for (const auto &initialNode : initialNodes) {
        oneSidedEdgeCount += initialNode->onesidedEdges.size();
    }
    const double edgeCountMs = static_cast<double>(edgeCountTimer.nsecsElapsed()) / 1000000.0;

    logSegmentManagerDebugIf(
        verbose,
        __func__,
        QStringLiteral(
            "One-sided edge scan nodes=%1 requested_threads=%2 used_threads=%3 parallel=%4 "
            "roi_voxels=%5 max_node_roi=%6 edges=%7 scan_ms=%8 edge_count_ms=%9")
            .arg(static_cast<qulonglong>(initialNodes.size()))
            .arg(requestedThreadCount)
            .arg(usedThreadCount)
            .arg(runParallel ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(static_cast<double>(totalRoiVoxels), 0, 'g', 12)
            .arg(static_cast<double>(maxNodeRoiVoxels), 0, 'g', 12)
            .arg(static_cast<qulonglong>(oneSidedEdgeCount))
            .arg(scanMs, 0, 'f', 3)
            .arg(edgeCountMs, 0, 'f', 3));
}


void SegmentManager::buildTwoSidedInitialEdgesFromOneSidedInitialEdges(int threadCount, bool veryVerbose) {
    const bool logTiming = verbose || veryVerbose;
    ScopedSegmentManagerTimer timer(
        logTiming,
        __func__,
        QStringLiteral("Building two-sided initial edges from one-sided initial edges"));

    std::vector<OneSidedInitialEdgePair> oneSidedInitialEdgePairsToMerge;
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    std::size_t inspectedOneSidedEdgeCount = 0;
    for (const auto &[sourceLabel, sourceNode] : *pInitialNodes) {
        for (const auto &[neighborLabel, sourceSide] : sourceNode->onesidedEdges) {
            ++inspectedOneSidedEdgeCount;
            if (sourceLabel == neighborLabel) {
                throw std::logic_error("A one-sided initial edge cannot connect a label to itself.");
            }
            if (sourceLabel > neighborLabel) {
                continue;
            }

            const EdgePairIdType canonicalLabelPair{sourceLabel, neighborLabel};
            if (pInitialTwoSidedEdges->find(canonicalLabelPair) != pInitialTwoSidedEdges->end()) {
                continue;
            }

            const auto neighborNode = pInitialNodes->find(neighborLabel);
            if (neighborNode == pInitialNodes->end()) {
                throw std::logic_error(
                    "Cannot build a two-sided initial edge because the neighboring initial node is missing.");
            }
            const auto oppositeSide = neighborNode->second->onesidedEdges.find(sourceLabel);
            if (oppositeSide == neighborNode->second->onesidedEdges.end()) {
                throw std::logic_error(
                    "Cannot build a two-sided initial edge because the opposite one-sided initial edge is missing.");
            }
            if (sourceSide == nullptr || oppositeSide->second == nullptr) {
                throw std::logic_error("Cannot build a two-sided initial edge from a null one-sided initial edge.");
            }

            oneSidedInitialEdgePairsToMerge.push_back(
                {canonicalLabelPair, sourceSide.get(), oppositeSide->second.get()});
        }
    }
    const double collectEdgePairsMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    phaseTimer.restart();
    std::sort(
        oneSidedInitialEdgePairsToMerge.begin(),
        oneSidedInitialEdgePairsToMerge.end(),
        [](const OneSidedInitialEdgePair &left, const OneSidedInitialEdgePair &right) {
            return left.labelPair < right.labelPair;
        });
    const double sortEdgePairsMs = static_cast<double>(phaseTimer.nsecsElapsed()) / 1000000.0;

    const int requestedThreadCount = std::max(1, threadCount);
    const bool edgeFeaturesEnabled = !FeatureList::edgeFeaturesList.empty();
    bool buildInParallel = false;
#ifdef USE_OMP
    buildInParallel = requestedThreadCount > 1 &&
                      oneSidedInitialEdgePairsToMerge.size() > 1 &&
                      !edgeFeaturesEnabled;
#endif

    if (requestedThreadCount > 1 && edgeFeaturesEnabled) {
        logSegmentManager(
            LogLevel::Warning,
            __func__,
            QStringLiteral("Using serial two-sided initial-edge construction because edge feature prototypes are active"));
    }
#ifndef USE_OMP
    if (requestedThreadCount > 1) {
        logSegmentManagerDebugIf(
            logTiming,
            __func__,
            QStringLiteral("Using serial two-sided initial-edge construction because OpenMP is disabled"));
    }
#endif

    std::vector<std::unique_ptr<InitialEdge>> twoSidedInitialEdgesToRegister(
        oneSidedInitialEdgePairsToMerge.size());
    phaseTimer.restart();
    int usedThreadCount = 1;
#ifdef USE_OMP
    if (buildInParallel) {
        std::exception_ptr firstException;
        std::mutex exceptionMutex;
        std::atomic<bool> stopRequested{false};

#pragma omp parallel num_threads(requestedThreadCount) default(none) \
    shared(oneSidedInitialEdgePairsToMerge, twoSidedInitialEdgesToRegister, firstException, \
           exceptionMutex, stopRequested, usedThreadCount)
        {
#pragma omp single
            usedThreadCount = omp_get_num_threads();

#pragma omp for schedule(guided)
            for (long long index = 0;
                 index < static_cast<long long>(oneSidedInitialEdgePairsToMerge.size());
                 ++index) {
                if (stopRequested.load(std::memory_order_relaxed)) {
                    continue;
                }
                try {
                    twoSidedInitialEdgesToRegister[static_cast<std::size_t>(index)] =
                        createTwoSidedInitialEdge(
                            oneSidedInitialEdgePairsToMerge[static_cast<std::size_t>(index)]);
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
    } else
#endif
    {
        for (std::size_t index = 0; index < oneSidedInitialEdgePairsToMerge.size(); ++index) {
            twoSidedInitialEdgesToRegister[index] =
                createTwoSidedInitialEdge(oneSidedInitialEdgePairsToMerge[index]);
        }
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
    for (auto &twoSidedInitialEdge : twoSidedInitialEdgesToRegister) {
        addTwoSidedInitialEdge(std::move(twoSidedInitialEdge));
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
            .arg(static_cast<qulonglong>(oneSidedInitialEdgePairsToMerge.size()))
            .arg(requestedThreadCount)
            .arg(usedThreadCount)
            .arg(buildInParallel ? QStringLiteral("yes") : QStringLiteral("no"))
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
        newLabel = *nextFreeId;
        (*nextFreeId)++;
    }


    auto *newWorkingNode = new WorkingNode(baseInitialNode, newLabel, *pInitialNodes);
    addWorkingNode(newWorkingNode);
}


bool SegmentManager::isIgnoredId(SegmentIdType idToCheck) {
    return (std::find(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end(), idToCheck) !=
            pIgnoredSegmentLabels->end());
}

void SegmentManager::recalculateEdgesOnWorkingNode(WorkingNode *pWorkingNode, bool veryVerbose) {
    ScopedSegmentManagerTimer timer(veryVerbose, __func__, QStringLiteral("Recalculating edges on working node"));
    // de-register old edges and clear twosided edges
    // also deregister the edge from the other node

    std::vector<WorkingEdge *> workingEdgesToDelete =
            utils::getTargetPointersVecOfSharedPtrMap<SegmentIdType, WorkingEdge>(pWorkingNode->twosidedEdges);
    for (auto edge : workingEdgesToDelete) {
        removeWorkingEdge(edge);
    }


    SegmentIdType label = pWorkingNode->getLabel();
    // create new twosided edges, will look if its exist already
    for (auto &subInitialNode : pWorkingNode->subInitialNodes) {
        for (auto &initialEdge : subInitialNode.second->twosidedEdges) {
            SegmentIdType workingLabelOfConnectedNode = pInitialNodes->at(
                    initialEdge.first)->getCurrentWorkingNodeLabel();
            if (workingLabelOfConnectedNode != label) { // if it is not an "internal" edge
                if (pWorkingNode->twosidedEdges.count(workingLabelOfConnectedNode) == 0) {
                    auto newWorkingEdge = new WorkingEdge(initialEdge.second, label, workingLabelOfConnectedNode);
                    addWorkingEdge(newWorkingEdge);
                } else {
                    pWorkingNode->twosidedEdges[workingLabelOfConnectedNode]->subInitialEdges.push_back(
                            initialEdge.second);
                }
            }
        }
    }
}

void SegmentManager::buildWorkingGraphFromInitialGraph() {
    ScopedSegmentManagerTimer timer(verbose, __func__, QStringLiteral("Building working graph from initial graph"));

    if (!pWorkingNodes->empty() || !pWorkingEdges->empty()) {
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
    for (const auto &[edgePair, initialEdge] : *pInitialTwoSidedEdges) {
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
            .arg(static_cast<qulonglong>(pInitialTwoSidedEdges->size()))
            .arg(static_cast<qulonglong>(pWorkingNodes->size()))
            .arg(static_cast<qulonglong>(pWorkingEdges->size()))
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
            utils::getTargetPointersVecOfSharedPtrMap<SegmentIdType, WorkingEdge>(workingNodeToRemove->twosidedEdges);
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
        for (const auto &[neighborLabel, edge] : initialNode->onesidedEdges) {
            outStream << "key: " << sourceLabel << "," << neighborLabel << "\n";
            edge->print(nodeIndentationLevel, outStream);
        }
    }
}

void SegmentManager::printInitialTwoSidedEdges(std::ostream &outStream) {
    outStream << "=== initialTwoSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : *pInitialTwoSidedEdges) {
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
    for (auto &edge : *pWorkingEdges) {
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
