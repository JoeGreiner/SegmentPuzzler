#include "initialNode.h"
#include "workingNode.h"
#include "src/utils/utils.h"
#include <itkConstShapedNeighborhoodIterator.h>
#include <itkBinaryThresholdImageFunction.h>
#include <itkFloodFilledImageFunctionConditionalIterator.h>
#include <unordered_set>


InitialNode::InitialNode(std::shared_ptr<GraphBase> graphBaseIn, SegmentIdImageType::Pointer pSegmentsIn, SegmentIdType labelIn) {
    graphBase = graphBaseIn;
    pSegments = pSegmentsIn;
    label = labelIn;
    roi = Roi();

    for (auto &feature : FeatureList::nodeFeaturesList) {
        addFeature(feature);
    }
}

// creates initial node from flood filling into a label at a coordinate x,y,z
InitialNode::InitialNode(std::shared_ptr<GraphBase> graphBaseIn, SegmentIdImageType::Pointer pSegmentsIn, SegmentIdType labelIn,
                         int x, int y, int z) {
    graphBase = graphBaseIn;
    pSegments = pSegmentsIn;
    label = labelIn;
    roi = Roi();

    for (auto &feature : FeatureList::nodeFeaturesList) {
        addFeature(feature);
    }

    SegmentIdType labelInSegmentsImage = pSegmentsIn->GetPixel({x, y, z});

    // create new Node;
    // Do background check while adding the watershed!
    using FunctionType = itk::BinaryThresholdImageFunction<dataType::SegmentsImageType>;
    using IteratorType = itk::FloodFilledImageFunctionConditionalIterator<dataType::SegmentsImageType, FunctionType>;
    FunctionType::Pointer function = FunctionType::New();
    function->SetInputImage(pSegmentsIn);
    function->ThresholdBetween(labelInSegmentsImage, labelInSegmentsImage);
    IteratorType itFlood = IteratorType(graphBase->pWorkingSegmentsImage, function, {x, y, z});

    // add voxel via flood filling
    while (!itFlood.IsAtEnd()) {
        const auto &index = itFlood.GetIndex();
        Voxel tmpVoxel = Voxel(index[0], index[1], index[2]);
        addVoxel(tmpVoxel);
        ++itFlood;
    }
}


void InitialNode::addFeature(std::unique_ptr<Feature> &feature) {
    nodeFeatures.emplace_back(feature->createNew());
}

void InitialNode::calculateNodeFeatures() {
    for (auto &feature : nodeFeatures) {
        feature->compute(voxels);
    }
}

void InitialNode::addVoxel(Voxel voxelToAdd) {
//    roi.updateBoundingRoi(voxelToAdd);
    voxels.push_back(voxelToAdd);
}

void InitialNode::addVoxel(int x, int y, int z) {
    voxels.emplace_back(x, y, z);
}

void InitialNode::addVoxel(itk::Index<3> const &index) {
//    roi.updateBoundingRoi(index);
    voxels.emplace_back(index[0], index[1], index[2]);
//    voxels.push_back({static_cast<int>(index[0]), static_cast<int>(index[1]), static_cast<int>(index[2])});
}


std::vector<std::vector<Voxel> *> InitialNode::getVoxelPointerArray() {
    std::vector<std::vector<Voxel> *> voxelList;
    voxelList.push_back(&voxels);
    return voxelList;
}


bool InitialNode::isIgnoredId(SegmentIdType idToCheck,
                              std::vector<SegmentIdType> *ignoredSegmentIds) {
    // check if idToCFheck is in pIgnoredSegmentsLabel
    return (std::find(ignoredSegmentIds->begin(), ignoredSegmentIds->end(), idToCheck) != ignoredSegmentIds->end());
}

inline bool isValidIndex(size_t index, size_t dimX, size_t dimY, size_t dimZ) {
    return index >= 0 && index < (dimX * dimY * dimZ);
}

