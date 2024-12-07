
#include "node.h"
#include "graph.h"
#include "itkShapedNeighborhoodIterator.h"

Node::Node() {
    merged = false;
    for (auto &feature : nodeFeaturesList) {
        addFeature(feature);
    }
}

Node::Node(Voxel voxel) {
    merged = false;
//    voxels.reserve(1000);
    voxels.push_back(voxel);
    GraphBase::SegmentsImageType::IndexType index = {voxel.x, voxel.y, voxel.z};
    label = GraphBase::pSegments->GetPixel(index);
    roi = Roi(voxel);
    for (auto &feature : nodeFeaturesList) {
        addFeature(feature);
    }
};

bool Node::isAIgnoredSegmentLabel(GraphBase::SegmentsVoxelType label) {
    return (std::find(ignoredSegmentLabels.begin(), ignoredSegmentLabels.end(), label) != ignoredSegmentLabels.end());
}

void Node::addFeature(std::unique_ptr<Feature> &feature) {
    nodeFeatures.push_back(feature->createNew());
}

void Node::addEdge(std::shared_ptr<Edge> edge) {
    edges[edge->labelSmaller] = edge;
};

void Node::calculateNodeFeatures() {
    for (auto &feature : nodeFeatures) {
        feature->compute(voxels);
    }
}

void Node::calculateEdgeFeatures() {
//    if (edges.empty()){
//        std::cout << "Warning: There are no edges at that node! (id: " << this->label << " )" << std::endl;
//    }
    for (auto &edge : edges) {
        edge.second->calculateEdgeFeatures();
    }
}

void Node::calculateUnionFeatures() {
//  if (edges.empty()){
//    std::cout << "Warning: There are no edges at that node! (id: " << this->label << " )" << std::endl;
//  }
    for (auto &edge : edges) {
        edge.second->calculateUnionFeatures();
    }
}

void Node::removeConnectionToId(GraphBase::SegmentsVoxelType idToErase, bool verbose) {
    if (verbose) { std::cout << "erased: " << label << "->" << idToErase << std::endl; }
    edges.erase(idToErase);
}

void Node::addVoxel(Voxel &voxel) {
    voxels.push_back(voxel);
    roi.updateBoundingRoi(voxel);
}

bool Node::isMergedFrom(GraphBase::SegmentsVoxelType nodeIdA) {
    return (initialSegments.count(nodeIdA) > 0);
}

bool Node::isMergedFrom(GraphBase::SegmentsVoxelType nodeIdA, GraphBase::SegmentsVoxelType nodeIdB) {
    return (isMergedFrom(nodeIdA) && isMergedFrom(nodeIdB));
}

std::vector<GraphBase::SegmentsVoxelType> Node::getVectorOfConnectedNodeIds() {
    std::vector<GraphBase::SegmentsVoxelType> connectedNodeId;
//    edgeId.reserve(edges.size());
    if (!edges.empty()) {
        for (auto &edge: edges) {
            connectedNodeId.push_back(edge.first);
        }
        return connectedNodeId;
    } else {
        return connectedNodeId = {}; // return empty list
    }

}


void Node::computeSurfaceVoxels() {
    // clear old surface list
    bool recalculateSurfaceVoxels = surfaceVoxels.empty();
    edges.clear();

    std::set<GraphBase::SegmentsVoxelType> nodeIdsOfEdgesToCompute;
    if (recalculateSurfaceVoxels)
        for (auto listEntry : getVoxelList()) { // iterate over all voxels (also not-surface ones)
            for (const Voxel &voxel : *listEntry) {
                subCalculateConnectivity(recalculateSurfaceVoxels, nodeIdsOfEdgesToCompute, voxel);
            }
        }
    else { // only iterate over surface voxels
        for (const Voxel &voxel : surfaceVoxels) {
            subCalculateConnectivity(recalculateSurfaceVoxels, nodeIdsOfEdgesToCompute, voxel);
        }
    }

    for (auto &id : nodeIdsOfEdgesToCompute) {
        GraphBase::SegmentsVoxelType labelSmaller, labelBigger;
        labelSmaller = (label < id) ? label : id;
        labelBigger = (label > id) ? label : id;
        std::pair<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType> edgeIdentifier = {labelSmaller,
                                                                                                labelBigger};
        pGraph->edges[edgeIdentifier]->edgeComputed = true;
    }
}

