#ifndef graph_h
#define graph_h

#include "src/utils/voxel.h"

#include "initialEdge.h"
#include "workingEdge.h"
#include "workingNode.h"
#include "initialNode.h"
#include <map>
#include <itkImageFileWriter.h>

#include "src/file_definitions/dataTypes.h"

#include "node.h"
#include "SegmentManager.h"
#include "src/utils/utils.h"
#include <src/qtUtils/QBackgroundIdRadioBox.h>


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

        float distX = (x - B.x) * (x - B.x);
        float distY = (y - B.y) * (y - B.y);
        float distZ = (z - B.z) * (z - B.z);
        float total = distX + distY + distZ;
        if (std::isinf(total)) {
            std::cout << x << " " << y << " " << z << "\n";
            std::cout << B.x << " " << B.y << " " << B.z << "\n";
        }
        return distX + distY + distZ;
    }
};

class Graph : public QWidget{
    Q_OBJECT



public slots:
            // get the segment of the refinement watershed and put into the current segmentation map
    void refineSegmentByPosition(int x, int y, int z);
    void transferSegmentationSegmentToInitialSegment(int x, int y, int z);
    void receiveBackgroundIdStrategy(QString backgroundIdStrategyIn);

public:
    Graph(std::shared_ptr<GraphBase> graphBaseIn, bool verboseIn = true);
    void askForBackgroundStrategy();


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

    void transferSegmentsWithVolumeCriterion(double volumeThreshold=50000);

    void transferSegmentsWithRefinementOverlap();

    void exportDebugInformation();


    template<typename imageType>
    void ITKImageWriter(typename imageType::Pointer pImage, std::string filePathWriter) {
        double t = 0;
        if (verbose) {
            std::cout << "Graph::ITKImageWriter Writing to: " << filePathWriter << "\n";
            t = utils::tic();
        }
        using WriterType = itk::ImageFileWriter<imageType>;
        typename WriterType::Pointer writer = WriterType::New();
        writer->SetInput(pImage);
        writer->SetUseCompression(true);
        writer->SetFileName(filePathWriter);
        writer->Update();
        if (verbose) { utils::toc(t, "Graph::ITKImageWriter finished"); }
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

    void mergeSegmentsWithRefinementWatershed();




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



    SegmentManager segmentManager;

    void insertWorkingNodeInSegmentImage(WorkingNode &pWorkingNode);

    QBackgroundIdRadioBox* dialog;

};

#endif
