#ifndef SEGMENTCOUPLER_DATATYPES_H
#define SEGMENTCOUPLER_DATATYPES_H

#include "itkImage.h"

//#define SEGMENTSHORT
#define SEGMENTUINT


namespace dataType {
    const int Dimension = 3;

#ifdef SEGMENTSHORT
    using SegmentIdType = short;
#endif

#ifdef SEGMENTUINT
    using SegmentIdType = unsigned int;
#endif

    using SegmentsImageType = itk::Image<SegmentIdType, Dimension>;
    using EdgePairIdType = std::pair<SegmentIdType, SegmentIdType>;
    using EdgeNumIdType = unsigned int;
    using LandscapeType = itk::Image<unsigned char, 3>;
    using DistanceType = itk::Image<short, 3>;
    using BoundaryVoxelType = unsigned short;
    using BoundaryImageType = itk::Image<BoundaryVoxelType, 3>;

    using MappedEdgeIdType = unsigned int;
    using EdgeImageType = itk::Image<MappedEdgeIdType, 3>;
    using FeatureVoxelType = float;
    using FeatureImageType = itk::Image<FeatureVoxelType, 3>;

}


#endif //SEGMENTCOUPLER_DATATYPES_H
