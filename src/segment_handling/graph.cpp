#include <itkImageRegionConstIterator.h>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <fstream>
#include "graph.h"
#include <itkImageFileWriter.h>
#include <queue>
#include <itkNeighborhoodIterator.h>
#include <itkBinaryThresholdImageFunction.h>
#include <QMessageBox>
#include "src/utils/utils.h"
#include "graphBase.h"
#include <QtConcurrent/QtConcurrent>
#include <QtWidgets/QProgressDialog>
#include <src/qtUtils/QBackgroundIdRadioBox.h>
#include <src/qtUtils/QImageSelectionRadioButtons.h>

Graph::Graph(std::shared_ptr<GraphBase> graphBaseIn, bool verboseIn)  : QWidget(){
    verbose = verboseIn;
    nextFreeId = 0;
    pIgnoredSegmentLabels = nullptr;
    backgroundIdStrategy = "backgroundIsLowestId";
    backgroundId = 0;

    graphBase = graphBaseIn;

    initialNodes = std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>>();
    initialOneSidedEdges = std::map<EdgePairIdType, std::shared_ptr<InitialEdge>>();
    initialTwoSidedEdges = std::map<EdgePairIdType, std::shared_ptr<InitialEdge>>();
    initialEdgeIdLookup = std::unordered_map<EdgeNumIdType, EdgePairIdType>();
    workingNodes = std::unordered_map<SegmentIdType, std::shared_ptr<WorkingNode>>();
    workingEdges = std::map<EdgePairIdType, std::shared_ptr<WorkingEdge>>();

    segmentManager = SegmentManager(graphBase, &initialNodes, &initialOneSidedEdges, &initialTwoSidedEdges,
                                    &initialEdgeIdLookup, &graphBase->colorLookUpEdgesStatus, &graphBase->edgeStatus,
                                    &graphBase->pEdgesInitialSegmentsImage, &graphBase->pWorkingSegmentsImage,
                                    &workingNodes, &workingEdges,
                                    pIgnoredSegmentLabels, &nextFreeId);
}

void Graph::askForBackgroundStrategy(){
    dialog = new QBackgroundIdRadioBox();
    QObject::connect(dialog, &QBackgroundIdRadioBox::sendBackgroundIdStrategy, this, &Graph::receiveBackgroundIdStrategy);
    dialog->exec();
}

void Graph::constructFromVolume(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    initializeEdgeVolumeAndEdgeStatus();

    //TODO: Implement none/guess
    std::cout << "BackgroundIdStrategy: " << backgroundIdStrategy << "\n";
    if(backgroundIdStrategy == "backgroundIsHighestId"){
        backgroundId = getLargestSegmentId(pImage);
    } else if  (backgroundIdStrategy == "backgroundIsLowestId"){
        backgroundId = getSmallestSegmentId(pImage);
    } else {
        throw std::invalid_argument("Received unknown backgroundIdStrategy in Graph::constructFromVolume");
    }//TODO: Implement: No Background
    pIgnoredSegmentLabels->push_back(backgroundId);


    nextFreeId = getNextFreeId(pImage);
    constructInitialNodes(pImage);
    segmentManager.computeSurfaceAndOneSidedEdgesOnAllInitialNodes();
    segmentManager.mergeNewOneSidedEdgesIntoTwosidedEdges();
    graphBase->pEdgesInitialSegmentsITKSignal->computeExtrema();
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    segmentManager.convertAllInitialNodesIntoWorkingNodes();
}

