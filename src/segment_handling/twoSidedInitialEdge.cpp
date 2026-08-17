#include "twoSidedInitialEdge.h"

#include <stdexcept>
#include <utility>

TwoSidedInitialEdge::TwoSidedInitialEdge(
        std::shared_ptr<OneSidedInitialEdge> smallerLabelOneSidedInitialEdgeIn,
        std::shared_ptr<OneSidedInitialEdge> largerLabelOneSidedInitialEdgeIn)
    : BaseEdge(
          smallerLabelOneSidedInitialEdgeIn != nullptr
              ? smallerLabelOneSidedInitialEdgeIn->getLabelSmaller()
              : 0,
          smallerLabelOneSidedInitialEdgeIn != nullptr
              ? smallerLabelOneSidedInitialEdgeIn->getLabelBigger()
              : 0),
      smallerLabelOneSidedInitialEdge(std::move(smallerLabelOneSidedInitialEdgeIn)),
      largerLabelOneSidedInitialEdge(std::move(largerLabelOneSidedInitialEdgeIn)) {
    if (smallerLabelOneSidedInitialEdge == nullptr || largerLabelOneSidedInitialEdge == nullptr) {
        throw std::invalid_argument("A two-sided initial edge requires both one-sided initial edges.");
    }
    if (smallerLabelOneSidedInitialEdge->pairId != largerLabelOneSidedInitialEdge->pairId ||
        pairId != smallerLabelOneSidedInitialEdge->pairId) {
        throw std::invalid_argument("The one-sided initial edges must have the same label pair.");
    }
    if (smallerLabelOneSidedInitialEdge == largerLabelOneSidedInitialEdge) {
        throw std::invalid_argument("A two-sided initial edge requires two distinct one-sided initial edges.");
    }

    roi = smallerLabelOneSidedInitialEdge->getRoi();
    roi.mergeRoiWith(largerLabelOneSidedInitialEdge->getRoi());
    for (auto &feature : FeatureList::edgeFeaturesList) {
        edgeFeatures.emplace_back(feature->createNew());
    }
}

const std::shared_ptr<const OneSidedInitialEdge> &
TwoSidedInitialEdge::getSmallerLabelOneSidedInitialEdge() const {
    return smallerLabelOneSidedInitialEdge;
}

const std::shared_ptr<const OneSidedInitialEdge> &
TwoSidedInitialEdge::getLargerLabelOneSidedInitialEdge() const {
    return largerLabelOneSidedInitialEdge;
}

VoxelLists TwoSidedInitialEdge::getVoxelLists() const {
    return {
        smallerLabelOneSidedInitialEdge->getVoxelList(),
        largerLabelOneSidedInitialEdge->getVoxelList()
    };
}

std::size_t TwoSidedInitialEdge::getVoxelCount() const {
    return smallerLabelOneSidedInitialEdge->getVoxelCount() +
           largerLabelOneSidedInitialEdge->getVoxelCount();
}

void TwoSidedInitialEdge::calculateEdgeFeatures() {
    const VoxelLists voxelLists = getVoxelLists();
    for (auto &feature : edgeFeatures) {
        feature->compute(voxelLists, labelSmaller, labelBigger);
    }
}

void TwoSidedInitialEdge::print(int indentationLevel, std::ostream &outStream) {
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
    outStream << indentationString << "Edge Features:\n";
    for (auto &feature : edgeFeatures) {
        outStream << indentationString << "\t" << feature->filterName << " " << feature->signalName << "\n";
        outStream << indentationString << "\t\t";
        for (auto &value : feature->values) {
            outStream << value << " ";
        }
        outStream << "\n";
    }
}
