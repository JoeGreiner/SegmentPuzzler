

#include "initialEdge.h"

std::vector<std::vector<Voxel> const *> InitialEdge::getVoxelPointerArray() {
    return std::vector<std::vector<Voxel> const *>{&voxels};
}

InitialEdge::InitialEdge(InitialEdge const &copyEdge) : BaseEdge(copyEdge.labelSmaller, copyEdge.labelBigger) {
    roi = copyEdge.roi;
    voxels = copyEdge.voxels;
    wasUsedToComputeTwoSidedEdge = false;
    for (auto &feature : FeatureList::edgeFeaturesList) {
        addEdgeFeature(feature);
    }
}

InitialEdge::InitialEdge(BaseEdge::SegmentIdType labelAIn, BaseEdge::SegmentIdType labelBIn) : BaseEdge(labelAIn,
                                                                                                        labelBIn) {
    for (auto &feature : FeatureList::edgeFeaturesList) {
        addEdgeFeature(feature);
    }
    wasUsedToComputeTwoSidedEdge = false;
}

bool InitialEdge::getWasUsedToComputeTwoSidedEdge() {
    return wasUsedToComputeTwoSidedEdge;
}

void InitialEdge::setWasUsedToComputeTwoSidedEdge(bool wasUsedToComputeTwoSidedEdgeIn) {
    wasUsedToComputeTwoSidedEdge = wasUsedToComputeTwoSidedEdgeIn;
}


void InitialEdge::mergeVoxelsAndROIwithOtherEdge(InitialEdge *edgeToMerge) {
    roi.mergeRoiWith(edgeToMerge->roi);
    voxels.insert(voxels.end(), edgeToMerge->voxels.begin(), edgeToMerge->voxels.end());
}


void InitialEdge::addVoxel(Voxel &voxel) {
    roi.updateBoundingRoi(voxel);
    voxels.push_back(voxel);
}


void InitialEdge::calculateEdgeFeatures() {
    for (auto &feature : edgeFeatures) {
        feature->compute(voxels, labelSmaller, labelBigger);
    }
}


void InitialEdge::addEdgeFeature(std::unique_ptr<Feature> &feature) {
    edgeFeatures.emplace_back(feature->createNew());
}

size_t InitialEdge::getNumberVoxels() {
    return voxels.size();
}


void InitialEdge::print(int indentationLevel, std::ostream &outStream) {
    std::string indentationString;
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outStream << indentationString << "\tShouldMerge: " << shouldMerge << "\n";
    outStream << indentationString << "\tnumId: " << numId << "\n";
    outStream << indentationString << "\tpairId: " << pairId.first << "," << pairId.second << "\n";
    outStream << indentationString << "\tNumber EdgeVoxels: " << getNumberVoxels() << "\n";
    outStream << indentationString << "\tfx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << "\n";
    outStream << indentationString << "\ttx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << "\n";
    outStream << indentationString << "Edge Features:\n";
    for (auto &feature : edgeFeatures) {
        outStream << indentationString << "\t" << feature->filterName << " " << feature->signalName << "\n";
        outStream << indentationString << "\t\t";
        for (auto &val : feature->values) {
            outStream << val << " ";
        }
        outStream << "\n";
    }
}

std::vector<Voxel> const *InitialEdge::getVoxelPointer() {
    return &voxels;
}