void Graph::constructInitialNodes(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    double t = 0, tBig = 0;
    if (verbose) { tBig = utils::tic("Graph::constructInitialNodes2 called"); }
    if (verbose) { t = utils::tic("Graph::constructInitialNodes2:Loop1 called"); }

    // get histogram of label ids to preallocate voxel array sizes
    std::vector<int> segmentIdHistogram(nextFreeId, 0);
    {
        itk::ImageRegionConstIterator<SegmentsImageType> it(pImage, pImage->GetLargestPossibleRegion());
        it.GoToBegin();
        while (!it.IsAtEnd()) {
            segmentIdHistogram[it.Get()]++;
            ++it;
        }
    }

    if (verbose) { utils::toc(t, "Graph::constructInitialNodes2:Loop1 finished"); }
    if (verbose) { t = utils::tic("Graph::constructInitialNodes2:Loop2 called"); }

    std::vector<std::shared_ptr<InitialNode>> initialNodesVec; // vector use instead of a map to save lookup time
    initialNodesVec.resize(nextFreeId);
    segmentManager.clearAndReserveInitialNodes(nextFreeId);
    for (unsigned int i = 0; i < segmentIdHistogram.size(); i++) {
        if (segmentIdHistogram[i] > 0) {
            segmentManager.addInitialNode(i, segmentIdHistogram[i]);
            initialNodesVec[i] = initialNodes[i];
        }
    }

    if (verbose) { utils::toc(t, "Graph::constructInitialNodes2:Loop2 finished"); }
    if (verbose) { t = utils::tic("Graph::constructInitialNodes2:Loop3 called"); }


    {
        const SegmentIdType* imageBuffer = pImage->GetBufferPointer();
        const itk::ImageRegion<3>& region = pImage->GetLargestPossibleRegion();
        const itk::Size<3>& size = region.GetSize();
        std::vector<std::vector<Voxel>> labelVoxels(nextFreeId);
//        reserve vector space
        for (unsigned int i = 0; i < nextFreeId; ++i) {
            labelVoxels[i].reserve(segmentIdHistogram[i]);
        }

        for (unsigned int z = 0; z < size[2]; ++z) {
            for (unsigned int y = 0; y < size[1]; ++y) {
                for (unsigned int x = 0; x < size[0]; ++x) {
                    unsigned long linearIndex = (z * size[1] * size[0]) + (y * size[0]) + x;
                    SegmentIdType label = imageBuffer[linearIndex];
                    labelVoxels[label].emplace_back(x, y, z);
                }
            }
        }

        for (SegmentIdType label = 0; label < nextFreeId; ++label) {
            if (!labelVoxels[label].empty()) {
                initialNodesVec[label]->voxels = std::move(labelVoxels[label]);
            }
        }
    }
//    {
//        itk::ImageRegionConstIteratorWithIndex<SegmentsImageType> it(pImage, pImage->GetLargestPossibleRegion());
//        it.GoToBegin();
//        while (!it.IsAtEnd()) {
//            initialNodesVec[it.Get()]->addVoxel(it.GetIndex());
//            ++it;
//        }
//    }

    if (verbose) { utils::toc(t, "Graph::constructInitialNodes2:Loop3 finished"); }
    if (verbose) { t = utils::tic("Graph::constructInitialNodes2:Loop4 called"); }

    for (unsigned int i = 0; i < initialNodesVec.size(); ++i) {
        if (segmentIdHistogram[i] > 0) {
            if (isIgnoredId(i)) {
                segmentManager.removeInitialNode(i);
            }
            initialNodesVec[i]->roi.updateBoundingRoi(initialNodesVec[i]->voxels);
        }
    }

    if (verbose) { utils::toc(t, "Graph::constructInitialNodes2:Loop4 finished"); }
    if (verbose) { t = utils::tic("Graph::constructInitialNodes2:calculateNodeFeatures called"); }


    std::vector<SegmentIdType> idsOfInitialNodes = utils::getKeyVecOfSharedPtrMap<SegmentIdType>(initialNodes);
//#pragma omp parallel for schedule(dynamic)
    for (long long i = 0; i < static_cast<long long>(idsOfInitialNodes.size()); i++) {
        initialNodes[idsOfInitialNodes[i]]->calculateNodeFeatures();
    }

    if (verbose) { utils::toc(t, "Graph::constructInitialNodes2:calculateNodeFeatures finished"); }
    if (verbose) { utils::toc(tBig, "Graph::constructInitialNodes2 finished"); }
}


void Graph::initializeEdgeVolumeAndEdgeStatus() {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::initializeEdgeVolumeAndEdgeStatus called"); }
    graphBase->pEdgesInitialSegmentsImage = GraphBase::EdgeImageType::New();
    graphBase->pEdgesInitialSegmentsImage->SetRegions(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
    graphBase->pEdgesInitialSegmentsImage->Allocate(true);

    graphBase->colorLookUpEdgesStatus.insert(
            std::pair<char, std::vector<unsigned char>>(0, {255, 255, 255, 255}));
    graphBase->colorLookUpEdgesStatus.insert(
            std::pair<char, std::vector<unsigned char>>(1, {0, 0, 255, 255}));
    graphBase->colorLookUpEdgesStatus.insert(
            std::pair<char, std::vector<unsigned char>>(-1, {255, 255, 0, 255}));
    graphBase->colorLookUpEdgesStatus.insert(
            std::pair<char, std::vector<unsigned char>>(-2, {255, 0, 0, 255}));
    graphBase->colorLookUpEdgesStatus.insert(
            std::pair<char, std::vector<unsigned char>>(2, {0, 255, 0, 255}));


    graphBase->pEdgesInitialSegmentsITKSignal = new itkSignal<dataType::MappedEdgeIdType>(
            graphBase->pEdgesInitialSegmentsImage);
    graphBase->pEdgesInitialSegmentsITKSignal->setLUTEdgeMap(&graphBase->edgeStatus,
                                                             &graphBase->colorLookUpEdgesStatus);
    if (verbose) { utils::toc(t, "Graph::initializeEdgeVolumeAndEdgeStatus finished"); }
}


Graph::SegmentIdType Graph::getSmallestSegmentId(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::getSmallestSegmentId called"); }
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
    if (verbose) { std::cout << "Smallest Id: " << smallestId << "\n"; }
    if (verbose) { utils::toc(t, "Graph::getSmallestSegmentId finished"); }
    return smallestId;
}

Graph::SegmentIdType Graph::getLargestSegmentId(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::getLargestSegmentId called"); }
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
    if (verbose) { std::cout << "Largest Id: " << largestId << "\n"; }
    if (verbose) { utils::toc(t, "Graph::getLargestSegmentId finished"); }
    return largestId;
}


Graph::SegmentIdType Graph::getNextFreeId(itk::Image<Graph::SegmentIdType, 3>::Pointer pImage) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::getNextFreeId called"); }

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
    if (verbose) { std::cout << "NextFreeId: " << largestId + 1 << "\n"; }
    if (verbose) { utils::toc(t, "Graph::getNextFreeId finished"); }
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


void Graph::mergeEdges(std::set<EdgeNumIdType> &vecOfEdgeIdsToMerge) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::mergeEdges called"); }

    EdgePairIdType pairId;
    bool updateSegmentImage = false;     // optimization: only update the resulting segments at the end, do not update incrementally
    for (auto &numId : vecOfEdgeIdsToMerge) {
        pairId = initialEdgeIdLookup[numId];
        mergeEdge(initialTwoSidedEdges.at(pairId).get(), updateSegmentImage);
    }

    std::set<SegmentIdType> newWorkingNodeIds;
    for (auto &numId : vecOfEdgeIdsToMerge) {
        pairId = initialEdgeIdLookup[numId];
        SegmentIdType initialNodeLabel = initialTwoSidedEdges[pairId]->getLabelSmaller();
        SegmentIdType workingNodeLabel = initialNodes[initialNodeLabel]->getCurrentWorkingNodeLabel();
        newWorkingNodeIds.insert(workingNodeLabel);
    }

    for (auto &label : newWorkingNodeIds) {
        insertWorkingNodeInSegmentImage(*workingNodes[label]);
    }
    if (verbose) { utils::toc(t, "Graph::mergeEdges finished"); }
}

