
#ifndef SEGMENTCOUPLER_INITIALNODE_H
#define SEGMENTCOUPLER_INITIALNODE_H


#include "src/segment_handling/baseNode.h"

//FIXME: keep edge.h? replace with initialEdge or sth?
#include "edge.h"
#include "initialEdge.h"
#include "feature.h"
#include <unordered_map>

class WorkingNode;

class InitialNode : public BaseNode {
public:

    InitialNode(std::shared_ptr<GraphBase> graphBaseIn, SegmentIdImageType::Pointer pSegmentsIn, SegmentIdType labelIn);

    InitialNode(std::shared_ptr<GraphBase> graphBaseIn, SegmentIdImageType::Pointer pSegmentsIn, SegmentIdType labelIn,
                int x, int y, int z);

    void addVoxel(int x, int y, int z);
    void addVoxel(Voxel voxelToAdd);
    void addVoxel(itk::Index<3> const &index);


    void addFeature(std::unique_ptr<Feature> &feature);


    void setSegmentPointer(SegmentIdImageType::Pointer pSegmentsIn);

    // print node properties
    void print(int indentationLevel, std::ostream &outStream) override;

    std::vector<std::vector<Voxel> *> getVoxelPointerArray() override;

    std::vector<Voxel> getVoxelArray() override;

    SegmentIdType getCurrentWorkingNodeLabel() const;

    void setCurrentWorkingNodeLabel(SegmentIdType currentWorkingNodeLabelIn);
    //TODO: Implemt!

    // get a vector of the ids of the connected nodes. useful to iterate over elements if normal iterators get invalided
    std::vector<SegmentIdType> getVectorOfConnectedNodeIds() override;

    void parallelComputeOnesidedSurfaceAndEdges(std::vector<SegmentIdType> *ignoredSegmentIds);

    void addTwoSidedEdge(std::shared_ptr<InitialEdge> const &edgeToAdd);


    void calculateNodeFeatures();

    InitialEdge *computeCorrospondingOneSidedEdge(InitialEdge *pInitialEdge, bool verbose = true);


    std::unordered_map<SegmentIdType, std::shared_ptr<InitialEdge>> onesidedEdges;
    std::unordered_map<SegmentIdType, std::shared_ptr<InitialEdge>> twosidedEdges;
    std::vector<Voxel> voxels;

    std::shared_ptr<GraphBase> graphBase;

    std::vector<std::unique_ptr<Feature>> nodeFeatures;


private:
    bool isIgnoredId(SegmentIdType idToCheck, std::vector<SegmentIdType> *ignoredSegmentIds);

    // this indicates the numId of the corresponding segment in the working copy
    SegmentIdType currentWorkingNodeLabel;

    std::vector<Voxel> onesidedSurfaceVoxels;


    SegmentIdImageType::Pointer pSegments;

};


#endif //SEGMENTCOUPLER_INITIALNODE_H
