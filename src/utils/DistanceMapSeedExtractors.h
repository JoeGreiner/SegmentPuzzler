#ifndef SEGMENTPUZZLER_DISTANCEMAPSEEDEXTRACTORS_H
#define SEGMENTPUZZLER_DISTANCEMAPSEEDEXTRACTORS_H

#include <itkImage.h>

#include <string>

namespace distance_map_benchmark {

using BinaryVoxelType = unsigned char;
using BinaryImageType = itk::Image<BinaryVoxelType, 3>;
using DistanceVoxelType = float;
using DistanceImageType = itk::Image<DistanceVoxelType, 3>;
using SeedLabelType = unsigned int;
using SeedImageType = itk::Image<SeedLabelType, 3>;

enum class SeedExtractorKind {
    LocalMaxima,
    HConvex,
    All
};

SeedExtractorKind parseSeedExtractor(const std::string &value);
std::string seedExtractorName(SeedExtractorKind kind);

SeedImageType::Pointer extractHConvexSeedsFromDistanceImage(DistanceImageType::Pointer distanceMap,
                                                            double minimalHeight = 1.0);
SeedImageType::Pointer extractLocalMaximaSeedsFromDistanceImage(DistanceImageType::Pointer distanceMap);
SeedImageType::Pointer extractSeedsFromDistanceImage(DistanceImageType::Pointer distanceMap,
                                                     SeedExtractorKind extractorKind,
                                                     double minimalHeight = 1.0);

} // namespace distance_map_benchmark

#endif