void Node::computeSurfaceVoxelsParallel(std::map<GraphBase::SegmentsVoxelType, std::shared_ptr<Edge>> &tmpEdges,
                                        std::vector<Voxel> &tmpSurfaceVoxels) {

    using NeighborhoodIteratorType = itk::ConstShapedNeighborhoodIterator<GraphBase::SegmentsImageType>;

    GraphBase::SegmentsImageType::IndexType startIndex = {roi.minX, roi.minY, roi.minZ};
    GraphBase::SegmentsImageType::SizeType size = {roi.maxX - roi.minX + 1, roi.maxY - roi.minY + 1,
                                                   roi.maxZ - roi.minZ + 1};
    GraphBase::SegmentsImageType::RegionType region = {startIndex, size};

    NeighborhoodIteratorType::RadiusType radius;
    radius.Fill(1);

    NeighborhoodIteratorType neighborhoodIt(radius, GraphBase::pSegments, region);

    NeighborhoodIteratorType::OffsetType off = {{0, 0, 0}};
    NeighborhoodIteratorType::OffsetType off1 = {{1, 0, 0}};
    NeighborhoodIteratorType::OffsetType off2 = {{-1, 0, 0}};
    NeighborhoodIteratorType::OffsetType off3 = {{0, 1, 0}};
    NeighborhoodIteratorType::OffsetType off4 = {{0, -1, 0}};
    NeighborhoodIteratorType::OffsetType off5 = {{0, 0, 1}};
    NeighborhoodIteratorType::OffsetType off6 = {{0, 0, -1}};

    neighborhoodIt.ActivateOffset(off);
    neighborhoodIt.ActivateOffset(off1);
    neighborhoodIt.ActivateOffset(off2);
    neighborhoodIt.ActivateOffset(off3);
    neighborhoodIt.ActivateOffset(off4);
    neighborhoodIt.ActivateOffset(off5);
    neighborhoodIt.ActivateOffset(off6);
    //4 10 12 14 16 22
    //4 10 12 13 14 16 22

    std::vector<unsigned int> offSetIndices = {4, 10, 12, 14, 16, 22};
    unsigned int centerOffset = 13;

    neighborhoodIt.GoToBegin();
    std::vector<GraphBase::SegmentsVoxelType> addedToLabelAlready;
    GraphBase::SegmentsVoxelType itLabel;
    GraphBase::SegmentsVoxelType newLabel;
    size_t x, y, z;
    NeighborhoodIteratorType::IndexType index;
    NeighborhoodIteratorType::ConstIterator innerIterator;
    bool IsInBounds;
    bool isSurface;

    while (!neighborhoodIt.IsAtEnd()) {
        itLabel = neighborhoodIt.GetPixel(centerOffset);
        if (itLabel == label) {
            addedToLabelAlready.clear();
            isSurface = false;
            index = neighborhoodIt.GetIndex();
            x = index[0];
            y = index[1];
            z = index[2];
            innerIterator = neighborhoodIt.Begin();
            for (unsigned int i = 0; i < 6; ++i) {
                newLabel = neighborhoodIt.GetPixel(offSetIndices[i], IsInBounds);
                if (IsInBounds) {
                    if ((newLabel != label) && !isAIgnoredSegmentLabel(newLabel)) {
                        isSurface = true;
                        if (!tmpEdges.count(newLabel)) {
                            tmpEdges[newLabel] = std::make_shared<Edge>(Voxel(x, y, z), newLabel, label, false);
                            addedToLabelAlready.push_back(newLabel);
                        } else {
                            // if this exact voxel was not added to the same edge before
                            if (find(addedToLabelAlready.begin(), addedToLabelAlready.end(), newLabel) ==
                                addedToLabelAlready.end()) {
                                // add it
                                tmpEdges[newLabel]->addVoxel(Voxel(x, y, z));
                                addedToLabelAlready.push_back(newLabel);
                            }
                        }
                    }
                }
            }
            if (isSurface) {
                tmpSurfaceVoxels.emplace_back(x, y, z);
            }
        }
        ++neighborhoodIt;
    }
}