void InitialNode::parallelComputeOnesidedSurfaceAndEdges(std::vector<SegmentIdType> *ignoredSegmentIds) {
    onesidedEdges.clear();
    onesidedSurfaceVoxels.clear();
    unsigned int estimateNumberEdges = 30;
    unsigned int estimateNumberSurfaceVoxels = 9.0 * pow(voxels.size(), 2.0/3.0);
//    double estimatedSurfaceVoxels = k * pow(V, 2.0 / 3.0);
    onesidedEdges.reserve(estimateNumberEdges);
    onesidedSurfaceVoxels.reserve(estimateNumberSurfaceVoxels);

    // Dimensions and strides
    const auto dimX = pSegments->GetLargestPossibleRegion().GetSize()[0];
    const auto dimY = pSegments->GetLargestPossibleRegion().GetSize()[1];
    const auto dimZ = pSegments->GetLargestPossibleRegion().GetSize()[2];
    const auto sliceStride = dimX * dimY;
    const auto rowStride = dimX;

    // Image buffer
    const SegmentIdType* buffer = pSegments->GetBufferPointer();

    // Neighbor offsets
    const int offsets[6] = {
            1, -1, static_cast<int>(dimX), static_cast<int>(-dimX), static_cast<int>(sliceStride), static_cast<int>(-sliceStride)
    };

    Voxel voxelToAdd;
    std::unordered_set<SegmentIdType> addedToLabelAlready;
    addedToLabelAlready.reserve(6); // 6 neighbors

    for (size_t z = roi.minZ; z <= roi.maxZ; ++z) {
        for (size_t y = roi.minY; y <= roi.maxY; ++y) {
            for (size_t x = roi.minX; x <= roi.maxX; ++x) {
                size_t centerIndex = x + y * rowStride + z * sliceStride;
                SegmentIdType itLabel = buffer[centerIndex];
                if (itLabel == label) {
                    addedToLabelAlready.clear();
                    bool isSurface = false;
                    for (unsigned int i = 0; i < 6; ++i) {
                        size_t neighborIndex = centerIndex + offsets[i];
                        if (isValidIndex(neighborIndex, dimX, dimY, dimZ)) {
                            SegmentIdType newLabel = buffer[neighborIndex];
                            if (newLabel != label && !isIgnoredId(newLabel, ignoredSegmentIds)) {
                                isSurface = true;
                                if (addedToLabelAlready.find(newLabel) == addedToLabelAlready.end()) {
                                    {
                                        if (!onesidedEdges.count(newLabel)) {
                                            onesidedEdges[newLabel] = std::make_shared<InitialEdge>(newLabel, label);
                                        }
                                        voxelToAdd = Voxel(x, y, z);
                                        onesidedEdges[newLabel]->addVoxel(voxelToAdd);
                                    }
                                    addedToLabelAlready.insert(newLabel);
                                }
                            }
                        }
                    }
                    if (isSurface) {
                        onesidedSurfaceVoxels.emplace_back(x, y, z);
                    }
                }
            }
        }
    }
//     cast to double division
//    double factor_edges = static_cast<double>(onesidedEdges.size()) / estimateNumberEdges;
//    double factor_surface = static_cast<double>(onesidedSurfaceVoxels.size()) / estimateNumberSurfaceVoxels;
//    std::cout << "Estimated number of edges: " << estimateNumberEdges << " Real number of edges: " << onesidedEdges.size() << " factor: " << factor_edges << std::endl;
//    std::cout << "Estimated number of surface voxels: " << estimateNumberSurfaceVoxels << " Real number of surface voxels: " << onesidedSurfaceVoxels.size() << " factor: " << factor_surface << std::endl;
}

