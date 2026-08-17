#include "oneSidedInitialEdge.h"

OneSidedInitialEdge::OneSidedInitialEdge(SegmentIdType labelA, SegmentIdType labelB)
    : BaseEdge(labelA, labelB) {
}

VoxelLists OneSidedInitialEdge::getVoxelLists() const {
    return {&voxels};
}

const VoxelList *OneSidedInitialEdge::getVoxelList() const {
    return &voxels;
}

std::size_t OneSidedInitialEdge::getVoxelCount() const {
    return voxels.size();
}

void OneSidedInitialEdge::addVoxel(const Voxel &voxel) {
    roi.updateBoundingRoi(voxel);
    voxels.push_back(voxel);
}

void OneSidedInitialEdge::print(int indentationLevel, std::ostream &outStream) {
    std::string indentationString;
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outStream << indentationString << "\tShouldMerge: " << shouldMerge << "\n";
    outStream << indentationString << "\tnumId: " << numId << "\n";
    outStream << indentationString << "\tpairId: " << pairId.first << "," << pairId.second << "\n";
    outStream << indentationString << "\tNumber EdgeVoxels: " << getVoxelCount() << "\n";
    outStream << indentationString << "\tfx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << "\n";
    outStream << indentationString << "\ttx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << "\n";
}
