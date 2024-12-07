
#include "workingEdge.h"
#include "graph.h"


WorkingEdge::WorkingEdge(std::shared_ptr<InitialEdge> &initialEdgeToCopy) :
        BaseEdge(initialEdgeToCopy->getLabelSmaller(), initialEdgeToCopy->getLabelBigger()) {
    roi = initialEdgeToCopy->getRoi();
    shouldMerge = initialEdgeToCopy->getShouldMerge();
    subInitialEdges.push_back(initialEdgeToCopy);
}

WorkingEdge::WorkingEdge(std::shared_ptr<InitialEdge> &initialEdgeToCopy, SegmentIdType labelA, SegmentIdType labelB) :
        BaseEdge(labelA, labelB) {
//    std::cout << "Creating edge: " << labelA << " " << labelB << " \n";
    roi = initialEdgeToCopy->getRoi();
    subInitialEdges.push_back(initialEdgeToCopy);
    shouldMerge = 0;
}

WorkingEdge::WorkingEdge(WorkingEdge &workingEdgeToCopy, SegmentIdType labelA, SegmentIdType labelB) :
        BaseEdge(labelA, labelB) {
    roi = workingEdgeToCopy.getRoi();
    for (auto &initialEdge : workingEdgeToCopy.subInitialEdges) {
        subInitialEdges.push_back(initialEdge);
    }
    shouldMerge = 0;
}


void WorkingEdge::addSubInitialEdges(std::vector<std::shared_ptr<InitialEdge>> subInitialEdgesToAdd) {
    for (auto &pEdge : subInitialEdgesToAdd) {
        subInitialEdges.push_back(pEdge);
    }
}


std::vector<std::vector<Voxel> const *> WorkingEdge::getVoxelPointerArray() {
    std::vector<std::vector<Voxel> const *> voxelList;
    for (auto &initialEdge : subInitialEdges) {
        voxelList.push_back(initialEdge->getVoxelPointer());
    }
    return voxelList;
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
    outStream << indentationString << "Sub-InitialEdges:";
    for (auto &edge : subInitialEdges) {
        outStream << " " << edge->numId;
    }
    outStream << "\n";
}