//
// given the roi of the initial edge, this computes onesided edges and onesided surface voxels
// needs ignoredids to be set for e.g. background (input argument)
// old function with ITK functions, faster version above
//void InitialNode::parallelComputeOnesidedSurfaceAndEdges(std::vector<SegmentIdType> *ignoredSegmentIds) {
//    // clear old values
//    onesidedEdges.clear();
//    onesidedSurfaceVoxels.clear();
//
//    using NeighborhoodIteratorType = itk::ConstShapedNeighborhoodIterator<SegmentIdImageType>;
//
//    SegmentIdImageType::IndexType startIndex = {roi.minX, roi.minY, roi.minZ};
//    SegmentIdImageType::SizeType size = {static_cast<unsigned long>(roi.maxX - roi.minX + 1),
//                                         static_cast<unsigned long>(roi.maxY - roi.minY + 1),
//                                         static_cast<unsigned long>(roi.maxZ - roi.minZ + 1)};
//    SegmentIdImageType::RegionType region = {startIndex, size};
//
//    NeighborhoodIteratorType::RadiusType radius;
//    radius.Fill(1);
//
//    NeighborhoodIteratorType neighborhoodIt(radius, pSegments, region);
//
//    NeighborhoodIteratorType::OffsetType off = {{0, 0, 0}};
//    NeighborhoodIteratorType::OffsetType off1 = {{1, 0, 0}};
//    NeighborhoodIteratorType::OffsetType off2 = {{-1, 0, 0}};
//    NeighborhoodIteratorType::OffsetType off3 = {{0, 1, 0}};
//    NeighborhoodIteratorType::OffsetType off4 = {{0, -1, 0}};
//    NeighborhoodIteratorType::OffsetType off5 = {{0, 0, 1}};
//    NeighborhoodIteratorType::OffsetType off6 = {{0, 0, -1}};
//
//    neighborhoodIt.ActivateOffset(off);
//    neighborhoodIt.ActivateOffset(off1);
//    neighborhoodIt.ActivateOffset(off2);
//    neighborhoodIt.ActivateOffset(off3);
//    neighborhoodIt.ActivateOffset(off4);
//    neighborhoodIt.ActivateOffset(off5);
//    neighborhoodIt.ActivateOffset(off6);
//    //4 10 12 14 16 22
//    //4 10 12 13 14 16 22
//
//    std::vector<unsigned int> offSetIndices = {4, 10, 12, 14, 16, 22};
//    unsigned int centerOffset = 13;
//
//    neighborhoodIt.GoToBegin();
//    std::vector<SegmentIdType> addedToLabelAlready;
//    SegmentIdType itLabel;
//    SegmentIdType newLabel;
//    size_t x, y, z;
//    NeighborhoodIteratorType::IndexType index;
//    NeighborhoodIteratorType::ConstIterator innerIterator;
//    bool IsInBounds;
//    bool isSurface;
//    Voxel voxelToAdd;
//
//    while (!neighborhoodIt.IsAtEnd()) {
//        itLabel = neighborhoodIt.GetPixel(centerOffset);
//        if (itLabel == label) {
//            addedToLabelAlready.clear();
//            isSurface = false;
//            index = neighborhoodIt.GetIndex();
//            x = index[0];
//            y = index[1];
//            z = index[2];
//            innerIterator = neighborhoodIt.Begin();
//            for (unsigned int i = 0; i < 6; ++i) {
//                newLabel = neighborhoodIt.GetPixel(offSetIndices[i], IsInBounds);
//                if (IsInBounds) {
//                    if ((newLabel != label) && !isIgnoredId(newLabel, ignoredSegmentIds)) {
//                        isSurface = true;
//                        voxelToAdd = Voxel(x, y, z);
//                        if (!onesidedEdges.count(newLabel)) {
//                            onesidedEdges[newLabel] = std::shared_ptr<InitialEdge>(new InitialEdge(newLabel, label));
//                            onesidedEdges[newLabel]->addVoxel(voxelToAdd);
//                            addedToLabelAlready.push_back(newLabel);
//                        } else {
//                            // if this exact voxel was not added to the same edge before
//                            if (find(addedToLabelAlready.begin(), addedToLabelAlready.end(), newLabel) ==
//                                addedToLabelAlready.end()) {
//                                onesidedEdges[newLabel]->addVoxel(voxelToAdd);
//                                addedToLabelAlready.push_back(newLabel);
//                            }
//                        }
//                    }
//                }
//            }
//            if (isSurface) {
//                onesidedSurfaceVoxels.emplace_back(x, y, z);
//            }
//        }
//        ++neighborhoodIt;
//    }
//}


