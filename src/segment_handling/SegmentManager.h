
#ifndef SEGMENTCOUPLER_SEGMENTMANAGER_H
#define SEGMENTCOUPLER_SEGMENTMANAGER_H

#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/initialNode.h"
#include "src/segment_handling/workingNode.h"


#include <unordered_map>
#include <fstream>


// this should contain all the data for one graph
class SegmentManager {
public:
    using SegmentIdType = dataType::SegmentIdType;
    using EdgePairIdType = dataType::EdgePairIdType;
    using EdgeNumIdType = dataType::EdgeNumIdType;

    SegmentManager() = default;

//TODO: manage nextfreeid completely!
    SegmentManager(
            std::shared_ptr<GraphBase> graphBaseIn,
            std::unordered_map<dataType::SegmentIdType, std::shared_ptr<InitialNode>> *pInitialNodesIn,
            std::map<dataType::EdgePairIdType, std::shared_ptr<InitialEdge>> *pInitialOneSidedEdgesIn,
            std::map<dataType::EdgePairIdType, std::shared_ptr<InitialEdge>> *pInitialTwoSidedEdgesIn,
            std::unordered_map<dataType::EdgeNumIdType, dataType::EdgePairIdType> *pInitialEdgeIdLookUpIn,
            std::unordered_map<char, std::vector<unsigned char>> *pColorLookUpEdgesStatusIn,
            std::unordered_map<dataType::MappedEdgeIdType, char> *pEdgeStatusIn,
            dataType::EdgeImageType::Pointer *ppEdgesInitialSegmentsImageIn,
            dataType::SegmentsImageType::Pointer *ppWorkingSegmentsImageIn,
            std::unordered_map<dataType::SegmentIdType, std::shared_ptr<WorkingNode>> *pWorkingNodesIn,
            std::map<dataType::EdgePairIdType, std::shared_ptr<WorkingEdge>> *pWorkingEdgesIn,
            std::vector<SegmentIdType> *pIgnoredSegmentLabelsIn,
            SegmentIdType *nextFreeIdIn,
            bool verboseIn = true

    ) :
            graphBase(graphBaseIn),
            pInitialNodes(pInitialNodesIn),
            pInitialOneSidedEdges(pInitialOneSidedEdgesIn),
            pInitialTwoSidedEdges(pInitialTwoSidedEdgesIn),
            pInitialEdgeIdLookUp(pInitialEdgeIdLookUpIn),
            pColorLookUpEdgesStatus(pColorLookUpEdgesStatusIn),
            pEdgeStatus(pEdgeStatusIn),
            ppEdgesInitialSegmentsImage(ppEdgesInitialSegmentsImageIn),
            pIgnoredSegmentLabels(pIgnoredSegmentLabelsIn),
            ppWorkingSegmentsImage(ppWorkingSegmentsImageIn),
            pWorkingNodes(pWorkingNodesIn),
            pWorkingEdges(pWorkingEdgesIn),
            nextFreeId(nextFreeIdIn),
            verbose(verboseIn) {};

    void setPointerToIgnoredSegmentsLabels(std::vector<SegmentIdType> *pIgnoredSegmentLabelsIn);

    void clearAndReserveInitialNodes(int numberOfNodesToReserveFor = 0);

    void addInitialNode(SegmentIdType labelOfNewNode, int reserveMemoryForVoxels = 0);

    void addInitialNode(InitialNode *pInitialNodeToAdd);

    void removeEdgePropertiesOnInitialNode(InitialNode *pInitialNode);

    void removeInitialNode(SegmentIdType labelOfNodeToRemove);

    void splitWorkingNodeIntoInitialNodes(SegmentIdType workingNodeIdToSplit);

    void splitIntoConnectedComponentsOfWorkingNode(WorkingNode &workingNodeToAnalyze);

    void removeInitialNodeFromWorkingNodeAtPosition(int x, int y, int z);

    void insertWorkingNodeIntoITKImage(WorkingNode *newWorkingNode);

    void computeCorrospondingOneSidedInitialEdges(InitialNode *pInitialNode);

