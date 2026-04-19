#ifndef graph_h
#define graph_h

#include <memory>
#include <QElapsedTimer>
#include "src/utils/voxel.h"
#include "Projected3DCut.h"

#include "initialEdge.h"
#include "workingEdge.h"
#include "workingNode.h"
#include "initialNode.h"
#include <map>
#include <itkImageFileWriter.h>

#include "src/file_definitions/dataTypes.h"

#include "node.h"
#include "SegmentManager.h"
#include "src/utils/AppLogger.h"
#include "src/utils/utils.h"
// todo: add this to node/featurres or whatever
struct CenterOfMass {
    CenterOfMass() {}


    CenterOfMass(InitialNode *initialNode) {
        x = 0;
        y = 0;
        z = 0;
        for (auto voxel : initialNode->voxels) {
            x += voxel.x;
            y += voxel.y;
            z += voxel.z;
        }
        x /= initialNode->voxels.size();
        y /= initialNode->voxels.size();
        z /= initialNode->voxels.size();
    }

    CenterOfMass(InitialEdge *initialEdge) {
        x = 0;
        y = 0;
        z = 0;
        for (auto voxel : initialEdge->voxels) {
            x += voxel.x;
            y += voxel.y;
            z += voxel.z;
        }
        x /= initialEdge->voxels.size();
        y /= initialEdge->voxels.size();
        z /= initialEdge->voxels.size();
    }

    float x;
    float y;
    float z;

    float distTo(CenterOfMass B) {
        return std::sqrt(distToSquared(B));
    }

    float distToSquared(CenterOfMass B) {

        float distX = (x - B.x) * (x - B.x);
        float distY = (y - B.y) * (y - B.y);
        float distZ = (z - B.z) * (z - B.z);
        float total = distX + distY + distZ;
        if (std::isinf(total)) {
            ::segment_puzzler::app_logging::AppLogger::log(
                ::segment_puzzler::app_logging::LogLevel::Warning,
                QStringLiteral("segmentation"),
                QStringLiteral("CenterOfMass distance overflow from (%1, %2, %3) to (%4, %5, %6)")
                    .arg(x, 0, 'g', 6)
                    .arg(y, 0, 'g', 6)
                    .arg(z, 0, 'g', 6)
                    .arg(B.x, 0, 'g', 6)
                    .arg(B.y, 0, 'g', 6)
                    .arg(B.z, 0, 'g', 6),
                __func__);
        }
        return distX + distY + distZ;
    }
};

// forward declaration — avoids pulling itkSignal.h into every Graph.h includer
template<typename T> class itkSignal;

class Graph {
public:
    Graph(std::shared_ptr<GraphBase> graphBaseIn, bool verboseIn = true);
    ~Graph();  // defined in Graph.cpp (needed for unique_ptr<itkSignal<...>> with forward decl)

    // Refine using the currently selected refinement image and signal at the given voxel.
    void refineWithSelectedRefinementAtPosition(int x, int y, int z);
    bool splitWorkingNodeByProjected3DCut(const Projected3DCutRequest &request,
                                          Projected3DCutProfile *profileOut = nullptr);
    void transferSegmentationSegmentToInitialSegment(int x, int y, int z);
    void setBackgroundIdStrategy(const std::string& backgroundIdStrategyIn);


    static constexpr int Dimension = 3;
    using SegmentIdType = dataType::SegmentIdType;
    using SegmentsImageType = dataType::SegmentsImageType;
    using EdgePairIdType =  dataType::EdgePairIdType;
    using EdgeNumIdType =  dataType::EdgeNumIdType;
    using LandscapeType = dataType::LandscapeType;
    using DistanceType = dataType::DistanceType;


    //    Implemented: backgroundIsHighestId, backgroundIsLowestId
    std::string backgroundIdStrategy;
    SegmentIdType backgroundId;


    SegmentIdType nextFreeId;

    std::shared_ptr<GraphBase> graphBase;

    // initial nodes, as they come out of the watershed
    // note, the edge of initial nodes are 1vx wide (inside the node volume itself)
    // that means, on a interface between two nodes are two 1vx wide edges
    std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> initialNodes;

