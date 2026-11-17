#include "initialNode.h"
#include "workingNode.h"
#include "src/utils/AppLogger.h"
#include "src/utils/ConnectedComponentLabelSplitter.h"
#include "src/utils/utils.h"
#include <QElapsedTimer>
#include <itkConstShapedNeighborhoodIterator.h>
#include <cstdint>
#include <algorithm>
#include <tuple>
#include <stdexcept>

namespace {

using segment_puzzler::app_logging::AppLogger;
using segment_puzzler::app_logging::LogLevel;

void logInitialNodeDebug(const char *functionName, const QString &message) {
    AppLogger::log(LogLevel::Debug, QStringLiteral("segmentation"), message, functionName);
}

}


InitialNode::InitialNode(std::shared_ptr<GraphBase> graphBaseIn, dataType::SegmentsStorage::Pointer pSegmentsIn, SegmentIdType labelIn) {
    graphBase = graphBaseIn;
    pSegments = pSegmentsIn;
    label = labelIn;
    roi = Roi();

    for (auto &feature : FeatureList::nodeFeaturesList) {
        addFeature(feature);
    }
}

// creates initial node from flood filling into a label at a coordinate x,y,z
InitialNode::InitialNode(std::shared_ptr<GraphBase> graphBaseIn, dataType::SegmentsStorage::Pointer pSegmentsIn, SegmentIdType labelIn,
                         int x, int y, int z) {
    graphBase = graphBaseIn;
    pSegments = pSegmentsIn;
    label = labelIn;
    roi = Roi();

    for (auto &feature : FeatureList::nodeFeaturesList) {
        addFeature(feature);
    }

    using segment_puzzler::connected_components::ConnectivityStencil;
    using segment_puzzler::connected_components::visitLabelComponent;
    const SegmentIdImageType::IndexType seed{{x, y, z}};
    const auto traversal = visitLabelComponent(
        pSegmentsIn,
        seed,
        ConnectivityStencil::SixConnected,
        [this](const SegmentIdImageType::IndexType &index) {
            addVoxel(index);
            return true;
        });
    if (!traversal.completed || traversal.voxelCount == 0) {
        throw std::logic_error("Initial-node component traversal did not complete.");
    }
}


void InitialNode::addFeature(std::unique_ptr<Feature> &feature) {
    nodeFeatures.emplace_back(feature->createNew());
}