    void computeSurfaceAndOneSidedEdgesOnInitialNode(InitialNode *pInitialNode);

    void computeSurfaceAndOneSidedEdgesOnAllInitialNodes();

    void recomputeVoxelListAndOneSidedEdgesIfShrinked(std::vector<SegmentIdType> vecOfConnectedInitialNodeIds);

    void addOneSidedInitialEdge(std::shared_ptr<InitialEdge> pEdgeToAdd, EdgePairIdType pairId);

//    void removeOneSidedInitialEdge();
    InitialEdge *createTwoSidedInitialEdgeByMerging(SegmentIdType initialNodeLabelA, SegmentIdType initialNodeLabelB);

    void addTwoSidedInitialEdge(InitialEdge *pEdgeToAdd);

//    void removeTwoSidedInitialEdge();
    void mergeNewOneSidedEdgesIntoTwosidedEdges(bool veryVerbose = false);

    void convertAllInitialNodesIntoWorkingNodes();

//
    void constructWorkingNodeFromInitialNode(InitialNode *baseInitialNode, bool useSameIdAsInitialNode = true,
                                             bool veryVerbose = false);

    void addWorkingNode(WorkingNode *workingNodeToAdd);

    void removeWorkingNode(WorkingNode *workingNodeToRemove, bool veryVerbose = false);

    void addWorkingEdge(WorkingEdge *pWorkingEdgeToAdd);

    void removeWorkingEdge(WorkingEdge *workingEdgeToRemove);

    void recalculateEdgesOnWorkingNode(WorkingNode *pWorkingNode, bool veryVerbose = false);

    bool isIgnoredId(SegmentIdType idToCheck);


    void printInitialNodes(std::ostream &outStream = std::cout);
    void printInitialOneSidedEdges(std::ostream &outStream = std::cout);
    void printInitialTwoSidedEdges(std::ostream &outStream = std::cout);
    void printInitialNodesToFile(const std::string &pathToOutputfile);
    void printInitialTwoSidedEdgesToFile(const std::string &pathToOutputfile);
    void printInitialOneSidedEdgesToFile(const std::string &pathToOutputfile);
    void printWorkingNodes(std::ostream &outStream);
    void printEdgeIdLookUp(std::ostream &outStream);
    void printWorkingEdges(std::ostream &outStream);
    void printWorkingNodesToFile(const std::string &pathToOutputfile);
    void printEdgeIdLookUpToFile(const std::string &pathToOutputfile);
    void printWorkingEdgesToFile(const std::string &pathToOutputfile);

    std::shared_ptr<GraphBase> graphBase;

private:
    // initial nodes & edges
    std::unordered_map<dataType::SegmentIdType, std::shared_ptr<InitialNode>> *pInitialNodes;
    std::map<dataType::EdgePairIdType, std::shared_ptr<InitialEdge>> *pInitialOneSidedEdges;
    std::map<dataType::EdgePairIdType, std::shared_ptr<InitialEdge>> *pInitialTwoSidedEdges;
    std::unordered_map<dataType::EdgeNumIdType, dataType::EdgePairIdType> *pInitialEdgeIdLookUp;
    std::unordered_map<char, std::vector<unsigned char>> *pColorLookUpEdgesStatus;
    std::unordered_map<dataType::MappedEdgeIdType, char> *pEdgeStatus;
    dataType::EdgeImageType::Pointer *ppEdgesInitialSegmentsImage;
    std::vector<SegmentIdType> *pIgnoredSegmentLabels;

    // working segments & edges
    dataType::SegmentsImageType::Pointer *ppWorkingSegmentsImage;
    std::unordered_map<dataType::SegmentIdType, std::shared_ptr<WorkingNode>> *pWorkingNodes;
    std::map<dataType::EdgePairIdType, std::shared_ptr<WorkingEdge>> *pWorkingEdges;

    SegmentIdType *nextFreeId;
    bool verbose;
};

#endif //SEGMENTCOUPLER_SEGMENTMANAGER_H