void Graph::mergeEdge(InitialEdge *edge, bool updateSegmentImage) {
    double t = 0;
//    if (verbose) { t = utils::tic("Graph::mergeEdge called"); }

    SegmentIdType idWorkingNodeA = initialNodes[edge->getLabelSmaller()]->getCurrentWorkingNodeLabel();
    SegmentIdType idWorkingNodeB = initialNodes[edge->getLabelBigger()]->getCurrentWorkingNodeLabel();

    if (idWorkingNodeA != idWorkingNodeB) {
        SegmentIdType idOfNewNode = nextFreeId;
        nextFreeId++;
        if (verbose) {
//            std::cout << "Merging W.Nodes: (" << idWorkingNodeA << "," << idWorkingNodeB << ") -> "
//                      << idOfNewNode << "\n";
        }

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
    double t = 0;
    if (verbose) { t = utils::tic("Graph::insertWorkingNodeInSegmentImage called"); }

//    auto voxelList = pWorkingNode.getVoxelArray();
    auto voxelListPtr = pWorkingNode.getVoxelPointerArray();
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

    if (verbose) { utils::toc(t, "Graph::insertWorkingNodeInSegmentImage finished"); }
}


void Graph::unmergeEdges(std::set<EdgeNumIdType> &vecOfEdgeIdsToUnMerge) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::unmergeEdges called"); }

    for (auto &val : vecOfEdgeIdsToUnMerge) {
        std::cout << val << "\n";
    }

    EdgePairIdType pairId;
    SegmentIdType labelSmaller, labelBigger, currentWorkingNodeIdA, currentWorkingNodeIdB;
    for (auto &numId : vecOfEdgeIdsToUnMerge) {
        pairId = initialEdgeIdLookup[numId];

//         set status to no
        graphBase->edgeStatus[numId] = -2;
        initialTwoSidedEdges[pairId]->setShouldMergeNo();
//        graphBase->pEdgesInitialSegmentsITKSignal->updateLUTEdge((const std::set<unsigned int> &) numId);


        labelSmaller = initialTwoSidedEdges[pairId]->getLabelSmaller();
        labelBigger = initialTwoSidedEdges[pairId]->getLabelBigger();
        currentWorkingNodeIdA = initialNodes[labelSmaller]->getCurrentWorkingNodeLabel();
        currentWorkingNodeIdB = initialNodes[labelBigger]->getCurrentWorkingNodeLabel();


        if (currentWorkingNodeIdA == currentWorkingNodeIdB) {

            auto splitPair = calculateGraphDistancesFromEdge(workingNodes[currentWorkingNodeIdA].get(),
                                                             initialTwoSidedEdges[pairId].get());

            std::vector<SegmentIdType> initialNodeIdsSplitA = splitPair.first;
            std::vector<SegmentIdType> initialNodeIdsSplitB = splitPair.second;
            unmergeEdge(workingNodes[currentWorkingNodeIdA].get(), initialNodeIdsSplitA, initialNodeIdsSplitB);
//
//            LandscapeType::Pointer pLandscapeAB = generateLandscapePathfinding(GraphBase::pWorkingSegmentsImage,
//                                                                               currentWorkingNodeIdA,
//                                                                               *initialOneSidedEdges[{pairId.second,
//                                                                                                      pairId.first}]);
//            ITKImageWriter<LandscapeType>(pLandscapeAB, "landscapeAB.nrrd");
//            LandscapeType::Pointer pLandscapeBA = generateLandscapePathfinding(GraphBase::pWorkingSegmentsImage,
//                                                                               currentWorkingNodeIdA,
//                                                                               *initialOneSidedEdges[pairId]);
//            ITKImageWriter<LandscapeType>(pLandscapeBA, "landscabeBA.nrrd");
//
//            DistanceType::Pointer pDistanceAB = shortestPath(*initialOneSidedEdges[pairId], pLandscapeAB);
//            ITKImageWriter<DistanceType>(pDistanceAB, "distAB.nrrd");
//            DistanceType::Pointer pDistanceBA = shortestPath(*initialOneSidedEdges[{pairId.second, pairId.first}],
//                                                             pLandscapeBA);
//            ITKImageWriter<DistanceType>(pDistanceBA, "distBA.nrrd");
//
//            unmergeEdge(initialTwoSidedEdges[pairId].get(), pDistanceAB, pDistanceBA);
        }
//        graphBase->edgeStatus[numId] = -2;

    }

    if (verbose) { utils::toc(t, "Graph::unmergeEdges finished"); }
}