void InitialNode::calculateNodeFeatures() {
    for (auto &feature : nodeFeatures) {
        feature->compute(getVoxelLists());
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


VoxelLists InitialNode::getVoxelLists() const {
    return {&voxels};
}


bool InitialNode::isIgnoredId(SegmentIdType idToCheck,
                              const std::vector<SegmentIdType> &ignoredSegmentIds) const {
    // Check if idToCheck is in the ignored segment labels.
    return std::find(ignoredSegmentIds.begin(), ignoredSegmentIds.end(), idToCheck) != ignoredSegmentIds.end();
}

void InitialNode::computeOneSidedEdges(const std::vector<SegmentIdType> &ignoredSegmentIds) {
    neighborLabelToOneSidedInitialEdge.clear();

    if (voxels.empty()) {
        return;
    }

    const auto dimX = pSegments->GetLargestPossibleRegion().GetSize()[0];
    const auto dimY = pSegments->GetLargestPossibleRegion().GetSize()[1];
    const auto dimZ = pSegments->GetLargestPossibleRegion().GetSize()[2];

    if (roi.minX < 0 || roi.minY < 0 || roi.minZ < 0 ||
        roi.minX > roi.maxX || roi.minY > roi.maxY || roi.minZ > roi.maxZ ||
        static_cast<std::size_t>(roi.maxX) >= dimX ||
        static_cast<std::size_t>(roi.maxY) >= dimY ||
        static_cast<std::size_t>(roi.maxZ) >= dimZ) {
        throw std::logic_error("Initial node ROI is outside the segment image.");
    }

    constexpr unsigned int estimateNumberEdges = 30;
    neighborLabelToOneSidedInitialEdge.reserve(estimateNumberEdges);

    SegmentIdImageType::RegionType readRegion(
        {roi.minX - 1, roi.minY - 1, roi.minZ - 1},
        {static_cast<itk::SizeValueType>(roi.maxX - roi.minX + 3),
         static_cast<itk::SizeValueType>(roi.maxY - roi.minY + 3),
         static_cast<itk::SizeValueType>(roi.maxZ - roi.minZ + 3)});
    readRegion.Crop(pSegments->GetLargestPossibleRegion());
    struct RowRun { itk::IndexValueType begin, end; SegmentIdType label; };
    using Row = std::vector<RowRun>;
    const auto origin = readRegion.GetIndex();
    const auto height = readRegion.GetSize()[1];
    const auto endZ = origin[2] + static_cast<itk::IndexValueType>(readRegion.GetSize()[2]);
    std::vector<Row> previous(height), current(height);
    std::unordered_map<SegmentIdType, std::vector<VoxelList::Run>> contacts;
    const auto addContact = [&](SegmentIdType a, SegmentIdType b, SegmentIdImageType::IndexType first,
                                SegmentIdImageType::IndexType second, std::size_t length) {
        if (a == b || (a != label && b != label)) return;
        const auto neighbor = a == label ? b : a;
        const auto index = a == label ? first : second;
        if (isIgnoredId(neighbor, ignoredSegmentIds) || index[1] < roi.minY || index[1] > roi.maxY ||
            index[2] < roi.minZ || index[2] > roi.maxZ) return;
        const auto begin = std::max<itk::IndexValueType>(index[0], roi.minX);
        const auto end = std::min<itk::IndexValueType>(index[0] + length, std::int64_t(roi.maxX) + 1);
        if (begin < end) contacts[neighbor].push_back({Voxel(begin, index[1], index[2]), static_cast<std::uint32_t>(end - begin)});
    };
    const auto compareRows = [&](const Row &a, const Row &b, SegmentIdImageType::IndexType first,
                                 SegmentIdImageType::IndexType second) {
        std::size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            const auto begin = std::max(a[i].begin, b[j].begin);
            const auto end = std::min(a[i].end, b[j].end);
            first[0] = second[0] = begin;
            if (begin < end) addContact(a[i].label, b[j].label, first, second, end - begin);
            const auto endA = a[i].end, endB = b[j].end;
            if (endA <= endB) ++i;
            if (endB <= endA) ++j;
        }
    };
    readRegion.SetSize(2, 1);
    for (auto z = origin[2]; z < endZ; ++z) {
        for (auto &row : current) row.clear();
        readRegion.SetIndex(2, z);
        pSegments->forEachRun(readRegion, [&](const auto &index, std::size_t length, SegmentIdType value) {
            current[index[1] - origin[1]].push_back({index[0], index[0] + static_cast<itk::IndexValueType>(length), value});
        });
        for (std::size_t row = 0; row < height; ++row) {
            const auto y = origin[1] + static_cast<itk::IndexValueType>(row);
            const auto &runs = current[row];
            for (std::size_t i = 1; i < runs.size(); ++i)
                addContact(runs[i - 1].label, runs[i].label, {runs[i].begin - 1, y, z}, {runs[i].begin, y, z}, 1);
            if (row) compareRows(current[row - 1], runs, {0, y - 1, z}, {0, y, z});
            if (z > origin[2]) compareRows(previous[row], runs, {0, y, z - 1}, {0, y, z});
        }
        previous.swap(current);
    }
    for (auto &[neighbor, runs] : contacts) {
        std::sort(runs.begin(), runs.end(), [](const auto &a, const auto &b) {
            return std::tie(a.start.z, a.start.y, a.start.x) < std::tie(b.start.z, b.start.y, b.start.x);
        });
        auto end = runs.begin();
        for (const auto &run : runs) {
            if (end != runs.begin() && (end - 1)->start.z == run.start.z && (end - 1)->start.y == run.start.y &&
                std::int64_t((end - 1)->start.x) + (end - 1)->length >= run.start.x) {
                auto &last = *(end - 1);
                last.length = std::max<std::int64_t>(last.length, std::int64_t(run.start.x) + run.length - last.start.x);
            } else *end++ = run;
        }
        auto edge = std::make_shared<OneSidedInitialEdge>(neighbor, label);
        for (auto it = runs.begin(); it != end; ++it) edge->addRun(it->start, it->length);
        neighborLabelToOneSidedInitialEdge.emplace(neighbor, std::move(edge));
    }

}


// given a onesided edge, it will find the other, corrosponding onesided edge to form a twosided edge
// useful function when refining
OneSidedInitialEdge *InitialNode::computeCorrospondingOneSidedEdge(OneSidedInitialEdge *pInitialEdge, bool verbose) {
    QElapsedTimer timer;
    if (verbose) {
        logInitialNodeDebug(
            __func__,
            QStringLiteral("Computing corresponding one-sided edge %1->%2 on node %3")
                .arg(pInitialEdge->pairId.first)
                .arg(pInitialEdge->pairId.second)
                .arg(getLabel()));
        timer.start();
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

    const auto image = pSegments->readImageRegion(region);
    NeighborhoodIteratorType neighborhoodIt(radius, image, region);

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

    OneSidedInitialEdge *pNewEdge = new OneSidedInitialEdge(corrospondingNodeId, label);

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
    if (verbose) {
        const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        logInitialNodeDebug(__func__,
                            QStringLiteral("Finished corresponding one-sided edge computation (%1 ms)")
                                .arg(elapsedMs, 0, 'f', 3));
    }
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
    outStream << indentationString << "number of onesided edges: " << neighborLabelToOneSidedInitialEdge.size() << "\n";
    outStream << indentationString << "number of twosided edges: " << neighborLabelToTwoSidedInitialEdge.size() << "\n";
    outStream << indentationString << "fx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << "\n";
    outStream << indentationString << "tx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << "\n";

    outStream << indentationString << "Onesided Edges:\n";
    for (auto &edge : neighborLabelToOneSidedInitialEdge) {
        outStream << indentationString << "\tedgeKey: " << edge.second->pairId.first << " -> "
                  << edge.second->pairId.second << "\n";
        edge.second->print(indentationLevel + 3, outStream);
    }
    outStream << indentationString << "Twosided Edges:\n";
    for (auto &edge : neighborLabelToTwoSidedInitialEdge) {
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


void InitialNode::setSegmentPointer(dataType::SegmentsStorage::Pointer pSegmentsIn) {
    pSegments = pSegmentsIn;
}


// TODO: Implement me!
std::vector<BaseNode::SegmentIdType> InitialNode::getVectorOfConnectedNodeIds() {
    std::vector<BaseNode::SegmentIdType> idsOfConnectedNodes;
    for (auto &edge : neighborLabelToOneSidedInitialEdge) {
        idsOfConnectedNodes.push_back(edge.first);
    }
    return idsOfConnectedNodes;
}

void InitialNode::addTwoSidedInitialEdge(
        const std::shared_ptr<TwoSidedInitialEdge> &twoSidedInitialEdge) {
    SegmentIdType otherLabel = twoSidedInitialEdge->pairId.first == label
        ? twoSidedInitialEdge->pairId.second
        : twoSidedInitialEdge->pairId.first;
    neighborLabelToTwoSidedInitialEdge[otherLabel] = twoSidedInitialEdge;
}

BaseNode::SegmentIdType InitialNode::getCurrentWorkingNodeLabel() const {
    return currentWorkingNodeLabel;
}

void InitialNode::setCurrentWorkingNodeLabel(SegmentIdType currentWorkingNodeLabelIn) {
    currentWorkingNodeLabel = currentWorkingNodeLabelIn;
}

std::vector<Voxel> InitialNode::getVoxelArray() {
    std::vector<Voxel> result;
    result.reserve(voxels.size());
    result.insert(result.end(), voxels.begin(), voxels.end());
    return result;
}
