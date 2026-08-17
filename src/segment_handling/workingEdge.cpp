
#include "workingEdge.h"
#include "Graph.h"


WorkingEdge::WorkingEdge(const std::shared_ptr<TwoSidedInitialEdge> &twoSidedInitialEdge) :
        BaseEdge(twoSidedInitialEdge->getLabelSmaller(), twoSidedInitialEdge->getLabelBigger()) {
    roi = twoSidedInitialEdge->getRoi();
    shouldMerge = twoSidedInitialEdge->getShouldMerge();
    constituentTwoSidedInitialEdges.push_back(twoSidedInitialEdge);
}

WorkingEdge::WorkingEdge(const std::shared_ptr<TwoSidedInitialEdge> &twoSidedInitialEdge,
                         SegmentIdType labelA,
                         SegmentIdType labelB) :
        BaseEdge(labelA, labelB) {
//    std::cout << "Creating edge: " << labelA << " " << labelB << " \n";
    roi = twoSidedInitialEdge->getRoi();
    constituentTwoSidedInitialEdges.push_back(twoSidedInitialEdge);
    shouldMerge = 0;
}

WorkingEdge::WorkingEdge(WorkingEdge &workingEdgeToCopy, SegmentIdType labelA, SegmentIdType labelB) :
        BaseEdge(labelA, labelB) {
    roi = workingEdgeToCopy.getRoi();
    for (auto &initialEdge : workingEdgeToCopy.constituentTwoSidedInitialEdges) {
        constituentTwoSidedInitialEdges.push_back(initialEdge);
    }
    shouldMerge = 0;
}


void WorkingEdge::addConstituentTwoSidedInitialEdges(
        const std::vector<std::shared_ptr<TwoSidedInitialEdge>> &twoSidedInitialEdgesToAdd) {
    constituentTwoSidedInitialEdges.insert(
        constituentTwoSidedInitialEdges.end(),
        twoSidedInitialEdgesToAdd.begin(),
        twoSidedInitialEdgesToAdd.end());
}


VoxelLists WorkingEdge::getVoxelLists() const {
    VoxelLists voxelLists;
    voxelLists.reserve(constituentTwoSidedInitialEdges.size() * 2);
    for (const auto &initialEdge : constituentTwoSidedInitialEdges) {
        const VoxelLists initialEdgeVoxelLists = initialEdge->getVoxelLists();
        voxelLists.insert(voxelLists.end(), initialEdgeVoxelLists.begin(), initialEdgeVoxelLists.end());
    }
    return voxelLists;
}

void WorkingEdge::print(int indentationLevel, std::ostream &outStream) {
    std::string indentationString;
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outStream << indentationString << "\tShouldMerge: " << shouldMerge << "\n";
    outStream << indentationString << "\tnumId: " << numId << "\n";
    outStream << indentationString << "\tpairId: " << pairId.first << "," << pairId.second << "\n";
    outStream << indentationString << "\tfx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << "\n";
    outStream << indentationString << "\ttx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << "\n";
    outStream << indentationString << "Constituent Two-Sided Initial Edges:";
    for (auto &edge : constituentTwoSidedInitialEdges) {
        outStream << " " << edge->numId;
    }
    outStream << "\n";
}
