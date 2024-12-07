
#ifndef SEGMENTCOUPLER_BASEEDGE_H
#define SEGMENTCOUPLER_BASEEDGE_H

#include "src/utils/voxel.h"
#include "src/utils/roi.h"
#include <vector>
#include <unordered_map>
#include <src/file_definitions/dataTypes.h>

class BaseEdge {
public:
    using SegmentIdType = dataType::SegmentIdType;
    using EdgePairIdType = dataType::EdgePairIdType;
    using EdgeNumIdType = dataType::EdgeNumIdType;

    virtual std::vector<std::vector<Voxel> const *> getVoxelPointerArray() = 0;

    virtual void print(int indentationLevel, std::ostream &outStream) = 0;

    BaseEdge(SegmentIdType labelA, SegmentIdType labelB);

    virtual ~BaseEdge();

    void setIdAndRegister(EdgeNumIdType numIdToSet, std::unordered_map<EdgeNumIdType, EdgePairIdType> &mapToRegister);

    SegmentIdType getLabelSmaller() const;

    SegmentIdType getLabelBigger() const;

    void setLabelBigger(SegmentIdType labelBigger);

    void setLabelSmaller(SegmentIdType labelSmaller);

    const Roi &getRoi() const;

    int getShouldMerge() const;
    void setShouldMergeYes();
    void setShouldMergeNo();
    void setShouldMergeNotInformed();

    // pair <labelSmaller, labelBigger>
    EdgePairIdType pairId;
    // unique id (numeric number)
    EdgeNumIdType numId; // 0 = not initialized

protected:
    // ROI/bounding box of roi
    Roi roi;

    // labelA/labelB of the edge ordered
    SegmentIdType labelSmaller, labelBigger;


protected:

    // 0 = dont care (ecm, unset, ...) -1 == do not merge, 1 == do merge
    // -2 do not merge (determined by algorithm)
    // 2 do merge (determined by algorithm)
    int shouldMerge;
};


#endif //SEGMENTCOUPLER_BASEEDGE_H
