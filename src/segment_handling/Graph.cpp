#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <cmath>
#include <fstream>
#include "Graph.h"
#include <itkImageFileWriter.h>
#include <algorithm>
#include <queue>
#include <unordered_set>
#include <itkNeighborhoodIterator.h>
#include <itkBinaryThresholdImageFunction.h>
#include <QMessageBox>
#include <chrono>
#include "src/utils/utils.h"
#include "graphBase.h"

Graph::~Graph() = default;

Graph::Graph(std::shared_ptr<GraphBase> graphBaseIn, bool verboseIn) {
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

namespace {

struct StrokeSegmentInfo {
    std::array<double, 2> start;
    std::array<double, 2> end;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
};

struct StrokeMask {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;

    bool contains(double displayX, double displayY) const {
        if (pixels.empty() || width <= 0 || height <= 0) {
            return false;
        }
        const int pixelX = std::clamp(static_cast<int>(displayX), 0, width - 1);
        const int pixelY = std::clamp(static_cast<int>(displayY), 0, height - 1);
        return pixels[static_cast<std::size_t>(pixelY) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(pixelX)] != 0;
    }
};

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

struct ProjectedDisplayTransform {
    std::array<double, 4> clipXCoefficients{0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> clipYCoefficients{0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> clipWCoefficients{0.0, 0.0, 0.0, 1.0};
    int viewportWidth = 0;
    int viewportHeight = 0;
};

std::array<double, 4> buildProjectionRowCoefficients(const std::array<double, 16> &matrix,
                                                     int row,
                                                     const Graph::SegmentsImageType::SpacingType &spacing,
                                                     const Graph::SegmentsImageType::PointType &origin)
{
    const double centeredOriginX = origin[0] + 0.5 * spacing[0];
    const double centeredOriginY = origin[1] + 0.5 * spacing[1];
    const double centeredOriginZ = origin[2] + 0.5 * spacing[2];
    const std::size_t rowOffset = static_cast<std::size_t>(row) * 4;
    return {
        matrix[rowOffset + 0] * spacing[0],
        matrix[rowOffset + 1] * spacing[1],
        matrix[rowOffset + 2] * spacing[2],
        matrix[rowOffset + 0] * centeredOriginX +
        matrix[rowOffset + 1] * centeredOriginY +
        matrix[rowOffset + 2] * centeredOriginZ +
        matrix[rowOffset + 3]
    };
}

ProjectedDisplayTransform buildProjectedDisplayTransform(
    const Graph::SegmentsImageType::SpacingType &spacing,
    const Graph::SegmentsImageType::PointType &origin,
    const Projected3DCutRequest &request)
{
    ProjectedDisplayTransform transform;
    transform.clipXCoefficients = buildProjectionRowCoefficients(request.worldToNdcMatrix, 0, spacing, origin);
    transform.clipYCoefficients = buildProjectionRowCoefficients(request.worldToNdcMatrix, 1, spacing, origin);
    transform.clipWCoefficients = buildProjectionRowCoefficients(request.worldToNdcMatrix, 3, spacing, origin);
    transform.viewportWidth = request.viewportSize[0];
    transform.viewportHeight = request.viewportSize[1];
    return transform;
}

std::array<double, 2> projectVoxelCenterToDisplay(const Voxel &voxel,
                                                  const ProjectedDisplayTransform &transform)
{
    const double x = static_cast<double>(voxel.x);
    const double y = static_cast<double>(voxel.y);
    const double z = static_cast<double>(voxel.z);
    const double clipX =
        transform.clipXCoefficients[0] * x +
        transform.clipXCoefficients[1] * y +
        transform.clipXCoefficients[2] * z +
        transform.clipXCoefficients[3];
    const double clipY =
        transform.clipYCoefficients[0] * x +
        transform.clipYCoefficients[1] * y +
        transform.clipYCoefficients[2] * z +
        transform.clipYCoefficients[3];
    const double clipW =
        transform.clipWCoefficients[0] * x +
        transform.clipWCoefficients[1] * y +
        transform.clipWCoefficients[2] * z +
        transform.clipWCoefficients[3];

    double ndcX = 0.0;
    double ndcY = 0.0;
    if (std::abs(clipW) > 1e-9) {
        ndcX = clipX / clipW;
        ndcY = clipY / clipW;
    }

    return {
        (ndcX * 0.5 + 0.5) * static_cast<double>(transform.viewportWidth),
        (1.0 - (ndcY * 0.5 + 0.5)) * static_cast<double>(transform.viewportHeight)
    };
}

double distanceSquaredToSegment(const std::array<double, 2> &point,
                                const std::array<double, 2> &segA,
                                const std::array<double, 2> &segB)
{
    const double vx = segB[0] - segA[0];
    const double vy = segB[1] - segA[1];
    const double wx = point[0] - segA[0];
    const double wy = point[1] - segA[1];
    const double denom = vx * vx + vy * vy;
    if (denom <= 1e-9) {
        return wx * wx + wy * wy;
    }

    const double t = std::clamp((wx * vx + wy * vy) / denom, 0.0, 1.0);
    const double dx = point[0] - (segA[0] + t * vx);
    const double dy = point[1] - (segA[1] + t * vy);
    return dx * dx + dy * dy;
}

std::vector<StrokeSegmentInfo> buildStrokeSegments(const Projected3DCutRequest &request) {
    std::vector<StrokeSegmentInfo> segments;
    if (request.strokePixels.size() < 2) {
        return segments;
    }

    segments.reserve(request.strokePixels.size() - 1);
    for (std::size_t index = 1; index < request.strokePixels.size(); ++index) {
        StrokeSegmentInfo segment;
        segment.start = request.strokePixels[index - 1];
        segment.end = request.strokePixels[index];
        segment.minX = std::min(segment.start[0], segment.end[0]) - request.strokeWidthPixels;
        segment.maxX = std::max(segment.start[0], segment.end[0]) + request.strokeWidthPixels;
        segment.minY = std::min(segment.start[1], segment.end[1]) - request.strokeWidthPixels;
        segment.maxY = std::max(segment.start[1], segment.end[1]) + request.strokeWidthPixels;
        segments.push_back(segment);
    }
    return segments;
}

StrokeMask rasterizeStrokeMask(const Projected3DCutRequest &request,
                               const std::vector<StrokeSegmentInfo> &segments)
{
    StrokeMask mask;
    mask.width = request.viewportSize[0];
    mask.height = request.viewportSize[1];
    if (mask.width <= 0 || mask.height <= 0) {
        return mask;
    }

    mask.pixels.assign(static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height), 0);
    const double maxDistanceSquared = request.strokeWidthPixels * request.strokeWidthPixels;

    for (const auto &segment : segments) {
        const int minX = std::clamp(static_cast<int>(std::floor(segment.minX)), 0, mask.width - 1);
        const int maxX = std::clamp(static_cast<int>(std::ceil(segment.maxX)), 0, mask.width - 1);
        const int minY = std::clamp(static_cast<int>(std::floor(segment.minY)), 0, mask.height - 1);
        const int maxY = std::clamp(static_cast<int>(std::ceil(segment.maxY)), 0, mask.height - 1);

        for (int pixelY = minY; pixelY <= maxY; ++pixelY) {
            for (int pixelX = minX; pixelX <= maxX; ++pixelX) {
                const std::array<double, 2> pixelCenter{
                    static_cast<double>(pixelX) + 0.5,
                    static_cast<double>(pixelY) + 0.5};
                if (distanceSquaredToSegment(pixelCenter, segment.start, segment.end) <= maxDistanceSquared) {
                    mask.pixels[static_cast<std::size_t>(pixelY) * static_cast<std::size_t>(mask.width) +
                                static_cast<std::size_t>(pixelX)] = 1;
                }
            }
        }
    }
    return mask;
}

} // namespace


void Graph::constructFromVolume(itk::Image<SegmentIdType, 3>::Pointer pImage) {
    initializeEdgeVolumeAndEdgeStatus();
    updateBackgroundIdFromVolume(pImage);
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
    graphBase->pEdgesInitialSegmentsImage->FillBuffer(0);


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


    ownedEdgesSignal = std::make_unique<itkSignal<dataType::MappedEdgeIdType>>(
            graphBase->pEdgesInitialSegmentsImage);
    graphBase->pEdgesInitialSegmentsITKSignal = ownedEdgesSignal.get();
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

bool Graph::splitWorkingNodeByProjected3DCut(const Projected3DCutRequest &request,
                                             Projected3DCutProfile *profileOut) {
    using Clock = std::chrono::steady_clock;
    const auto durationMs = [](const Clock::time_point &start, const Clock::time_point &end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    if (profileOut != nullptr) {
        *profileOut = Projected3DCutProfile{};
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
    const auto spacing = graphBase->pWorkingSegmentsImage->GetSpacing();
    const auto origin = graphBase->pWorkingSegmentsImage->GetOrigin();
    auto *workingSegmentsBuffer = graphBase->pWorkingSegmentsImage->GetBufferPointer();
    const auto workingSegmentsSize = graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion().GetSize();
    const unsigned long strideZ = workingSegmentsSize[1] * workingSegmentsSize[0];
    const unsigned long strideY = workingSegmentsSize[0];
    const auto workingSegmentsLinearIndex = [strideY, strideZ](const Voxel &voxel) {
        return static_cast<unsigned long>(voxel.z) * strideZ +
               static_cast<unsigned long>(voxel.y) * strideY +
               static_cast<unsigned long>(voxel.x);
    };
    const Roi targetRoi = targetWorkingNode->roi;

    std::vector<Voxel> targetVoxels;
    std::vector<SegmentIdType> targetInitialLabels;
    std::vector<int> targetComponentIds;
    std::vector<unsigned char> targetProvisionalCut;
    std::vector<SegmentIdType> originalInitialLabels;
    std::unordered_map<SegmentIdType, std::vector<int>> voxelIndicesByInitialLabel;

    std::size_t totalVoxelCount = 0;
    for (const auto &initialNodeEntry : targetWorkingNode->subInitialNodes) {
        if (initialNodeEntry.second == nullptr) {
            continue;
        }
        originalInitialLabels.push_back(initialNodeEntry.first);
        totalVoxelCount += initialNodeEntry.second->voxels.size();
    }
    if (totalVoxelCount == 0) {
        return finish(false);
    }

    targetVoxels.reserve(totalVoxelCount);
    targetInitialLabels.reserve(totalVoxelCount);
    targetComponentIds.reserve(totalVoxelCount);
    targetProvisionalCut.reserve(totalVoxelCount);
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
        static_cast<std::size_t>(voxelGrid.sizeZ),
        -1);
    const auto strokeSegments = buildStrokeSegments(request);
    const auto rasterizeStrokeMaskStart = Clock::now();
    const StrokeMask strokeMask = rasterizeStrokeMask(request, strokeSegments);
    const double rasterizeStrokeMaskMs = durationMs(rasterizeStrokeMaskStart, Clock::now());
    const auto projectedDisplayTransform = buildProjectedDisplayTransform(spacing, origin, request);
    bool anyProvisionalCut = false;
    std::size_t provisionalCutVoxelCount = 0;

    const auto collectTargetVoxelsStart = Clock::now();
    for (const auto &initialNodeEntry : targetWorkingNode->subInitialNodes) {
        const SegmentIdType initialLabel = initialNodeEntry.first;
        const auto &voxels = initialNodeEntry.second->voxels;
        auto &indices = voxelIndicesByInitialLabel[initialLabel];
        indices.reserve(voxels.size());
        for (const Voxel &voxel : voxels) {
            const std::array<double, 2> projectedPoint =
                projectVoxelCenterToDisplay(voxel, projectedDisplayTransform);
            const bool provisionalCut = strokeMask.contains(projectedPoint[0], projectedPoint[1]);
            anyProvisionalCut = anyProvisionalCut || provisionalCut;
            provisionalCutVoxelCount += provisionalCut ? 1 : 0;
            targetVoxels.push_back(voxel);
            targetInitialLabels.push_back(initialLabel);
            targetComponentIds.push_back(-1);
            targetProvisionalCut.push_back(provisionalCut ? 1U : 0U);
            const int voxelIndex = static_cast<int>(targetVoxels.size()) - 1;
            indices.push_back(voxelIndex);
            voxelGrid.voxelIndices[voxelGrid.linearIndex(voxel.x, voxel.y, voxel.z)] = voxelIndex;
        }
    }
    if (profileOut != nullptr) {
        profileOut->targetVoxelCount = targetVoxels.size();
        profileOut->provisionalCutVoxelCount = provisionalCutVoxelCount;
        profileOut->rasterizeStrokeMaskMs = rasterizeStrokeMaskMs;
        profileOut->projectAndClassifyTargetVoxelsMs = durationMs(collectTargetVoxelsStart, Clock::now());
        profileOut->collectTargetVoxelsMs =
            profileOut->rasterizeStrokeMaskMs + profileOut->projectAndClassifyTargetVoxelsMs;
    }

    if (!anyProvisionalCut) {
        return finish(false);
    }

    int nextComponentId = 0;
    std::vector<int> openVoxelIndices;
    openVoxelIndices.reserve(targetVoxels.size());
    const auto connectedComponentsStart = Clock::now();
    for (int voxelIndex = 0; voxelIndex < static_cast<int>(targetVoxels.size()); ++voxelIndex) {
        if (targetProvisionalCut[static_cast<std::size_t>(voxelIndex)] != 0 ||
            targetComponentIds[static_cast<std::size_t>(voxelIndex)] != -1) {
            continue;
        }

        targetComponentIds[static_cast<std::size_t>(voxelIndex)] = nextComponentId;
        openVoxelIndices.clear();
        openVoxelIndices.push_back(voxelIndex);

        for (std::size_t queueIndex = 0; queueIndex < openVoxelIndices.size(); ++queueIndex) {
            const int activeIndex = openVoxelIndices[queueIndex];
            const Voxel &activeVoxel = targetVoxels[static_cast<std::size_t>(activeIndex)];

            voxelGrid.forEachPresentNeighborIndex(activeVoxel, [&](int neighborIndex) {
                if (targetProvisionalCut[static_cast<std::size_t>(neighborIndex)] != 0 ||
                    targetComponentIds[static_cast<std::size_t>(neighborIndex)] != -1) {
                    return;
                }
                targetComponentIds[static_cast<std::size_t>(neighborIndex)] = nextComponentId;
                openVoxelIndices.push_back(neighborIndex);
            });
        }

        ++nextComponentId;
    }
    if (profileOut != nullptr) {
        profileOut->finalComponentCount = nextComponentId;
        profileOut->connectedComponentsMs = durationMs(connectedComponentsStart, Clock::now());
    }

    if (nextComponentId < 2) {
        return finish(false);
    }

    std::vector<int> reassignQueue;
    reassignQueue.reserve(targetVoxels.size());
    const auto reassignCutVoxelsStart = Clock::now();
    for (int voxelIndex = 0; voxelIndex < static_cast<int>(targetVoxels.size()); ++voxelIndex) {
        if (targetComponentIds[static_cast<std::size_t>(voxelIndex)] != -1) {
            reassignQueue.push_back(voxelIndex);
        }
    }

    for (std::size_t queueIndex = 0; queueIndex < reassignQueue.size(); ++queueIndex) {
        const int activeIndex = reassignQueue[queueIndex];
        const Voxel &activeVoxel = targetVoxels[static_cast<std::size_t>(activeIndex)];
        const int componentId = targetComponentIds[static_cast<std::size_t>(activeIndex)];

        voxelGrid.forEachPresentNeighborIndex(activeVoxel, [&](int neighborIndex) {
            if (targetComponentIds[static_cast<std::size_t>(neighborIndex)] != -1) {
                return;
            }
            targetComponentIds[static_cast<std::size_t>(neighborIndex)] = componentId;
            reassignQueue.push_back(neighborIndex);
        });
    }
    if (profileOut != nullptr) {
        profileOut->reassignCutVoxelsMs = durationMs(reassignCutVoxelsStart, Clock::now());
    }

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
        return finish(false);
    }

    std::vector<NeighborWorkingGroup> neighborGroups;
    neighborGroups.reserve(targetWorkingNode->twosidedEdges.size());
    const auto collectNeighborGroupsStart = Clock::now();
    for (const auto &edgeEntry : targetWorkingNode->twosidedEdges) {
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

    const auto splitWorkingNodesStart = Clock::now();
    splitWorkingNodeIntoInitialNodes(request.targetWorkingLabel);
    for (const auto &neighborGroup : neighborGroups) {
        if (neighborGroup.initialLabels.size() > 1 && workingNodes.count(neighborGroup.workingLabel) > 0) {
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
        static_cast<std::size_t>(nextComponentId));
    std::vector<std::size_t> replacementVoxelCountsByComponent(
        static_cast<std::size_t>(nextComponentId), 0);
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
        segmentManager.computeSurfaceAndOneSidedEdgesOnInitialNode(initialNodes.at(initialLabel).get());
    }
    for (const SegmentIdType initialLabel : replacementInitialLabels) {
        segmentManager.computeCorrospondingOneSidedInitialEdges(initialNodes.at(initialLabel).get());
    }
    segmentManager.mergeNewOneSidedEdgesIntoTwosidedEdges();
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
    componentOrder.reserve(static_cast<std::size_t>(nextComponentId));
    for (int componentId = 0; componentId < nextComponentId; ++componentId) {
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

    for (std::size_t orderIndex = 0; orderIndex < componentOrder.size(); ++orderIndex) {
        const int componentId = componentOrder[orderIndex];
        auto &componentLabels = replacementInitialLabelsByComponent[static_cast<std::size_t>(componentId)];
        if (componentLabels.empty()) {
            continue;
        }

        const SegmentIdType workingLabel =
            orderIndex == 0 ? request.targetWorkingLabel : nextFreeId++;
        auto *replacementWorkingNode =
            new WorkingNode(componentLabels, workingLabel, initialNodes);
        segmentManager.addWorkingNode(replacementWorkingNode);
        touchedWorkingLabels.insert(workingLabel);
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

    return finish(true);
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
// calculate overlap of initial voxels with the refinement
// if edge connects two segments with the same overlaplabel, both greater than threshold, merge them
// if overlap higher than threshold, merge them

// future:
// use different labels for manually merged segments than automatically merged labels
// override automatic decision with manual decision, if available
void Graph::mergeSegmentsWithRefinement() {
    double t = 0, t1 = 0;
    if (verbose) {
        std::cout << "Graph::mergeSegmentsWithRefinement called: \n";
        t = utils::tic();
    }


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
        if (verbose) { utils::toc(t1, "Graph::mergeSegmentsWithRefinement Merging edges finished"); }


//    for (auto &feature : nodeFeatures) {
//        feature->compute(voxels);
//    }

    }
    if (verbose) { utils::toc(t, "Graph::mergeSegmentsWithRefinement finished"); }

}

void Graph::transferSegmentsWithRefinementOverlap() {
    double t = 0;
    if (verbose) {
        std::cout << "Graph::transferSegmentsWithRefinementOverlap called: \n";
        t = utils::tic();
    }

    if (graphBase->pSelectedSegmentation != nullptr) {
        if (graphBase->pSelectedRefinement != nullptr) {
            double overlapThreshold = 0.7;
            for (auto &node : workingNodes) {
                LabelOverlap overlapFeature = LabelOverlap();
            overlapFeature.setPToOtherLabelImagePointer(graphBase->pSelectedRefinement);
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

void Graph::setBackgroundIdStrategy(const std::string& backgroundIdStrategyIn) {
    backgroundIdStrategy = backgroundIdStrategyIn;
    if (verbose) {
        std::cout << "Graph::setBackgroundIdStrategy: " << backgroundIdStrategy << "\n";
    }
}

void Graph::updateBackgroundIdFromVolume(SegmentsImageType::Pointer pImage) {
    std::cout << "BackgroundIdStrategy: " << backgroundIdStrategy << "\n";
    if (backgroundIdStrategy == "backgroundIsHighestId") {
        backgroundId = getLargestSegmentId(pImage);
    } else if (backgroundIdStrategy == "backgroundIsLowestId") {
        backgroundId = getSmallestSegmentId(pImage);
    } else {
        throw std::invalid_argument("Received unknown backgroundIdStrategy in Graph::updateBackgroundIdFromVolume");
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


void Graph::deleteSegmentationLabel(SegmentIdType label) {
    if (graphBase->pSelectedSegmentation == nullptr) { return; }
    itk::ImageRegionIterator<SegmentsImageType> it(
        graphBase->pSelectedSegmentation,
        graphBase->pSelectedSegmentation->GetLargestPossibleRegion());
    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        if (it.Get() == label) {
            it.Set(backgroundId);
        }
    }
}


void Graph::transferSegmentationSegmentToInitialSegment(int x, int y, int z) {
// high level workflow: create volume with just the one segment in background
// treat that new segment as a normal refinement segmentation call
    if (graphBase->pSelectedSegmentation == nullptr) {
        std::cout << "Graph::transferSegmentationSegmentToInitialSegment: no selected segmentation loaded\n";
        return;
    }
    if (graphBase->pWorkingSegmentsImage == nullptr) {
        std::cout << "Graph::transferSegmentationSegmentToInitialSegment: no working segments loaded\n";
        return;
    }

    auto label = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
    const SegmentIdType backgroundLabel = backgroundId;

    if (label == backgroundLabel) {
        std::cout << "Graph::transferSegmentationSegmentToInitialSegment: Label matches background label ("
                  << backgroundLabel << "), not transferring\n";
        return;
    }

//    create temporary refinement
    auto temporaryRefinement = SegmentsImageType::New();
    temporaryRefinement->SetRegions(graphBase->pSelectedSegmentation->GetLargestPossibleRegion());
    temporaryRefinement->Allocate();
    temporaryRefinement->FillBuffer(backgroundLabel);

//    just transfer the whole image and only copy if label matches
    itk::ImageRegionConstIterator<SegmentsImageType> it(graphBase->pSelectedSegmentation,
                                                        graphBase->pSelectedSegmentation->GetLargestPossibleRegion());
    itk::ImageRegionIterator<SegmentsImageType> itRefinement(temporaryRefinement,
                                                             temporaryRefinement->GetLargestPossibleRegion());
    std::cout << "Label: " << label << "\n";
    for (it.GoToBegin(), itRefinement.GoToBegin(); !it.IsAtEnd(); ++it, ++itRefinement) {
        if (it.Get() == label) {
            itRefinement.Set(label);
        }
    }

//    std::unique_ptr<itkSignal<unsigned char>> pSignal2(new itkSignal<unsigned char>(pImage));

//    auto temporaryRefinementSignal = itkSignal<dataType::SegmentIdType>(temporaryRefinement);
    auto temporaryRefinementSignal =
        std::make_shared<itkSignal<dataType::SegmentIdType>>(temporaryRefinement);

    auto previousSelectedRefinement = graphBase->pSelectedRefinement;
    auto previousSelectedRefinementSignal = graphBase->pSelectedRefinementSignal;

    // Reuse the normal refinement-by-position path by temporarily swapping in
    // both selected-refinement pointers for a refinement built from the clicked
    // segmentation label, then restore the previous selection afterwards.
    graphBase->pSelectedRefinement = temporaryRefinement;
    graphBase->pSelectedRefinementSignal = temporaryRefinementSignal.get();

    refineWithSelectedRefinementAtPosition(x, y, z);
    graphBase->pSelectedRefinement = previousSelectedRefinement;
    graphBase->pSelectedRefinementSignal = previousSelectedRefinementSignal;
}


// highlevel workflow:
// * get the background id of the refinement
// * check if the clicked label is not background
// * do a floodfill on the clicked pixel to get the refined segments voxels
void Graph::refineWithSelectedRefinementAtPosition(int x, int y, int z) {
    double t = 0, t1 = 0, t2 = 0;
    if (verbose) { t = utils::tic("Graph::refineWithSelectedRefinementAtPosition called"); }
    auto &selectedRefinement = graphBase->pSelectedRefinement;
    auto *selectedRefinementSignal = graphBase->pSelectedRefinementSignal;
    if (selectedRefinement == nullptr || selectedRefinementSignal == nullptr) {
        std::cout << "Selected refinement was never initialized, add a refinement before refining!\n";
        return;
    }

    bool coordinates_in_ROI = true;
    if (selectedRefinementSignal->ROI_set){

        if(x < selectedRefinementSignal->ROI_fx){
            coordinates_in_ROI = false;
        }
        if(y < selectedRefinementSignal->ROI_fy){
            coordinates_in_ROI = false;
        }
        if(z < selectedRefinementSignal->ROI_fz){
            coordinates_in_ROI = false;
        }

        if(x > selectedRefinementSignal->ROI_tx){
            coordinates_in_ROI = false;
        }
        if(y > selectedRefinementSignal->ROI_ty){
            coordinates_in_ROI = false;
        }
        if(z > selectedRefinementSignal->ROI_tz){
            coordinates_in_ROI = false;
        }
    }

    if(coordinates_in_ROI) {
        if (verbose) { t1 = utils::tic(); }
//        SegmentIdType backgroundLabelInRefinement = getLargestIdInSegmentVolume(selectedRefinement);

        const SegmentIdType backgroundLabel = backgroundId;
        SegmentIdType labelInRefinement = selectedRefinement->GetPixel({x, y, z});
        SegmentIdType labelToInsertTarget = nextFreeId;
        nextFreeId++;

        int msgBoxAnswer = QMessageBox::Yes;

        bool hardExitIfLabelInRefinementIsBackground = true;
        if (labelInRefinement == backgroundLabel) {
            if (hardExitIfLabelInRefinementIsBackground){
                return;
            }
            QMessageBox msgBox;
            msgBox.setWindowTitle("Refinement");
            msgBox.setText(QString("You're trying to refine a segment with the background label (%1) in the selected refinement. Continue?")
                               .arg(backgroundLabel));
            msgBox.setStandardButtons(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::No);
            msgBox.setDefaultButton(QMessageBox::No);
            msgBoxAnswer = msgBox.exec();
        }

        if (msgBoxAnswer == QMessageBox::Yes) {
            // create new initial node by flood-filling the refinement
            std::cout << "Inserting Initial Node (id: " << labelToInsertTarget << ")\n";
            InitialNode *newInitialNode = new InitialNode(graphBase, selectedRefinement, labelToInsertTarget, x,
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
//                printf("INSERT: %d %d %d label: %d\n", voxel.x, voxel.y, voxel.z, labelToInsertTarget);
                    graphBase->pWorkingSegmentsImage->SetPixel({voxel.x, voxel.y, voxel.z}, labelToInsertTarget);
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

                auto newWorkingNode = new WorkingNode(newInitialNode, labelToInsertTarget, initialNodes);
                segmentManager.addWorkingNode(newWorkingNode);
                segmentManager.recalculateEdgesOnWorkingNode(newWorkingNode);

                //TODO: check voxelwise if refinement split a initial node into two segments
                if (verbose) { utils::toc(t2, "second part finished"); }

        } else {
            std::cout << "Label to insert matches background label (" << backgroundLabel
                      << "), refinement is not done.\n";
        }
    } else {
        std::cout << "Clicked point outside of refinement ROI.\n";
    }

    if (verbose) { utils::toc(t, "Graph::refineWithSelectedRefinementAtPosition finished"); }

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
    if (graphBase->pSelectedSegmentationSignal != nullptr) {
        graphBase->pSelectedSegmentationSignal->checkAndResizeLUT(graphBase->selectedSegmentationMaxSegmentId);
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
