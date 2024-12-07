
#include "baseEdge.h"

BaseEdge::BaseEdge(SegmentIdType labelA, SegmentIdType labelB) {
    // initialize empty ROI (with min/max values, sensible ROI is obtained after first voxel is added!)
    roi = Roi();
    labelSmaller = (labelA > labelB) ? labelB : labelA;
    labelBigger = (labelA > labelB) ? labelA : labelB;
    pairId = EdgePairIdType{labelSmaller, labelBigger};
    shouldMerge = 0;
    numId = 0;
}

BaseEdge::~BaseEdge() {

}

void
BaseEdge::setIdAndRegister(EdgeNumIdType numIdToSet, std::unordered_map<EdgeNumIdType, EdgePairIdType> &mapToRegister) {
    numId = numIdToSet;
    mapToRegister[numId] = pairId;
}

const Roi &BaseEdge::getRoi() const {
    return roi;
}

BaseEdge::SegmentIdType BaseEdge::getLabelSmaller() const {
    return labelSmaller;
}

BaseEdge::SegmentIdType BaseEdge::getLabelBigger() const {
    return labelBigger;
}

int BaseEdge::getShouldMerge() const {
    return shouldMerge;
}

void BaseEdge::setLabelSmaller(BaseEdge::SegmentIdType labelSmaller) {
    BaseEdge::labelSmaller = labelSmaller;
}

void BaseEdge::setLabelBigger(BaseEdge::SegmentIdType labelBigger) {
    BaseEdge::labelBigger = labelBigger;
}

void BaseEdge::setShouldMergeYes() {
    shouldMerge = 2;
}

void BaseEdge::setShouldMergeNo() {
    shouldMerge = -2;
}

void BaseEdge::setShouldMergeNotInformed() {
    shouldMerge = 0;
}