// given a onesided edge, it will find the other, corrosponding onesided edge to form a twosided edge
// useful function when refining
InitialEdge *InitialNode::computeCorrospondingOneSidedEdge(InitialEdge *pInitialEdge, bool verbose) {
    double t = 0;
    if (verbose) {
        std::string desc =
                "InitialNode::computeCorrospondingOneSidedEdge (" + std::to_string(pInitialEdge->pairId.first) +
                "->" + std::to_string(pInitialEdge->pairId.second) + ") node: " + std::to_string(getLabel());
        t = utils::tic(desc);
    }

    using NeighborhoodIteratorType = itk::ConstShapedNeighborhoodIterator<SegmentIdImageType>;

//    this->roi.print();

    int iXMin, iYMin, iZMin, wXMax, wYMax, wZMax, wX, wY, wZ;
    iXMin = (roi.minX - 1) < 0 ? roi.minX : roi.minX - 1;
    iYMin = (roi.minY - 1) < 0 ? roi.minY : roi.minY - 1;
    iZMin = (roi.minZ - 1) < 0 ? roi.minZ : roi.minZ - 1;

    wXMax = pSegments->GetLargestPossibleRegion().GetSize()[0] - iXMin;
    wYMax = pSegments->GetLargestPossibleRegion().GetSize()[1] - iYMin;
    wZMax = pSegments->GetLargestPossibleRegion().GetSize()[2] - iZMin;

    wX = roi.maxX - roi.minX + 3 <= wXMax ? roi.maxX - roi.minX + 3 : wXMax;
    wY = roi.maxY - roi.minY + 3 <= wYMax ? roi.maxY - roi.minY + 3 : wYMax;
    wZ = roi.maxZ - roi.minZ + 3 <= wZMax ? roi.maxZ - roi.minZ + 3 : wZMax;


    SegmentIdImageType::IndexType startIndex = {iXMin, iYMin, iZMin};
    SegmentIdImageType::SizeType size = {static_cast<unsigned long>(wX),
                                         static_cast<unsigned long>(wY),
                                         static_cast<unsigned long>(wZ)};
    SegmentIdImageType::RegionType region = {startIndex, size};

    NeighborhoodIteratorType::RadiusType radius;
    radius.Fill(1);

    NeighborhoodIteratorType neighborhoodIt(radius, pSegments, region);

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

    itk::Image<bool, dataType::Dimension>::Pointer pVisitedBefore = itk::Image<bool, dataType::Dimension>::New();
    pVisitedBefore->SetRegions(region);
//    std::cout << region << "\n";
    pVisitedBefore->SetSpacing(pSegments->GetSpacing());
    pVisitedBefore->SetOrigin(pSegments->GetOrigin());
    pVisitedBefore->Allocate(true);
    pVisitedBefore->FillBuffer(false); // fill with false

    SegmentIdType corrospondingNodeId = utils::getOtherLabelOfPair(pInitialEdge->pairId, getLabel());

    neighborhoodIt.GoToBegin();
    std::vector<SegmentIdType> addedToLabelAlready;
    SegmentIdType newLabel;
    int x, y, z;
    NeighborhoodIteratorType::ConstIterator innerIterator;
    bool IsInBounds;
    Voxel voxelToAdd;

    InitialEdge *pNewEdge = new InitialEdge(corrospondingNodeId, label);

    for (auto voxel : pInitialEdge->voxels) {
        neighborhoodIt.SetLocation({voxel.x, voxel.y, voxel.z});
        innerIterator = neighborhoodIt.Begin();

        for (unsigned int i = 0; i < 6; ++i) {
            newLabel = neighborhoodIt.GetPixel(offSetIndices[i], IsInBounds);
            if (IsInBounds) {
                if (newLabel == corrospondingNodeId) {
                    auto index = neighborhoodIt.GetIndex(offSetIndices[i]);
//                    std::cout << index << "\n";
                    x = index[0];
                    y = index[1];
                    z = index[2];
                    if (pVisitedBefore->GetPixel({x, y, z}) == false) {
                        // if not added the voxel in a previous pass, make a logical mask that check it
                        // attention: in parallelcomputeedge, you iterate over the inner voxels of the node and add those to the edge.
                        // here, we iterate over the outer voxels. the simple labelAlreadyAdded-Set does not work here!
                        voxelToAdd = Voxel(x, y, z);
                        pNewEdge->addVoxel(voxelToAdd);
                        pVisitedBefore->SetPixel({x, y, z}, true);
                    }
                }
            }
        }
    }
    if (verbose) { utils::toc(t, "InitialNode::computeCorrospondingOneSidedEdge finished"); }
    return pNewEdge;
}