    // not unordered map because of hashfunction for pairs
    std::map<EdgePairIdType, std::shared_ptr<InitialEdge>> initialTwoSidedEdges;
    std::map<EdgePairIdType, std::shared_ptr<InitialEdge>> initialOneSidedEdges;

    // these nodes are created by operations on the initialNodes
    // note, the edges of workingnodes are 2vx widge, i.e. the one-sided edges are merged into a 2vx edge
    std::unordered_map<SegmentIdType, std::shared_ptr<WorkingNode>> workingNodes;
    std::map<EdgePairIdType, std::shared_ptr<WorkingEdge>> workingEdges;

    // lookup for the annotationsliceviewer that maps numId -> pairId
    std::unordered_map<EdgeNumIdType, EdgePairIdType> initialEdgeIdLookup;

    // construct a graph from
    void constructFromVolume(itk::Image<SegmentIdType, 3>::Pointer pImage);
    void updateBackgroundIdFromVolume(SegmentsImageType::Pointer pImage);

    void initializeEdgeVolumeAndEdgeStatus();

    SegmentIdType getLargestIdInSegmentVolume(SegmentsImageType::Pointer pSegment);

    // generate a landscape/path with the allowedsegmentids
    LandscapeType::Pointer
    generateLandscapePathfinding(SegmentsImageType::Pointer pSegments, SegmentIdType allowedWorkingNodeLabel,
                                 InitialEdge &forbiddenEdge);


    // calculate the shortest path between two points
    // TODO: return shortest path?
    // TODO: Dijkstra, AStar?
    DistanceType::Pointer shortestPath(InitialEdge &initialEdge, LandscapeType::Pointer pLandscape);

    // transfer working node at a given coordinate to the segmentation image
    void transferWorkingNodeToSegmentation(int x, int y, int z);

    void transferWorkingNodeToSegmentation(SegmentIdType labelOfNodeToTransfer);

    // Delete all pixels with the given label from pSelectedSegmentation (sets them to backgroundId).
    void deleteSegmentationLabel(SegmentIdType label);

    void transferSegmentsWithVolumeCriterion(double volumeThreshold=50000);

    void transferSegmentsWithRefinementOverlap();

    void exportDebugInformation();