void Node::subCalculateConnectivity(bool recalculateSurfaceVoxels,
                                    std::set<GraphBase::SegmentsVoxelType> &nodeIdsOfEdgesToCompute,
                                    const Voxel &voxel) {
    // Note: if you do not use 1-sided differences, you risk - in a naive implementation - that you add voxels twice
    // 1 1 1 1 1
    // 1 1 2 1 1
    // 2 2 2 2 2

    std::vector<GraphBase::SegmentsVoxelType> addedToLabelAlready;
    size_t x, y, z, newX, newY, newZ;
    std::vector<int> dirX{-1, 1, 0, 0, 0, 0};
    std::vector<int> dirY{0, 0, 1, -1, 0, 0};
    std::vector<int> dirZ{0, 0, 0, 0, 1, -1};
    //TODO: Make with itk neighborhooditerator, disable bound checking
    auto shape = GraphBase::pSegments->GetLargestPossibleRegion().GetSize();
    // min/max of the volume
    size_t vMaxX = shape[0], vMaxY = shape[1], vMaxZ = shape[2], vMinX = 0, vMinY = 0, vMinZ = 0;
    GraphBase::SegmentsVoxelType newLabel;
    bool isSurface = false;
    x = voxel.x;
    y = voxel.y;
    z = voxel.z;
    addedToLabelAlready.clear();
    for (size_t i = 0; i < dirX.size(); ++i) { // check their neighborhood
        newX = x + dirX[i];
        newY = y + dirY[i];
        newZ = z + dirZ[i];
        if (newX >= vMinX && newY >= vMinY && newZ >= vMinZ && newX < vMaxX && newY < vMaxY && newZ < vMaxZ) {
            // if the neighborhood is in bounds
            GraphBase::SegmentsImageType::IndexType index{0, 0, 0};
            index[0] = newX;
            index[1] = newY;
            index[2] = newZ;
            newLabel = GraphBase::pSegments->GetPixel(index);
            if ((newLabel != label) && !isAIgnoredSegmentLabel(newLabel)) {
                // if it is not itself or background
                isSurface = true;

                if (!edges.count(newLabel)) { // if there is no edge-container in this node for the label of the voxels
                    GraphBase::SegmentsVoxelType labelSmaller, labelBigger;
                    labelSmaller = (label < newLabel) ? label : newLabel;
                    labelBigger = (label > newLabel) ? label : newLabel;
                    std::pair<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType> edgeIdentifier =
                            {labelSmaller, labelBigger};

                    if (pGraph->edges.count(edgeIdentifier) ==
                        0) { // if there is also no fitting edge container in the graph//other nodes
                        // set edge in this node
                        edges[newLabel] = std::make_shared<Edge>(voxel, newLabel, label);
                        // edge to this node from connected node
                        pGraph->nodes[newLabel]->edges[label] = edges[newLabel];
                        // set edge id in edge lookup
                        pGraph->edges[edgeIdentifier] = edges[newLabel];
                        // in nodeIdsOfEdgesToCompute are the labels where edge voxels should be inserted
                        nodeIdsOfEdgesToCompute.insert(newLabel);
                    } else {
                        // use the edge that is already there (note: this assumes only a 1vx edge)
                        edges[newLabel] = pGraph->edges[edgeIdentifier];
                    }
                    addedToLabelAlready.push_back(newLabel);
                } else {
                    if (nodeIdsOfEdgesToCompute.count(newLabel) > 0) {
                        if (find(addedToLabelAlready.begin(), addedToLabelAlready.end(), newLabel) ==
                            addedToLabelAlready.end()) {
                            edges[newLabel]->addVoxel(voxel);
                            addedToLabelAlready.push_back(newLabel);
                        }
                    }
                }
            }
        }
    }
    if (isSurface && recalculateSurfaceVoxels) {
        surfaceVoxels.push_back(voxel);
    }
}


