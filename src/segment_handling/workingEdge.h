
#ifndef SEGMENTCOUPLER_WORKINGEDGE_H
#define SEGMENTCOUPLER_WORKINGEDGE_H


#include "src/segment_handling/baseEdge.h"
#include "graphBase.h"
#include "initialEdge.h"

class WorkingEdge : public BaseEdge {
public:
    WorkingEdge(std::shared_ptr<InitialEdge> &initialEdgeToCopy);

    WorkingEdge(WorkingEdge &workingEdgeToCopy, SegmentIdType labelA, SegmentIdType labelB);

    WorkingEdge(std::shared_ptr<InitialEdge> &initialEdgeToCopy, SegmentIdType labelA, SegmentIdType labelB);

    std::vector<std::vector<Voxel> const *> getVoxelPointerArray() override;

    void print(int indentationLevel, std::ostream &outStream) override;


    std::vector<std::shared_ptr<InitialEdge>> subInitialEdges;

    // add a vector of shared ptrs of initial edges through moving
    void addSubInitialEdges(std::vector<std::shared_ptr<InitialEdge>> subInitialEdgesToAdd);
};


#endif //SEGMENTCOUPLER_WORKINGEDGE_H
