#ifndef SEGMENTCOUPLER_ITKWATERSHEDHELPERS_H
#define SEGMENTCOUPLER_ITKWATERSHEDHELPERS_H

#include "itkImage.h"
#include "src/file_definitions/dataTypes.h"
#include "src/utils/DistanceMapSeedExtractors.h"
#include "src/utils/FastMarkerWatershed3D.h"

#include <unordered_map>
#include <vector>

enum class DistanceMapAlgorithm {
    Maurer,
    FH
};

enum class WatershedAlgorithm {
    MorphologicalWatershedFromMarkers,
    FastMarkerWatershed
};

struct WatershedRunOptions {
    WatershedAlgorithm algorithm = WatershedAlgorithm::MorphologicalWatershedFromMarkers;
    bool showWatershedLines = false;
    bool fullyConnected = false;
};

struct BoundaryConsistentPartitionResult {
    using SplitComponentMap = std::unordered_map<dataType::SegmentIdType, std::vector<dataType::SegmentIdType>>;

    dataType::SegmentsImageType::Pointer canonicalLabels;
    dataType::SegmentsImageType::Pointer displayLabels;
    SplitComponentMap splitComponentIds;
};

void binaryThresholdImageFilterFloat(itk::Image<unsigned short, 3>::Pointer &inputImage,
                                     itk::Image<unsigned char, 3>::Pointer &outputImage,
                                     float thresholdValueMin);

unsigned int getMaximumOfUIntImage(itk::Image<unsigned int, 3>::Pointer &UIntImage);

void setBoundariesToValue(itk::Image<unsigned char, 3>::Pointer &CharImage, unsigned char value);

void generateDistanceMap(itk::Image<unsigned char, 3>::Pointer &edgeImage,
                         itk::Image<float, 3>::Pointer &distanceMap,
                         double varianceGaussianFilter,
                         DistanceMapAlgorithm algorithm = DistanceMapAlgorithm::Maurer,
                         int threadCount = 1);

float getMaximumOfFloatImage(itk::Image<float, 3>::Pointer &floatImage);

void invertDistanceMap(itk::Image<float, 3>::Pointer &distanceMap,
                       itk::Image<float, 3>::Pointer &invertedDistanceMap);

void runWatershed(itk::Image<float, 3>::Pointer &invertedDistanceMap,
                  itk::Image<unsigned int, 3>::Pointer &seeds,
                  itk::Image<unsigned int, 3>::Pointer &watershedOut,
                  const WatershedRunOptions &options = WatershedRunOptions(),
                  segment_puzzler::FastMarkerWatershedMetrics *fastMetrics = nullptr);

void insertBoundariesIntoWatershed(itk::Image<unsigned int, 3>::Pointer &watershed,
                                   itk::Image<unsigned char, 3>::Pointer &thresholdedBoundaries);

BoundaryConsistentPartitionResult deriveBoundaryConsistentPartition(
    dataType::SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    const WatershedRunOptions &repairOptions = WatershedRunOptions(),
    bool repairCanonicalLabels = true,
    DistanceMapAlgorithm distanceMapAlgorithm = DistanceMapAlgorithm::Maurer,
    int threadCount = 1);

void binaryThresholdImageFilterFloat(itk::Image<float, 3>::Pointer &inputImage,
                                     itk::Image<float, 3>::Pointer &outputImage,
                                     float thresholdValueMin);

void castFloatToChar(itk::Image<float, 3>::Pointer &inputImage,
                     itk::Image<unsigned char, 3>::Pointer &outputType);

void connectedComponentCharToUInt(itk::Image<unsigned char, 3>::Pointer &inputImage,
                                  itk::Image<unsigned int, 3>::Pointer &outputImage);

void extractMinimaFromDistanceMap(itk::Image<float, 3>::Pointer &distanceMap,
                                  itk::Image<unsigned int, 3>::Pointer &seeds,
                                  double minimalHeight = 1,
                                  distance_map_benchmark::SeedExtractorKind seedExtractorKind =
                                      distance_map_benchmark::SeedExtractorKind::HConvex);

void filterSmallSegmentSeeds(itk::Image<unsigned int, 3>::Pointer &watershedIn,
                             itk::Image<unsigned int, 3>::Pointer &watershedOut,
                             float volumeThreshold = 2000);

#endif //SEGMENTCOUPLER_ITKWATERSHEDHELPERS_H
