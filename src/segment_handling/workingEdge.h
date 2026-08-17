
#ifndef SEGMENTCOUPLER_WORKINGEDGE_H
#define SEGMENTCOUPLER_WORKINGEDGE_H


#include "src/segment_handling/baseEdge.h"
#include "graphBase.h"
#include "twoSidedInitialEdge.h"

class WorkingEdge : public BaseEdge {
public:
    explicit WorkingEdge(const std::shared_ptr<TwoSidedInitialEdge> &twoSidedInitialEdge);

    WorkingEdge(WorkingEdge &workingEdgeToCopy, SegmentIdType labelA, SegmentIdType labelB);

    WorkingEdge(
        const std::shared_ptr<TwoSidedInitialEdge> &twoSidedInitialEdge,
        SegmentIdType labelA,
        SegmentIdType labelB);

    VoxelLists getVoxelLists() const override;

    void print(int indentationLevel, std::ostream &outStream) override;


    std::vector<std::shared_ptr<TwoSidedInitialEdge>> constituentTwoSidedInitialEdges;

    // add a vector of shared ptrs of initial edges through moving
    void addConstituentTwoSidedInitialEdges(
        const std::vector<std::shared_ptr<TwoSidedInitialEdge>> &twoSidedInitialEdgesToAdd);
};


#endif //SEGMENTCOUPLER_WORKINGEDGE_H
