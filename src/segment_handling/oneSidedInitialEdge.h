#ifndef SEGMENTCOUPLER_ONESIDEDINITIALEDGE_H
#define SEGMENTCOUPLER_ONESIDEDINITIALEDGE_H

#include "src/segment_handling/baseEdge.h"

class OneSidedInitialEdge final : public BaseEdge {
public:
    OneSidedInitialEdge(SegmentIdType labelA, SegmentIdType labelB);

    OneSidedInitialEdge(const OneSidedInitialEdge &) = delete;
    OneSidedInitialEdge &operator=(const OneSidedInitialEdge &) = delete;

    VoxelLists getVoxelLists() const override;
    const VoxelList *getVoxelList() const;
    std::size_t getVoxelCount() const;

    void addVoxel(const Voxel &voxel);
    void print(int indentationLevel, std::ostream &outStream) override;

    VoxelList voxels;
};

#endif // SEGMENTCOUPLER_ONESIDEDINITIALEDGE_H