void Node::recomputEdge(std::vector<Voxel> &voxelList) {
    std::cout << "start computing edge values ...";
    std::vector<int> dirX{-1, 1, 0, 0, 0, 0};
    std::vector<int> dirY{0, 0, 1, -1, 0, 0};
    std::vector<int> dirZ{0, 0, 0, 0, 1, -1};
    auto shape = GraphBase::pSegments->GetLargestPossibleRegion().GetSize();
    size_t vMaxX = shape[0], vMaxY = shape[1], vMaxZ = shape[2], vMinX = 0, vMinY = 0, vMinZ = 0;
    GraphBase::SegmentsVoxelType newLabel;
    size_t x, y, z, newX, newY, newZ;
    std::vector<GraphBase::SegmentsVoxelType> addedToLabelAlready;
    for (auto &voxel : voxelList) {
        x = voxel.x;
        y = voxel.y;
        z = voxel.z;
        for (size_t i = 0; i < dirX.size(); ++i) {
            newX = x + dirX[i];
            newY = y + dirY[i];
            newZ = z + dirZ[i];
            addedToLabelAlready.clear();
            if (newX >= vMinX && newY >= vMinY && newZ >= vMinZ && newX < vMaxX && newY < vMaxY && newZ < vMaxZ) {
                GraphBase::SegmentsImageType::IndexType index{0, 0, 0};
                index[0] = newX;
                index[1] = newY;
                index[2] = newZ;
                newLabel = GraphBase::pSegments->GetPixel(index);
                if ((newLabel != label) && !isAIgnoredSegmentLabel(newLabel)) {
                    if (!edges.count(newLabel)) { // if its not in the edge map already add it
                        edges[newLabel] = std::make_shared<Edge>(voxel, newLabel, label);
                        std::pair<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType> edgeIdentifier =
                                {edges[newLabel]->labelSmaller, edges[newLabel]->labelBigger};
                        pGraph->edges[edgeIdentifier] = edges[newLabel];

                        addedToLabelAlready.push_back(newLabel);
                    } else { // if its already there, add to the edge
                        if (std::find(addedToLabelAlready.begin(), addedToLabelAlready.end(), newLabel) ==
                            addedToLabelAlready.end()) {
                            edges[newLabel]->addVoxel(voxel);
                            addedToLabelAlready.push_back(newLabel);
                        }
                    }
                }
            }
        }
    }
    std::cout << "... finished!" << std::endl;
}

void Node::recomputEdge(GraphBase::SegmentsVoxelType connectedNodeId) {
    std::cout << "start computing edge: " << label << " -> " << connectedNodeId << std::endl;
    std::vector<int> dirX{-1, 1, 0, 0, 0, 0};
    std::vector<int> dirY{0, 0, 1, -1, 0, 0};
    std::vector<int> dirZ{0, 0, 0, 0, 1, -1};
    auto shape = GraphBase::pSegments->GetLargestPossibleRegion().GetSize();
    size_t vMaxX = shape[0], vMaxY = shape[1], vMaxZ = shape[2], vMinX = 0, vMinY = 0, vMinZ = 0;
    GraphBase::SegmentsVoxelType newLabel;
    size_t x, y, z, newX, newY, newZ;
    std::vector<GraphBase::SegmentsVoxelType> addedToLabelAlready;
    for (auto &voxel : surfaceVoxels) {
        x = voxel.x;
        y = voxel.y;
        z = voxel.z;
        for (size_t i = 0; i < dirX.size(); ++i) {
            newX = x + dirX[i];
            newY = y + dirY[i];
            newZ = z + dirZ[i];
            addedToLabelAlready.clear();
            if (newX >= vMinX && newY >= vMinY && newZ >= vMinZ && newX < vMaxX && newY < vMaxY && newZ < vMaxZ) {
                GraphBase::SegmentsImageType::IndexType index{0, 0, 0};
                index[0] = newX;
                index[1] = newY;
                index[2] = newZ;
                newLabel = GraphBase::pSegments->GetPixel(index);
                if (newLabel == connectedNodeId) {
                    if (!edges.count(newLabel)) { // if its not in the edge map already add it
                        edges[newLabel] = std::make_shared<Edge>(voxel, newLabel, label);
                        std::pair<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType> edgeIdentifier =
                                {edges[newLabel]->labelSmaller, edges[newLabel]->labelBigger};
                        pGraph->edges[edgeIdentifier] = edges[newLabel];
                        addedToLabelAlready.push_back(newLabel);
                    } else { // if its already there, add to the edge
                        if (std::find(addedToLabelAlready.begin(), addedToLabelAlready.end(), newLabel) ==
                            addedToLabelAlready.end()) {
                            edges[newLabel]->addVoxel(voxel);
                            addedToLabelAlready.push_back(newLabel);
                        }
                    }
                }
            }
        }
    }
    std::cout << "... finished!" << std::endl;
}