    template<typename imageType>
    void ITKImageWriter(typename imageType::Pointer pImage, std::string filePathWriter) {
        QElapsedTimer timer;
        if (verbose) {
            ::segment_puzzler::app_logging::AppLogger::log(
                ::segment_puzzler::app_logging::LogLevel::Info,
                QStringLiteral("io"),
                QStringLiteral("Graph::ITKImageWriter writing to %1").arg(QString::fromStdString(filePathWriter)),
                __func__);
            timer.start();
        }
        auto spacing = pImage->GetSpacing();
        auto origin = pImage->GetOrigin();
        auto direction = pImage->GetDirection();
        ::segment_puzzler::app_logging::AppLogger::log(
            ::segment_puzzler::app_logging::LogLevel::Info,
            QStringLiteral("io"),
            QStringLiteral("Graph::ITKImageWriter target=%1 spacing=[%2, %3, %4] origin=[%5, %6, %7] direction=[[%8, %9, %10], [%11, %12, %13], [%14, %15, %16]]")
                .arg(QString::fromStdString(filePathWriter))
                .arg(spacing[0], 0, 'g', 6)
                .arg(spacing[1], 0, 'g', 6)
                .arg(spacing[2], 0, 'g', 6)
                .arg(origin[0], 0, 'g', 6)
                .arg(origin[1], 0, 'g', 6)
                .arg(origin[2], 0, 'g', 6)
                .arg(direction[0][0], 0, 'g', 6)
                .arg(direction[0][1], 0, 'g', 6)
                .arg(direction[0][2], 0, 'g', 6)
                .arg(direction[1][0], 0, 'g', 6)
                .arg(direction[1][1], 0, 'g', 6)
                .arg(direction[1][2], 0, 'g', 6)
                .arg(direction[2][0], 0, 'g', 6)
                .arg(direction[2][1], 0, 'g', 6)
                .arg(direction[2][2], 0, 'g', 6),
            __func__);
        using WriterType = itk::ImageFileWriter<imageType>;
        typename WriterType::Pointer writer = WriterType::New();
        writer->SetInput(pImage);
        writer->SetUseCompression(true);
        writer->SetFileName(filePathWriter);
        writer->Update();
        if (verbose) {
            ::segment_puzzler::app_logging::AppLogger::log(
                ::segment_puzzler::app_logging::LogLevel::Debug,
                QStringLiteral("io"),
                QStringLiteral("Graph::ITKImageWriter finished (%1 ms)")
                    .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000.0, 0, 'f', 3),
                __func__);
        }
    };

    // print graph structure to file
    void printInitialNodes(std::ostream &outStream = std::cout);

    void printInitialOneSidedEdges(std::ostream &outStream = std::cout);

    void printInitialTwoSidedEdges(std::ostream &outStream = std::cout);

    void printWorkingNodes(std::ostream &outStream = std::cout);

    void printEdgeIdLookUp(std::ostream &outStream = std::cout);

    void printWorkingEdges(std::ostream &outStream = std::cout);

    void printInitialNodesToFile(const std::string &pathToOutputfile);

    void printInitialTwoSidedEdgesToFile(const std::string &pathToOutputfile);

    void printInitialOneSidedEdgesToFile(const std::string &pathToOutputfile);

    void printWorkingNodesToFile(const std::string &pathToOutputfile);

    void printEdgeIdLookUpToFile(const std::string &pathToOutputfile);

    void printWorkingEdgesToFile(const std::string &pathToOutputfile);

    // merge to nodes by specifying a edge that merges two nodes
    void mergeEdge(InitialEdge *edge, bool updateSegmentImage = true);

    void mergeEdges(std::set<EdgeNumIdType> &vecOfEdgeIdsToMerge);

    void splitIntoConnectedComponentsOfWorkingNode(WorkingNode &workingNodeToAnalyze);

    void splitWorkingNodeIntoInitialNodes(int x, int y, int z);

    void splitWorkingNodeIntoInitialNodes(SegmentIdType workingNodeIdToSplit);

    void removeInitialNodeFromWorkingNodeAtPosition(int x, int y, int z);

    void mergeSegmentsWithRefinement();




    // undo a merged edge
    void unmergeEdges(std::set<EdgeNumIdType> &vecOfEdgeIdsToUnMerge);

    void unmergeEdge(InitialEdge *initialEdge, DistanceType::Pointer pDistanceSmaller,
                     DistanceType::Pointer pDistanceBigger);

    void unmergeEdge(WorkingNode *workingNodeToSplit, std::vector<SegmentIdType> initialNodeIdsA,
                     std::vector<SegmentIdType> initialNodeIdsB);


    std::pair<std::vector<SegmentIdType>, std::vector<SegmentIdType>>
    calculateGraphDistancesFromEdge(WorkingNode *nodeToCalculateDistanceOn, InitialEdge *edgeToCalculateDistanceFrom);


    // set pointer to ignoredSegments vector
    void setPointerToIgnoredSegmentLabels(std::vector<SegmentIdType> *pIgnoredSegmentLabels);

    SegmentIdType getNextFreeId(itk::Image<SegmentIdType, 3>::Pointer pImage);


private:
    void constructInitialNodes(itk::Image<SegmentIdType, 3>::Pointer pImage);

    void generateWorkingCopyOfInitialNodesAndEdges();

    SegmentIdType getLargestSegmentId(itk::Image<SegmentIdType, 3>::Pointer pImage);
    SegmentIdType getSmallestSegmentId(itk::Image<SegmentIdType, 3>::Pointer pImage);


    void mergeOneSidedEdgesIntoTwosidedEdges();

    bool isIgnoredId(SegmentIdType idToCheck);

    SegmentsImageType::RegionType getDilatedRegionFromRoi(Roi roi, SegmentsImageType::SizeType imageMax,
                                                          int numberVxDilations = 0);


    std::vector<SegmentIdType> *pIgnoredSegmentLabels;
    bool verbose;

    std::unique_ptr<itkSignal<dataType::MappedEdgeIdType>> ownedEdgesSignal;

    SegmentManager segmentManager;

    void insertWorkingNodeInSegmentImage(WorkingNode &pWorkingNode);

};

#endif
