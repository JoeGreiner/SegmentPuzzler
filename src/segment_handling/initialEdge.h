
#ifndef SEGMENTCOUPLER_INITIALEDGE_H
#define SEGMENTCOUPLER_INITIALEDGE_H

#include "src/segment_handling/baseEdge.h"
#include "feature.h"


class InitialEdge : public BaseEdge {
public:
    std::vector<std::vector<Voxel> const *> getVoxelPointerArray() override;

    std::vector<Voxel> const *getVoxelPointer();

    InitialEdge(SegmentIdType labelAIn, SegmentIdType labelBIn);

    // copy a initial edge. shouldMerge Flag is not copied!
    InitialEdge(InitialEdge const &copyEdge);


    std::vector<std::unique_ptr<Feature>> edgeFeatures;


    // add a feature to the edge features vector
    void addEdgeFeature(std::unique_ptr<Feature> &feature);


    // recalculate all features stored for this node
    void calculateEdgeFeatures();

    // merge roi and voxels with other edge
    void mergeVoxelsAndROIwithOtherEdge(InitialEdge *edgeToMerge);

    void addVoxel(Voxel &voxel);

    void print(int indentationLevel, std::ostream &outStream) override;

    size_t getNumberVoxels();

    bool getWasUsedToComputeTwoSidedEdge();

    void setWasUsedToComputeTwoSidedEdge(bool wasUsedToComputeTwoSidedEdgeIn);

    bool wasUsedToComputeTwoSidedEdge;

    // vector of all voxels in the edge
    std::vector<Voxel> voxels;
};


#endif //SEGMENTCOUPLER_INITIALEDGE_H