void Node::recomputAllEdges() {
    edges.clear();
    std::vector<int> dirX{-1, 1, 0, 0, 0, 0};
    std::vector<int> dirY{0, 0, 1, -1, 0, 0};
    std::vector<int> dirZ{0, 0, 0, 0, 1, -1};
    auto shape = GraphBase::pSegments->GetLargestPossibleRegion().GetSize();
    size_t vMaxX = shape[0], vMaxY = shape[1], vMaxZ = shape[2], vMinX = 0, vMinY = 0, vMinZ = 0;
    GraphBase::SegmentsVoxelType newLabel;
    size_t x, y, z, newX, newY, newZ;
    std::vector<GraphBase::SegmentsVoxelType> addedToLabelAlready;
    for (auto &voxel : surfaceVoxels) {
        x = voxel.x;
        y = voxel.y;
        z = voxel.z;
        for (size_t i = 0; i < dirX.size(); ++i) {
            newX = x + dirX[i];
            newY = y + dirY[i];
            newZ = z + dirZ[i];
            addedToLabelAlready.clear();
            if (newX >= vMinX && newY >= vMinY && newZ >= vMinZ && newX < vMaxX && newY < vMaxY && newZ < vMaxZ) {
                GraphBase::SegmentsImageType::IndexType index{0, 0, 0};
                index[0] = newX;
                index[1] = newY;
                index[2] = newZ;
                newLabel = GraphBase::pSegments->GetPixel(index);
                if ((newLabel != label) && !isAIgnoredSegmentLabel(newLabel)) {
                    if (!edges.count(newLabel)) { // if its not in the edge map already add it
                        edges[newLabel] = std::make_shared<Edge>(voxel, newLabel, label);
                        std::pair<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType> edgeIdentifier =
                                {edges[newLabel]->labelSmaller, edges[newLabel]->labelBigger};
                        pGraph->edges[edgeIdentifier] = edges[newLabel];
                        addedToLabelAlready.push_back(newLabel);
                    } else { // if its already there, add to the edge
                        if (std::find(addedToLabelAlready.begin(), addedToLabelAlready.end(), newLabel) ==
                            addedToLabelAlready.end()) {
                            edges[newLabel]->addVoxel(voxel);
                            addedToLabelAlready.push_back(newLabel);
                        }
                    }
                }
            }
        }
    }
}

void Node::print(int indentationLevel = 0) {
    std::string indentationString = "";
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    std::cout << indentationString << "label: " << label << std::endl;
    std::cout << indentationString << "number of voxels: " << voxels.size() << std::endl;
    std::cout << indentationString << "number of surface voxels: " << surfaceVoxels.size() << std::endl;
    std::cout << indentationString << "number of edges: " << edges.size() << std::endl;
    std::cout << indentationString << "fx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << std::endl;
    std::cout << indentationString << "tx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << std::endl;

    std::cout << indentationString << "initial segments:";
    for (auto &segment : initialSegments) {
        std::cout << " " << segment;
    }
    std::cout << std::endl;
    std::cout << indentationString << "origin segments: " << std::get<0>(originSegments)
              << " " << std::get<1>(originSegments) << std::endl;
    std::cout << indentationString << "merged: " << merged << std::endl;
    std::cout << indentationString << "merged into: " << mergedIntoId << std::endl;

    for (auto &feature : nodeFeatures) {
        std::cout << "" << indentationString << feature->filterName << " " << feature->signalName << " ";
        for (auto &value : feature->values) {
            std::cout << value << " ";
        }
        std::cout << std::endl;
    }

    for (auto &edge : edges) {
        std::cout << indentationString << "edgeKey: " << edge.first << std::endl;
        edge.second->print(indentationLevel + 1);
    }
}

