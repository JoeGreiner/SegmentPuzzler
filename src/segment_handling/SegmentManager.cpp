

#include "SegmentManager.h"
#include <src/utils/utils.h>
#include <queue>

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


void SegmentManager::addOneSidedInitialEdge(std::shared_ptr<InitialEdge> pEdgeToAdd, EdgePairIdType pairId) {
    (*pInitialOneSidedEdges)[pairId] = pEdgeToAdd;
}


void SegmentManager::addTwoSidedInitialEdge(InitialEdge *pEdgeToAdd) {
    graphBase->edgeCounter++;
    size_t newEdgeId = graphBase->edgeCounter;
    pEdgeToAdd->setIdAndRegister(newEdgeId, *pInitialEdgeIdLookUp);
    auto sPEdgeToAdd = std::shared_ptr<InitialEdge>(pEdgeToAdd);
    pInitialNodes->at(pEdgeToAdd->getLabelSmaller())->addTwoSidedEdge(sPEdgeToAdd);
    pInitialNodes->at(pEdgeToAdd->getLabelBigger())->addTwoSidedEdge(sPEdgeToAdd);
    (*pInitialTwoSidedEdges)[pEdgeToAdd->pairId] = sPEdgeToAdd;

    char defaultEdgeStatus = 0;
    pEdgeStatus->insert(std::pair<EdgeNumIdType, char>(pEdgeToAdd->numId, defaultEdgeStatus));
    for (auto &voxel : pEdgeToAdd->voxels) {
        graphBase->pEdgesInitialSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, pEdgeToAdd->numId);
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
    double t = 0;
    if (verbose) {
        std::string desc = "Graph::removeInitialNode (" + std::to_string(nodeToRemove->getLabel()) + ") called";
        t = utils::tic(desc);
    }
    // take care of: initialtwosided edges, initial onesided edges, edgeidlookup, initialnodes, initialEdgeVolume, edgeStatus

    removeEdgePropertiesOnInitialNode(nodeToRemove);
    pInitialNodes->erase(nodeToRemove->getLabel());

    if (verbose) { utils::toc(t, "Graph::removeInitialNode finished"); }
}

void SegmentManager::removeEdgePropertiesOnInitialNode(InitialNode *pInitialNode) {

    int backgroundValueEdges = 0;
    for (auto initialEdge : pInitialNode->twosidedEdges) {
        // set voxels in the initial edge volume to background
        std::cout << "Removing Voxels of edgenum: " << initialEdge.second->numId << "\n";
        for (auto &voxel : initialEdge.second->voxels) {
            (*ppEdgesInitialSegmentsImage)->SetPixel({voxel.x, voxel.y, voxel.z}, backgroundValueEdges);
        }

        // delete edge status
        if (pEdgeStatus->count(initialEdge.second->numId) > 0) {
            std::cout << "Removing status of edgenum: " << initialEdge.second->numId << "\n";
            pEdgeStatus->erase(initialEdge.second->numId);
        }

        // take care of twosided edges
        if (pInitialTwoSidedEdges->count(initialEdge.second->pairId) > 0) {
            std::cout << "Removing InitialTwoSidedEdge: " << initialEdge.second->pairId.first << " -> "
                      << initialEdge.second->pairId.second << "\n";
            pInitialTwoSidedEdges->erase(initialEdge.second->pairId);
        }
        // also delete from the paired node
        if (pInitialNodes->count(initialEdge.first) > 0) {
            if ((*pInitialNodes)[initialEdge.first]->twosidedEdges.count(pInitialNode->getLabel()) > 0) {
                std::cout << "Removing twosided InitialEdge: " << initialEdge.first << " -> "
                          << pInitialNode->getLabel()
                          << "\n";
                (*pInitialNodes)[initialEdge.first]->twosidedEdges.erase(pInitialNode->getLabel());
            }
        }

        // delete corrosponding edgeidlookup
        if (pInitialEdgeIdLookUp->count(initialEdge.second->numId) > 0) {
            std::cout << "Removing Edgestatus of id: " << initialEdge.second->numId << "\n";
            pInitialEdgeIdLookUp->erase(initialEdge.second->numId);
        }
    }
    pInitialNode->twosidedEdges.clear();

    // delete onesided edges
    for (auto edge : pInitialNode->onesidedEdges) {
        if (pInitialOneSidedEdges->count({edge.first, pInitialNode->getLabel()}) > 0) {
            std::cout << "Removing onesided InitialEdge: " << edge.first << " -> " << pInitialNode->getLabel() << "\n";
            pInitialOneSidedEdges->erase({edge.first, pInitialNode->getLabel()});
        }
        if (pInitialOneSidedEdges->count({pInitialNode->getLabel(), edge.first}) > 0) {
            std::cout << "Removing onesided InitialEdge: " << pInitialNode->getLabel() << " -> " << edge.first << "\n";
            pInitialOneSidedEdges->erase({pInitialNode->getLabel(), edge.first});
        }
        if ((*pInitialNodes)[edge.first]->onesidedEdges.count(pInitialNode->getLabel()) > 0) {
            std::cout << "Removing onesided InitialEdge on node: " << edge.first << " -> " << pInitialNode->getLabel()
                      << "\n";
            (*pInitialNodes)[edge.first]->onesidedEdges.erase(pInitialNode->getLabel());
        }
    }
    pInitialNode->onesidedEdges.clear();
}