std::pair<std::vector<Graph::SegmentIdType>, std::vector<Graph::SegmentIdType>>
Graph::calculateGraphDistancesFromEdge(WorkingNode *nodeToCalculateDistanceOn,
                                       InitialEdge *edgeToCalculateDistanceFrom) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::calculateGraphDistancesFromEdge called"); }
    std::map<SegmentIdType, float> distA;
    std::map<SegmentIdType, float> distB;

    EdgeNumIdType forbiddenEdgeId = edgeToCalculateDistanceFrom->numId;

    SegmentIdType labelInitialNodeA = edgeToCalculateDistanceFrom->getLabelSmaller();
    SegmentIdType labelInitialNodeB = edgeToCalculateDistanceFrom->getLabelBigger();

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
            comSource = CenterOfMass(initialOneSidedEdges[{labelInitialNodeA, labelInitialNodeB}].get());
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
        for (auto edge : initialNodes[label]->twosidedEdges) {
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
            comSource = CenterOfMass(initialOneSidedEdges[{labelInitialNodeA, labelInitialNodeB}].get());
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
        for (auto edge : initialNodes[label]->twosidedEdges) {
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

    // print distA
    for (auto elem : distA) {
        std::cout << elem.first << " " << elem.second << "\n";
    }

    // print distB
    for (auto elem : distB) {
        std::cout << elem.first << " " << elem.second << "\n";
    }

    std::vector<SegmentIdType> initialNodeIdsSplitA;
    std::vector<SegmentIdType> initialNodeIdsSplitB;

    for (auto &elem : distA) {
        if (distA[elem.first] < distB[elem.first]) {
            initialNodeIdsSplitA.push_back(elem.first);
        } else if (distA[elem.first] > distB[elem.first]) {
            initialNodeIdsSplitB.push_back(elem.first);
        } else {
            std::cout << "WARNING: distA & B are the same!: " << distA[elem.first] << "\n";
            initialNodeIdsSplitB.push_back(elem.first);
        }
    }

    std::cout << "LabelsA: ";
    for (auto elem : initialNodeIdsSplitA) {
        std::cout << elem << " ";
    }
    std::cout << "\n";
    std::cout << "LabelsB: ";
    for (auto elem : initialNodeIdsSplitB) {
        std::cout << elem << " ";
    }
    std::cout << "\n";


    if (verbose) { utils::toc(t, "Graph::calculateGraphDistancesFromEdge finished"); }
    return {initialNodeIdsSplitA, initialNodeIdsSplitB};
}



// generate a landscape/mask from allowed segment ids
Graph::LandscapeType::Pointer
Graph::generateLandscapePathfinding(SegmentsImageType::Pointer pSegments, SegmentIdType allowedWorkingNodeLabel,
                                    InitialEdge &forbiddenEdge) {
    double t = 0;
    if (verbose) { t = utils::tic("Graph::generateLandscapePathfinding called"); }

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


    for (auto &voxel : forbiddenEdge.voxels) {
        pLandscape->SetPixel({voxel.x, voxel.y, voxel.z}, 0);
    }

    if (verbose) { utils::toc(t, "Graph::generateLandscapePathfinding finished"); }
    return pLandscape;
}

// find shortest path given a landscape/mask
itk::Image<short, 3>::Pointer Graph::shortestPath(InitialEdge &initialEdge,
                                                  LandscapeType::Pointer pLandscape) { //TODO: This is slow, make it fast! Fast Marching? Multithreaded?
    double t = 0;
    if (verbose) { t = utils::tic("Graph::shortestPath called"); }





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
    if (verbose) { utils::toc(t, "Graph::shortestPath finished"); }
    return pDistance;
}


void Graph::unmergeEdge(WorkingNode *workingNodeToSplit, std::vector<SegmentIdType> initialNodeIdsA,
                        std::vector<SegmentIdType> initialNodeIdsB) {
    //TODO: Handle cases where distance is inf/max, and assign them a own label (as they are not connected to the rest of the label at all)
    double t = 0;
    if (verbose) { t = utils::tic("Graph::unmergeEdge2 called"); }


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



    if (verbose) { utils::toc(t, "Graph::unmergeEdge2 finished"); }
}


void Graph::unmergeEdge(InitialEdge *initialEdge, DistanceType::Pointer pDistanceSmaller,
                        DistanceType::Pointer pDistanceBigger) {
    //TODO: Handle cases where distance is inf/max, and assign them a own label (as they are not connected to the rest of the label at all)
    double t = 0;
    if (verbose) { t = utils::tic("Graph::unmergeEdge called"); }

    SegmentIdType labelInitialNodeSmaller = initialEdge->getLabelSmaller();
    SegmentIdType labelInitialNodeBigger = initialEdge->getLabelBigger();
    SegmentIdType workingNodeSmaller = initialNodes[labelInitialNodeSmaller]->getCurrentWorkingNodeLabel();
    SegmentIdType workingNodeLarger = initialNodes[labelInitialNodeBigger]->getCurrentWorkingNodeLabel();
    //FIXME: has to be two nodes has to be forced!! uncomment the initialnode thingy
    //FIXME: Enforce Connectivity for segments after splitting? would be a good function to have anyway
    if (workingNodeLarger == workingNodeSmaller) {

        // this is an approx solution, best would be a geodesic distance to the edge
        SegmentIdType currentWorkingNodeLabel = workingNodeLarger;
        if (verbose) {
            std::cout << "Unmerging workinglabel: " << currentWorkingNodeLabel << "(InitialLabels Edge: "
                      << labelInitialNodeSmaller << "/" << labelInitialNodeBigger << ")\n";
        }
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



//                // distance onesided edge a->b
//                for (auto &voxel : initialOneSidedEdges[{labelInitialNodeSmaller, labelInitialNodeBigger}]->voxels) {
//                    distanceToEdgeSmaller += initialNode.second->roi.calculateMeanDistanceOfExtremeVoxelToPoint(voxel);
//                }
//                distanceToEdgeSmaller /= initialOneSidedEdges[{labelInitialNodeSmaller, labelInitialNodeBigger}]->getNumberVoxels();
//
//
//                // distance onesided edge b->a
//                for (auto &voxel : initialOneSidedEdges[{labelInitialNodeBigger, labelInitialNodeSmaller}]->voxels) {
//                    distanceToEdgeBigger += initialNode.second->roi.calculateMeanDistanceOfExtremeVoxelToPoint(voxel);
//                }
//                distanceToEdgeBigger /= initialOneSidedEdges[{labelInitialNodeBigger, labelInitialNodeSmaller}]->getNumberVoxels();

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

        std::cout << "LabelsSmaller size: " << labelsOfInitialNodesNearSmaller.size() << "\n";
        for (auto &el : labelsOfInitialNodesNearSmaller) {
            std::cout << " " << el;
        }
        std::cout << "\n";
        std::cout << "LabelsBigger size: " << labelsOfInitialNodesNearBigger.size() << "\n";
        for (auto &el : labelsOfInitialNodesNearBigger) {
            std::cout << " " << el;
        }
        std::cout << "\n";
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
    if (verbose) { utils::toc(t, "Graph::unmergeEdge finished"); }
}

void Graph::splitIntoConnectedComponentsOfWorkingNode(
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
            for (auto &twosidedEdge : initialNodes[activeNode]->twosidedEdges) {
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
    if (verbose) { utils::toc(t, "splitIntoConnectedComponentsOfWorkingNode finished"); }
}


Graph::SegmentIdType Graph::getLargestIdInSegmentVolume(Graph::SegmentsImageType::Pointer pSegment) {
    double tic = utils::tic();
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

    utils::toc(tic, "Graph::getLargestIdInSegmentVolume");
    return backgroundLabel;
}


void Graph::splitWorkingNodeIntoInitialNodes(int x, int y, int z) {
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
    double t = 0;
    if (verbose) {
        std::cout << "Graph::splitWorkingNodeIntoInitialNodes called: (" << workingNodeIdToSplit << ")\n";
        t = utils::tic();
    }
    if (!isIgnoredId(workingNodeIdToSplit)) {

        auto pToWorkingNode = workingNodes.at(workingNodeIdToSplit);
//        std::cout << "here" << "\n";

        if (verbose) { std::cout << "Splitting into: ";}
        std::vector<SegmentIdType> idsOfSubInitialNodes;
        for (auto &initialNode : pToWorkingNode->subInitialNodes) {
            idsOfSubInitialNodes.push_back(initialNode.first);
            if (verbose) {std::cout << initialNode.first << " ";}
        }
        if (verbose) {std::cout << "\n";}

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

            for (auto &voxelArray : newWorkingNode->getVoxelPointerArray()) {
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
            for (auto &edge : workingNodes.at(id)->twosidedEdges) {
                for (auto pInitialEdge : edge.second->subInitialEdges) {
                    graphBase->edgeStatus.at(pInitialEdge->numId) = defaultEdgeStatus;
                }
            }
        }

    }
    if (verbose) { utils::toc(t, "Graph::splitWorkingNodeIntoInitialNodes finished"); }

}


Graph::SegmentsImageType::RegionType Graph::getDilatedRegionFromRoi(Roi roi, SegmentsImageType::SizeType imageMax,
                                                                    int numberVxDilations) {
    double t = 0;
    if (verbose) {
        std::cout << "Graph::getDilatedRegionFromRoi called: \n";
        t = utils::tic();
    }
    // e.g.: (inclusive ranges!)
    // imageDimensions 100 200 300
    // roi: 0-19, 20-39, 30-49
    // numberVxDilations: 1


    // region
    // startIndex = 0, 19, 29
    // size = 21, 22, 22


    int startX, startY, startZ;
    startX = std::max<int>(0, roi.minX - numberVxDilations);
    startY = std::max<int>(0, roi.minY - numberVxDilations);
    startZ = std::max<int>(0, roi.minZ - numberVxDilations);

    int maxSizeX, maxSizeY, maxSizeZ;
    maxSizeX = imageMax[0] - startX;
    maxSizeY = imageMax[1] - startY;
    maxSizeZ = imageMax[2] - startZ;

    unsigned int sizeX, sizeY, sizeZ;
    sizeX = std::min<unsigned int>(maxSizeX, roi.maxX + numberVxDilations - startX + 1);
    sizeY = std::min<unsigned int>(maxSizeY, roi.maxY + numberVxDilations - startY + 1);
    sizeZ = std::min<unsigned int>(maxSizeZ, roi.maxZ + numberVxDilations - startZ + 1);

//    printf("roiMinX: %i roiMinY: %i roiMinZ: %i\n", roi.minX, roi.minY, roi.minZ);
//    printf("roiMaxX: %i roiMaxY: %i roiMaxZ: %i\n", roi.maxX, roi.maxY, roi.maxZ);
//    printf("imageSizeX: %lu imageSizeY: %lu imageSizeZ: %lu\n", imageMax[0], imageMax[1], imageMax[2]);
//    printf("startX: %i startY: %i startZ: %i\n", startX, startY, startZ);
//    printf("WidthX: %i WidthY: %i WidthZ: %i\n", sizeX, sizeY, sizFeZ);


    SegmentsImageType::IndexType startIndex = {startX, startY, startZ};
    SegmentsImageType::SizeType size = {sizeX, sizeY, sizeZ};
    SegmentsImageType::RegionType region = {startIndex, size};
    if (verbose) { utils::toc(t, "Graph::getDilatedRegionFromRoi finished"); }
    return region;
}


// highlevel workflow:
// calculate overlap of initial voxels with refinement watershed
// if edge connects two segments with the same overlaplabel, both greater than threshold, merge them
// if overlap higher than threshold, merge them

// future:
// use different labels for manually merged segments than automatically merged labels
// override automatic decision with manual decision, if available
void Graph::mergeSegmentsWithRefinementWatershed() {
    double t = 0, t1 = 0;
    if (verbose) {
        std::cout << "Graph::mergeSegmentsWithRefinementWatershed called: \n";
        t = utils::tic();
    }


    if (graphBase->pRefinementWatershed != nullptr) {

        std::map<dataType::SegmentIdType, LabelOverlap> overlapMap;
        double mergeThreshold = 0.75;
        for (auto &node : initialNodes) {
            overlapMap[node.first] = LabelOverlap();
            overlapMap[node.first].setPToOtherLabelImagePointer(graphBase->pRefinementWatershed);
            overlapMap[node.first].compute(node.second->voxels);
//            std::cout << "label: " << overlapMap[node.first].corrospondingLabelWithMostOverlap << " " <<
//                      overlapMap[node.first].overlapPercentage << "\n";
        }

        t1 = utils::tic();
//        get vec of edges to merge to then call mergeEdges on all of them
        std::set<EdgeNumIdType> edgesToMerge;
        for (auto &edge : initialTwoSidedEdges) {
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
        if (verbose) { utils::toc(t1, "Graph::mergeSegmentsWithRefinementWatershed Merging edges finished"); }


//    for (auto &feature : nodeFeatures) {
//        feature->compute(voxels);
//    }

    }
    if (verbose) { utils::toc(t, "Graph::mergeSegmentsWithRefinementWatershed finished"); }

}

void Graph::transferSegmentsWithRefinementOverlap() {
    double t = 0;
    if (verbose) {
        std::cout << "Graph::transferSegmentsWithRefinementOverlap called: \n";
        t = utils::tic();
    }

    if (graphBase->pSelectedSegmentation != nullptr) {
        if (graphBase->pRefinementWatershed != nullptr) {
            double overlapThreshold = 0.7;
            for (auto &node : workingNodes) {
                LabelOverlap overlapFeature = LabelOverlap();
                overlapFeature.setPToOtherLabelImagePointer(graphBase->pRefinementWatershed);
                overlapFeature.compute(node.second->getVoxelPointerArray());
                std::cout << overlapFeature.values[0] << "\n";
                if (overlapFeature.values[0] > overlapThreshold) {
                    if (overlapFeature.corrospondingLabelWithMostOverlap != backgroundId) {
                        transferWorkingNodeToSegmentation(node.first);
                    }
                }
            }
        }
    }
    if (verbose) { utils::toc(t, "Graph::transferSegmentsWithRefinementOverlap finished"); }
}

void Graph::transferSegmentsWithVolumeCriterion(double volumeThreshold) {
    double t = 0;
    if (verbose) {
        std::cout << "Graph::transferSegmentsWithVolumeCriterion called: \n";
        t = utils::tic();
    }
    if (graphBase->pSelectedSegmentation != nullptr) {
        NumberOfVoxels volumeFeature = NumberOfVoxels();
        for (auto &node : workingNodes) {
            volumeFeature.compute(node.second->getVoxelPointerArray());
            std::cout << volumeFeature.values[0] << "\n";
            if (volumeFeature.values[0] > volumeThreshold) {
                transferWorkingNodeToSegmentation(node.first);
            }
        }
    }
    if (verbose) { utils::toc(t, "Graph::transferSegmentsWithVolumeCriterion finished"); }
}

void Graph::receiveBackgroundIdStrategy(QString backgroundIdStrategyIn){
    std::cout << "Graph::receiveBackgroundIdStrategy started"  << std::endl;
    backgroundIdStrategy = backgroundIdStrategyIn.toStdString();
    if (verbose) {
        std::cout << "Graph::receiveBackgroundIdStrategy Changing background strategy to:" << backgroundIdStrategy << "\n";
    }
}

void Graph::exportDebugInformation(){
    std::cout << "Graph::exportDebugInformation started"  << std::endl;
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



// highlevel workflow:
// * get the background id of the refinement ws (highest label)<
// * check if the clicked label is not background
// * do a floodfill on the clicked pixel to get the refined segments voxels

void Graph::refineSegmentByPosition(int x, int y, int z) {
    double t = 0, t1 = 0, t2 = 0;
    if (verbose) { t = utils::tic("Graph::refineSegmentByPosition called"); }
    // use a reference to write shorter/better more readable code
    auto &pRefinementWatershed = graphBase->pRefinementWatershed;

    if (pRefinementWatershed != nullptr) {
        bool coordinates_in_ROI = true;
        if (graphBase->pRefinementWatershedSignal->ROI_set){

            if(x < graphBase->pRefinementWatershedSignal->ROI_fx){
                coordinates_in_ROI = false;
            }
            if(y < graphBase->pRefinementWatershedSignal->ROI_fy){
                coordinates_in_ROI = false;
            }
            if(z < graphBase->pRefinementWatershedSignal->ROI_fz){
                coordinates_in_ROI = false;
            }

            if(x > graphBase->pRefinementWatershedSignal->ROI_tx){
                coordinates_in_ROI = false;
            }
            if(y > graphBase->pRefinementWatershedSignal->ROI_ty){
                coordinates_in_ROI = false;
            }
            if(z > graphBase->pRefinementWatershedSignal->ROI_tz){
                coordinates_in_ROI = false;
            }
        }

        if(coordinates_in_ROI) {
            if (verbose) { t1 = utils::tic(); }
//        SegmentIdType backgroundLabelInRefinementWS = getLargestIdInSegmentVolume(pRefinementWatershed);

            SegmentIdType labelInRefinementWS = pRefinementWatershed->GetPixel({x, y, z});
            SegmentIdType labelToInsertTargetWS = nextFreeId;
            nextFreeId++;

            int msgBoxAnswer = QMessageBox::Yes;

            bool hardExtIfLabelInRefinementIs0 = true;
            if (labelInRefinementWS == 0) {
                if (hardExtIfLabelInRefinementIs0){
                    return;
                }
                QMessageBox msgBox;
                msgBox.setWindowTitle("Refinment");
                msgBox.setText(
                        "You're trying to get a segment that has the label of 0 (normally background label) in the watershed. Continue?");
                msgBox.setStandardButtons(QMessageBox::Yes);
                msgBox.addButton(QMessageBox::No);
                msgBox.setDefaultButton(QMessageBox::No);
                msgBoxAnswer = msgBox.exec();
            }

            if (msgBoxAnswer == QMessageBox::Yes) {
                // create new initial node by floodfilling refinement watershed
                std::cout << "Inserting Initial Node (id: " << labelToInsertTargetWS << ")\n";
                InitialNode *newInitialNode = new InitialNode(graphBase, pRefinementWatershed, labelToInsertTargetWS, x,
                                                              y, z);
                // reset the corrosponding segment pointer to the working segments image
                newInitialNode->setSegmentPointer(graphBase->pWorkingSegmentsImage);
                // insert node in initialNodes
                segmentManager.addInitialNode(newInitialNode);
                newInitialNode->roi.updateBoundingRoi(newInitialNode->voxels);


                auto sizeMax = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion().GetSize();
                SegmentsImageType::RegionType regionDilated = getDilatedRegionFromRoi(newInitialNode->roi, sizeMax, 2);

                if (graphBase->pWorkingSegmentsImage == nullptr) {
                    std::cout << "Graph::refineSegmentByPosition: Something went wrong -- pWorkingSegmentsImage is null!!\n";
                    return;
                }

                itk::ImageRegionConstIterator<SegmentsImageType> itAfter(graphBase->pWorkingSegmentsImage,
                                                                         regionDilated);

                // unmerge all segments inside the refined segment
                std::set<SegmentIdType> segmentsInsideRefinedRegion;
                SegmentIdType currentSegment;
                itAfter.GoToBegin();
                while (!itAfter.IsAtEnd()) {
                    currentSegment = itAfter.Get();
                    if (!isIgnoredId(currentSegment)) {
                        segmentsInsideRefinedRegion.insert(currentSegment);
                    }
                    ++itAfter;
                }

                // add also every working node that is connected to a working node where the new initial node gets placed
                for (auto &segment : segmentsInsideRefinedRegion) {
                    if (workingNodes.count(segment) == 0) {
                        std::cout << "CRASH! " << segment << " is not in workingNodes!!\n";
                    }
                    for (auto node : workingNodes[segment]->twosidedEdges) { // crash here!
                        segmentsInsideRefinedRegion.insert(node.first);
                    }
                }

                for (auto &segment : segmentsInsideRefinedRegion) {
                    splitWorkingNodeIntoInitialNodes(segment);
                }

                std::set<SegmentIdType> labelsInROIAfter;
                std::set<SegmentIdType> labelsInROIBefore;
                labelsInROIBefore.insert(newInitialNode->getLabel());

                itAfter.GoToBegin();
                while (!itAfter.IsAtEnd()) {
                    labelsInROIBefore.insert(itAfter.Get());
                    ++itAfter;
                }
                std::cout << "Labels in ROI before: ";
                for (auto &val : labelsInROIBefore) {
                    std::cout << val << " ";
                }
                std::cout << "\n";

                for (auto &voxel : newInitialNode->voxels) {
//                printf("INSERT: %d %d %d label: %d\n", voxel.x, voxel.y, voxel.z, labelToInsertTargetWS);
                    graphBase->pWorkingSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, labelToInsertTargetWS);
                }

                itAfter.GoToBegin();
                while (!itAfter.IsAtEnd()) {
                    labelsInROIAfter.insert(itAfter.Get());
                    ++itAfter;
                }

                std::cout << "Labels in ROI after: ";
                for (auto &val : labelsInROIAfter) {
                    std::cout << val << " ";
                }
                std::cout << "\n";


                // delete labels that are not present anymore due to the new segments
                std::set<GraphBase::SegmentsVoxelType> labelsInROIIntersection;
                std::set_difference(labelsInROIBefore.begin(), labelsInROIBefore.end(),
                                    labelsInROIAfter.begin(), labelsInROIAfter.end(),
                                    std::inserter(labelsInROIIntersection, labelsInROIIntersection.begin()));

                std::cout << "Deleting the following Nodes: ";
                for (auto &val : labelsInROIIntersection) {
                    std::cout << val << " ";
                }
                std::cout << "\n";

                // TODO: cant we make that difference/bg estimation etc all in one go?


                // double check if the labels in the intersection are really to be deleted
                // if segments are not connected, this is necessary
                itk::ImageRegionConstIterator<SegmentsImageType>
                        it(graphBase->pWorkingSegmentsImage,
                           graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
                auto copyLabelsInROIIntersection = labelsInROIIntersection; // have a copy of the id to not invalidate iterators while erasing keys
                for (auto &id : copyLabelsInROIIntersection) {
                    bool labelShouldBeDeleted = true;
                    it.GoToBegin();
                    while (!it.IsAtEnd()) {
                        if (it.Get() == id) {
                            labelShouldBeDeleted = false;
                            break;
                        }
                        ++it;
                    }
                    if (!labelShouldBeDeleted) {
                        std::cout << "Found Label (" << id << ") in remaining Lattice, not deleting node!" << std::endl;
                        labelsInROIIntersection.erase(id);
                    }
                }

                // remove labels that get replaced by the new segment
                // as we have unsplit the regions before, all labels are both initial nodes and working nodes!
                for (auto label : labelsInROIIntersection) {
                    segmentManager.removeWorkingNode(workingNodes[label].get());
                    segmentManager.removeInitialNode(label);
//                removeInitialNode(initialNodes[label].get());
//                removeWorkingNode(workingNodes[label].get());
                }
                if (verbose) { utils::toc(t1, "first part finished"); }
                if (verbose) { t2 = utils::tic(); }


                // calculate surfacevoxels and onesidededges for new node
                segmentManager.computeSurfaceAndOneSidedEdgesOnInitialNode(newInitialNode);
                if (verbose) { std::cout << "done computeSurfaceAndOneSidedEdgesOnInitialNode" << std::endl;}
                segmentManager.computeCorrospondingOneSidedInitialEdges(newInitialNode);

                if (verbose) { utils::toc(t2, "1 part finished"); }
//            printInitialNodesToFile("initialNodes.txt");

                // recalculate voxels for neighboring initial nodes
                std::vector<SegmentIdType> vecOfConnectedInitialNodeIds = utils::getKeyVecOfSharedPtrMap<SegmentIdType>(
                        newInitialNode->onesidedEdges);

                // take all the neighbors of the unsplitted node
                // attention: here workingnode == initialnode does not have to be true!
                // TODO: EdgeCase: InitialNode --> Workingnode edges on the border!
                //TODO: is there a edge case where actually one of the segments is not an initialNode but a workingnode?
                //TODO: Maybe enforce it/check it computationally
                segmentManager.recomputeVoxelListAndOneSidedEdgesIfShrinked(vecOfConnectedInitialNodeIds);

                if (verbose) { utils::toc(t2, "3 part finished"); }
                segmentManager.mergeNewOneSidedEdgesIntoTwosidedEdges();

                // generate new workingnode  and working edges based of new segment

                auto newWorkingNode = new WorkingNode(newInitialNode, labelToInsertTargetWS, initialNodes);
                segmentManager.addWorkingNode(newWorkingNode);
                segmentManager.recalculateEdgesOnWorkingNode(newWorkingNode);

                //TODO: check voxelwise if refinement split a initial node into two segments
                if (verbose) { utils::toc(t2, "second part finished"); }

            } else {
                std::cout << "Label to insert is background (highest label), refinement is not done. \n";
            }
        } else {
            std::cout << "Clicked point outside of refinement ROI.\n";
        }

    } else {
        std::cout << "Watershed Pointer was never initialized, add refinement watershed before refining! \n";
    }

    if (verbose) { utils::toc(t, "Graph::refineSegmentByPosition finished"); }

}


void Graph::transferWorkingNodeToSegmentation(int x, int y, int z) {
    double t = 0;
    if (verbose) {
        std::cout << "Graph::transferWorkingNodeToSegmentation called\n";
        t = utils::tic();
    }
    if (graphBase->pSelectedSegmentation != nullptr) {
        SegmentIdType labelOfTargetedWorkingNode = graphBase->pWorkingSegmentsImage->GetPixel({x, y, z});
        transferWorkingNodeToSegmentation(labelOfTargetedWorkingNode);
    }
    if (verbose) { utils::toc(t, "Graph::transferWorkingNodeToSegmentation finished"); }

}

void Graph::transferWorkingNodeToSegmentation(SegmentIdType labelOfNodeToTransfer) {

    SegmentIdType selectedSegmentationMaxSegmentId =  graphBase->selectedSegmentationMaxSegmentId;
    SegmentIdType newSegmentId = selectedSegmentationMaxSegmentId + 1;
    graphBase->selectedSegmentationMaxSegmentId = newSegmentId;

    if (graphBase->pSelectedSegmentation != nullptr) {
        if (!isIgnoredId(labelOfNodeToTransfer)) {
            for (auto voxelList : workingNodes[labelOfNodeToTransfer]->getVoxelPointerArray()) {
                for (auto voxel : *voxelList) {
                    graphBase->pSelectedSegmentation->SetPixel({voxel.x, voxel.y, voxel.z}, newSegmentId);
                }
            }
        }
    }
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
    for (auto &edge : initialOneSidedEdges) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        edge.second->print(nodeIndentationLevel, outStream);
    }
}

void Graph::printInitialTwoSidedEdges(std::ostream &outStream) {
    outStream << "=== initialTwoSidedEdges ===\n";
    int nodeIndentationLevel = 1;
    for (auto &edge : initialTwoSidedEdges) {
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
    for (auto &edge : workingEdges) {
        outStream << "key: " << edge.first.first << "," << edge.first.second << "\n";
        edge.second->print(nodeIndentationLevel, outStream);
    }
}

// == wrappers for writing to file ==


void Graph::printInitialNodesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing initialNodes to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printInitialNodes(outFile);
    outFile.close();
}

void Graph::printInitialTwoSidedEdgesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing initialTwoSidedEdges to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printInitialTwoSidedEdges(outFile);
    outFile.close();
}

void Graph::printInitialOneSidedEdgesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing initialTwoSidedEdges to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printInitialOneSidedEdges(outFile);
    outFile.close();
}

void Graph::printWorkingNodesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing workingNodes to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printWorkingNodes(outFile);
    outFile.close();
}

void Graph::printEdgeIdLookUpToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing edgeIdLookup to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printEdgeIdLookUp(outFile);
    outFile.close();
}

void Graph::printWorkingEdgesToFile(const std::string &pathToOutputfile) {
    std::cout << "Printing workingEdges to File: " << pathToOutputfile << "\n";
    std::ofstream outFile(pathToOutputfile);
    printWorkingEdges(outFile);
    outFile.close();
}