void Node::printToFile(int indentationLevel, std::ostream &outFile) {
    std::string indentationString = "";
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outFile << indentationString << "label: " << label << std::endl;
    outFile << indentationString << "number of voxels: " << voxels.size() << std::endl;
    outFile << indentationString << "number of surface voxels: " << surfaceVoxels.size() << std::endl;
    outFile << indentationString << "number of edges: " << edges.size() << std::endl;
    outFile << indentationString << "fx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << std::endl;
    outFile << indentationString << "tx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << std::endl;
    outFile << indentationString << "initial segments:";
    for (auto &segment : initialSegments) {
        outFile << " " << segment;
    }
    outFile << std::endl;
    outFile << indentationString << "origin segments: " << std::get<0>(originSegments)
            << " " << std::get<1>(originSegments) << std::endl;
    outFile << indentationString << "merged: " << merged << std::endl;
    outFile << indentationString << "merged into: " << mergedIntoId << std::endl;

    for (auto &feature : nodeFeatures) {
        outFile << "" << indentationString << feature->filterName << " " << feature->signalName << " ";
        for (auto &value : feature->values) {
            outFile << value << " ";
        }
        outFile << std::endl;
    }

    for (auto &edge : edges) {
        outFile << indentationString << "edgeKey: " << edge.first << std::endl;
        edge.second->printToFile(indentationLevel + 1, outFile);
    }
}

GraphBase::SegmentsVoxelType Node::getGroundTruthLabel() {
    if (!FeatureList::GroundTruthLabelComputed) {
    }
    bool gtLabelFound = false;
    GraphBase::SegmentsVoxelType gtLabel;
    for (std::unique_ptr<Feature> &feature : nodeFeatures) {
        if (feature->filterName == "groundTruthLabel") {
            gtLabelFound = true;
            gtLabel = static_cast<GraphBase::SegmentsVoxelType>(feature->values[0]);
        }
    }
    if (!gtLabelFound) { throw std::logic_error("cant find ground truth label, is it added to node features?"); }
    return gtLabel;
}

std::vector<float> Node::getNodeFeatureByName(std::string filterName, std::string signalName) {
    bool featureFound = false;
    std::vector<float> featureValues;
    for (auto &feature : nodeFeatures) {
        if (feature->filterName == filterName) {
            if (feature->signalName == signalName) {
                featureFound = true;
                featureValues = feature->values;
                break;
            }
        }
    }
    if (!featureFound) {
        std::string errorString = "The requested nodefilter '" + filterName + "' for the signal '" + signalName +
                                  "' was not found. Was it added?";
        throw std::logic_error(errorString);
    }
    return featureValues;
}

void Node::setShouldMergeAttributeOnAllEdges() {
    for (auto &edge : edges) {
        GraphBase::SegmentsVoxelType gtLabel1, gtLabel2;
        gtLabel1 = pGraph->nodes[edge.second->labelBigger]->getGroundTruthLabel();
        gtLabel2 = pGraph->nodes[edge.second->labelSmaller]->getGroundTruthLabel();
        edge.second->shouldMerge = (gtLabel1 == gtLabel2) ? 1 : -1;
        if (gtLabel1 == 0 || gtLabel2 == 0) { edge.second->shouldMerge = -1; }
    }
}

void Node::mergeNodeFeatures(std::vector<Voxel> &allVoxel, Node &A, Node &B) {
    for (auto &feature : nodeFeatures) {
        std::vector<float> featureValA = A.getNodeFeatureByName(feature->filterName, feature->signalName);
        size_t numberElementsA = A.voxels.size();
        std::vector<float> featureValB = B.getNodeFeatureByName(feature->filterName, feature->signalName);
        size_t numberElementsB = B.voxels.size();
        feature->merge(allVoxel, featureValA, numberElementsA, featureValB, numberElementsB);
    }
}

std::vector<float> Node::getAllNodeFeaturesAsVector() {
    std::vector<float> allFeatures; // this is the minimal size assuming 1 value/feat,
    // however std::vector is dynamic
//    allFeatures.push_back(this->label);
    for (auto &nodeFeature : nodeFeatures) {
        if (nodeFeature->signalName != "groundTruth") {
            allFeatures.insert(allFeatures.end(), nodeFeature->values.begin(), nodeFeature->values.end());
        }
    }
    return allFeatures;
}

std::vector<std::vector<Voxel> *> Node::getVoxelList() {
    std::vector<std::vector<Voxel> *> voxelList;

    std::cout << "Initial Segments:\n";
    for (auto &segment : initialSegments) {
        std::cout << segment << " " << "size: " << pGraph->mergeTree[segment]->voxels.size() << "\n";
        voxelList.push_back(&pGraph->mergeTree[segment]->voxels);
    }
    return voxelList;
}