void SegmentManager::clearAndReserveInitialNodes(int numberOfNodesToReserveFor) {
    pInitialNodes->clear();
    if (numberOfNodesToReserveFor > 0) {
        pInitialNodes->reserve(numberOfNodesToReserveFor);
    }
}


void SegmentManager::removeWorkingEdge(WorkingEdge *workingEdgeToRemove) {
    EdgePairIdType pairId = workingEdgeToRemove->pairId;
    SegmentIdType labelA = pairId.first;
    SegmentIdType labelB = pairId.second;

    if (pWorkingNodes->count(labelA) > 0) { // if workingNode exist
        if (pWorkingNodes->at(labelA)->twosidedEdges.count(labelB) > 0) { // if twosided edge exists
            pWorkingNodes->at(labelA)->twosidedEdges.erase(labelB); // delete it
        } else {
            std::cout << "Warning: node with dangeling edge!!\n";
        }
    } else {
        std::cout << "Warning: Edge with dangeling node!!\n";
    }

    if (pWorkingNodes->count(labelB) > 0) { // if workingNode exist
        if (pWorkingNodes->at(labelB)->twosidedEdges.count(labelA) > 0) { // if twosided edge exists
            pWorkingNodes->at(labelB)->twosidedEdges.erase(labelA); // delete it
        } else {
            std::cout << "Warning: node with dangeling edge!!\n";
        }
    } else {
        std::cout << "Warning: Edge with dangeling node!!\n";
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
                addOneSidedInitialEdge(pNewEdge, {edge.first, pInitialNode->getLabel()});
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
    double t = 0;
    if (verbose) {
        std::cout << "Graph::splitWorkingNodeIntoInitialNodes called: (" << workingNodeIdToSplit << ")\n";
        t = utils::tic();
    }
    if (!isIgnoredId(workingNodeIdToSplit)) {

        auto pToWorkingNode = pWorkingNodes->at(workingNodeIdToSplit);
        std::cout << "here" << "\n";

        std::vector<SegmentIdType> idsOfSubInitialNodes;
        for (auto &initialNode : pToWorkingNode->subInitialNodes) {
            idsOfSubInitialNodes.push_back(initialNode.first);
        }

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
    if (verbose) { utils::toc(t, "Graph::splitWorkingNodeIntoInitialNodes finished"); }

}


void SegmentManager::splitIntoConnectedComponentsOfWorkingNode(
        WorkingNode &workingNodeToAnalyze) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::splitIntoConnectedComponentsOfWorkingNode called"); }


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

        std::cout << "Graph::splitIntoConnectedComponentsOfWorkingNode visited Labels this round: ";
        for (auto &val : visitedWorkingNodeLabelsThisRun) {
            std::cout << val << " ";
        }
        std::cout << "\n";

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
    if (verbose) { utils::toc(t, "splitIntoConnectedComponentsOfWorkingNode finished"); }
}


void SegmentManager::computeSurfaceAndOneSidedEdgesOnInitialNode(InitialNode *pInitialNode) {
    double t = 0;
    if (verbose) { t = utils::tic("SegmentManager::computeSurfaceAndOneSidedEdgesOnInitialNode called"); }
    pInitialNode->parallelComputeOnesidedSurfaceAndEdges(pIgnoredSegmentLabels);

    for (auto &edge : pInitialNode->onesidedEdges) {
        addOneSidedInitialEdge(edge.second, {pInitialNode->getLabel(), edge.first});
    }
    if (verbose) { utils::toc(t, "SegmentManager::computeSurfaceAndOneSidedEdgesOnInitialNode finished"); }
}

void SegmentManager::computeCorrospondingOneSidedInitialEdges(InitialNode *pInitialNode) {
    double t = 0;
    if (verbose) { t = utils::tic("SegmentManager::computeCorrospondingOneSidedInitialEdges called"); }
    auto labelOfThisInitialNode = pInitialNode->getLabel();
    for (auto &edge : pInitialNode->onesidedEdges) {
        // TODO: this is ugly, add a function to add the initial edge!
        auto labelOfConnectedInitialNode = edge.first;

        auto pNewEdge = std::shared_ptr<InitialEdge>(pInitialNode->computeCorrospondingOneSidedEdge(edge.second.get()));

        if  ((*pInitialNodes).count(labelOfConnectedInitialNode) == 0){
            std::cout << "connected initial node: " << labelOfConnectedInitialNode << " is not in initial nodes!!" << std::endl;
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

        addOneSidedInitialEdge(pNewEdge, {labelOfConnectedInitialNode, labelOfThisInitialNode});
    }
    if (verbose) { utils::toc(t, "SegmentManager::computeCorrospondingOneSidedInitialEdges finished"); }
}


void SegmentManager::computeSurfaceAndOneSidedEdgesOnAllInitialNodes() {
    double t = 0;
    if (verbose) { t = utils::tic("SegmentManager::computeOneSidedEdgesOnAllInitialNodes called"); }


    std::vector<SegmentIdType> initialNodeIds = utils::getKeyVecOfSharedPtrMap<SegmentIdType>(*pInitialNodes);
//#pragma omp parallel for schedule(dynamic) default(none) shared(initialNodeIds)
    for (long long i = 0; i < static_cast<long long>(initialNodeIds.size()); ++i) {
        (*pInitialNodes)[initialNodeIds[i]]->parallelComputeOnesidedSurfaceAndEdges(pIgnoredSegmentLabels);
    }
//#pragma omp barrier


    for (auto &initialNode : *pInitialNodes) {
        for (auto &edge : initialNode.second->onesidedEdges) {
            addOneSidedInitialEdge(edge.second, {initialNode.first, edge.first});
        }
    }

    if (verbose) { utils::toc(t, "SegmentManager::computeOneSidedEdgesOnAllInitialNodes finished"); }
}


InitialEdge *
SegmentManager::createTwoSidedInitialEdgeByMerging(SegmentIdType initialNodeLabelA, SegmentIdType initialNodeLabelB) {
    auto pEdgeA = (*pInitialOneSidedEdges)[{initialNodeLabelA, initialNodeLabelB}];
    auto pEdgeB = (*pInitialOneSidedEdges)[{initialNodeLabelB, initialNodeLabelA}];
    auto newEdge = new InitialEdge(*pEdgeA);
    newEdge->mergeVoxelsAndROIwithOtherEdge(pEdgeB.get());
    pEdgeA->setWasUsedToComputeTwoSidedEdge(true);
    pEdgeB->setWasUsedToComputeTwoSidedEdge(true);
    return newEdge;
}


void SegmentManager::mergeNewOneSidedEdgesIntoTwosidedEdges(bool veryVerbose) {
    double t = 0;
    if (veryVerbose) { t = utils::tic("SegmentManager::mergeNewOneSidedEdgesIntoTwosidedEdges called"); }

    std::vector<InitialEdge *> newlyAddedInitialEdges;
    for (auto &initialNode: *pInitialNodes) {
        for (auto &initialEdge : initialNode.second->onesidedEdges) {
            if (!initialEdge.second->getWasUsedToComputeTwoSidedEdge()) {
                // if no edge exists, create a new one
                SegmentIdType labelSmaller = initialEdge.second->getLabelSmaller();
                SegmentIdType labelBigger = initialEdge.second->getLabelBigger();
                if (veryVerbose) {
                    std::cout << "Merging onesided edges: " << labelSmaller << " " << labelBigger << "\n";
                }
                InitialEdge *newEdge = createTwoSidedInitialEdgeByMerging(labelSmaller, labelBigger);
                addTwoSidedInitialEdge(newEdge);
                newlyAddedInitialEdges.push_back(newEdge);
            }
        }
    }

// not save to parallelize, dataraces
//#pragma omp parallel for schedule(dynamic) default(none) shared(newlyAddedInitialEdges)
    for (long long i = 0; i < static_cast<long long>(newlyAddedInitialEdges.size()); ++i) {
        newlyAddedInitialEdges[i]->calculateEdgeFeatures();
    }

    if (veryVerbose) { utils::toc(t, "SegmentManager::mergeNewOneSidedEdgesIntoTwosidedEdges finished"); }
}


void
SegmentManager::constructWorkingNodeFromInitialNode(InitialNode *baseInitialNode, bool useSameIdAsInitialNode,
                                                    bool veryVerbose) {
    double t = 0;
    if (veryVerbose) { t = utils::tic("SegmentManager::constructWorkingNodeFromInitialNode called"); }

    SegmentIdType newLabel;
    if (useSameIdAsInitialNode) {
        newLabel = baseInitialNode->getLabel();
    } else {
        newLabel = *nextFreeId;
        (*nextFreeId)++;
    }


    auto *newWorkingNode = new WorkingNode(baseInitialNode, newLabel, *pInitialNodes);
    addWorkingNode(newWorkingNode);

    if (veryVerbose) { utils::toc(t, "SegmentManager::mergeOneSidedEdgesIntoTwosidedEdges finished"); }
}


bool SegmentManager::isIgnoredId(SegmentIdType idToCheck) {
    return (std::find(pIgnoredSegmentLabels->begin(), pIgnoredSegmentLabels->end(), idToCheck) !=
            pIgnoredSegmentLabels->end());
}

void SegmentManager::recalculateEdgesOnWorkingNode(WorkingNode *pWorkingNode, bool veryVerbose) {
    double t = 0;
    if (veryVerbose) { t = utils::tic("SegmentManager::recalculateEdgesOnWorkingNode called\n"); }
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
    if (veryVerbose) { utils::toc(t, "SegmentManager::mergeOneSidedEdgesIntoTwosidedEdges finished"); }
}

void SegmentManager::convertAllInitialNodesIntoWorkingNodes() {
    double t = 0;
    if (verbose) {
        t = utils::tic("SegmentManager::mergeOneSidedEdgesIntoTwosidedEdges called\n");
    }

    for (auto &initialNode : *pInitialNodes) {
        constructWorkingNodeFromInitialNode(initialNode.second.get());
    }

    for (auto &workingNode : *pWorkingNodes) {
        recalculateEdgesOnWorkingNode(workingNode.second.get());
    }


    if (verbose) { utils::toc(t, "SegmentManager::mergeOneSidedEdgesIntoTwosidedEdges finished"); }
}

void SegmentManager::removeWorkingNode(WorkingNode *workingNodeToRemove, bool veryVerbose) {
    double t = 0;
    if (veryVerbose) {
        t = utils::tic(
                "SegmentManager::removeWorkingNode (" + std::to_string(workingNodeToRemove->getLabel()) + ") called");
    }
    // deregister edges
    std::vector<WorkingEdge *> vecOfWorkingEdgesToRemove =
            utils::getTargetPointersVecOfSharedPtrMap<SegmentIdType, WorkingEdge>(workingNodeToRemove->twosidedEdges);
    for (auto pWorkingEdge : vecOfWorkingEdgesToRemove) {
        removeWorkingEdge(pWorkingEdge);
    }
    // deregeister Node
    pWorkingNodes->erase(workingNodeToRemove->getLabel());
    if (veryVerbose) { utils::toc(t, "SegmentManager::removeWorkingNode finished"); }

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
    for (auto &edge : *pInitialOneSidedEdges) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        edge.second->print(nodeIndentationLevel, outStream);
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
    std::cout << "Printing initialNodes to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printInitialNodes(outFile);
    outFile.close();
}

void SegmentManager::printInitialTwoSidedEdgesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing initialTwoSidedEdges to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printInitialTwoSidedEdges(outFile);
    outFile.close();
}

void SegmentManager::printInitialOneSidedEdgesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing initialTwoSidedEdges to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printInitialOneSidedEdges(outFile);
    outFile.close();
}

void SegmentManager::printWorkingNodesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing workingNodes to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printWorkingNodes(outFile);
    outFile.close();
}

void SegmentManager::printEdgeIdLookUpToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing edgeIdLookup to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printEdgeIdLookUp(outFile);
    outFile.close();
}

void SegmentManager::printWorkingEdgesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing workingEdges to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printWorkingEdges(outFile);
    outFile.close();
}

