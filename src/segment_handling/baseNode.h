
#ifndef SEGMENTCOUPLER_BASENODE_H
#define SEGMENTCOUPLER_BASENODE_H

#include "src/utils/voxel.h"
#include "src/utils/roi.h"
#include <vector>
#include <itkImage.h>
#include "src/file_definitions/dataTypes.h"
#include "feature.h"

class BaseNode : public FeatureList {
public:
    using SegmentIdType = dataType::SegmentIdType;
    static constexpr int Dimension = dataType::Dimension;
    using SegmentIdImageType = dataType::SegmentsImageType;

    using EdgePairIdType = dataType::EdgePairIdType;
    using EdgeNumIdType = dataType::EdgeNumIdType;

    // print node properties
    virtual void print(int indentationLevel, std::ostream &outStream) = 0;

    virtual std::vector<std::vector<Voxel> *> getVoxelPointerArray() = 0;

    virtual std::vector<Voxel> getVoxelArray() = 0;

    // get a vector of the ids of the connected nodes. useful to iterate over elements if normal iterators get invalided
    virtual std::vector<SegmentIdType> getVectorOfConnectedNodeIds() = 0;

    virtual ~BaseNode();


    SegmentIdType getLabel() const;

    // Region Of Interest of the node
    Roi roi;

protected:

    // Label of the node
    SegmentIdType label;


private:
    bool verbose;

};


#endif //SEGMENTCOUPLER_BASENODE_H