void InitialNode::print(int indentationLevel, std::ostream &outStream) {
    std::string indentationString = "";
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outStream << indentationString << "label: " << label << "\n";
    outStream << indentationString << "currentWorkingNodeId: " << currentWorkingNodeLabel << "\n";
    outStream << indentationString << "number of voxels: " << voxels.size() << "\n";
    outStream << indentationString << "number of surface voxels: " << onesidedSurfaceVoxels.size() << "\n";
    outStream << indentationString << "number of onesided edges: " << onesidedEdges.size() << "\n";
    outStream << indentationString << "number of twosided edges: " << twosidedEdges.size() << "\n";
    outStream << indentationString << "fx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << "\n";
    outStream << indentationString << "tx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << "\n";

    outStream << indentationString << "Onesided Edges:\n";
    for (auto &edge : onesidedEdges) {
        outStream << indentationString << "\tedgeKey: " << edge.second->pairId.first << " -> "
                  << edge.second->pairId.second << "\n";
        edge.second->print(indentationLevel + 3, outStream);
    }
    outStream << indentationString << "Twosided Edges:\n";
    for (auto &edge : twosidedEdges) {
        outStream << indentationString << "\tedgeKey: " << edge.second->pairId.first << " -> "
                  << edge.second->pairId.second << "\n";
        edge.second->print(indentationLevel + 3, outStream);
    }
    outStream << indentationString << "Node Features:\n";
    for (auto &feature : nodeFeatures) {
        outStream << indentationString << "\t" << feature->filterName << " " << feature->signalName << "\n";
        outStream << indentationString << "\t\t";
        for (auto &val : feature->values) {
            outStream << val << " ";
        }
        outStream << "\n";
    }

}


void InitialNode::setSegmentPointer(SegmentIdImageType::Pointer pSegmentsIn) {
    pSegments = pSegmentsIn;
}


// TODO: Implement me!
std::vector<BaseNode::SegmentIdType> InitialNode::getVectorOfConnectedNodeIds() {
    std::vector<BaseNode::SegmentIdType> idsOfConnectedNodes;
    for (auto &edge : onesidedEdges) {
        idsOfConnectedNodes.push_back(edge.first);
    }
    return idsOfConnectedNodes;
}

void InitialNode::addTwoSidedEdge(std::shared_ptr<InitialEdge> const &edgeToAdd) {
    SegmentIdType otherLabel = edgeToAdd->pairId.first == label ? edgeToAdd->pairId.second : edgeToAdd->pairId.first;
    twosidedEdges[otherLabel] = edgeToAdd;
}

BaseNode::SegmentIdType InitialNode::getCurrentWorkingNodeLabel() const {
    return currentWorkingNodeLabel;
}

void InitialNode::setCurrentWorkingNodeLabel(SegmentIdType currentWorkingNodeLabelIn) {
    currentWorkingNodeLabel = currentWorkingNodeLabelIn;
}

std::vector<Voxel> InitialNode::getVoxelArray() {
    return voxels;
}
