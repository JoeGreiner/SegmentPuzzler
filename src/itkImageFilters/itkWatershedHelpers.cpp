#include "itkWatershedHelpers.h"

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "itkBinaryThresholdImageFilter.h"
#include <itkCastImageFilter.h>
#include <itkChangeLabelImageFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkDiscreteGaussianImageFilter.h>
#include <itkImageRegionIterator.h>
#include <itkInvertIntensityImageFilter.h>
#include <itkLabelGeometryImageFilter.h>
#include <itkMorphologicalWatershedFromMarkersImageFilter.h>
#include <itkSignedMaurerDistanceMapImageFilter.h>
#include <itkStatisticsImageFilter.h>

#include "src/utils/DistanceMapFH3D.h"
#include "src/utils/FastMarkerWatershed3D.h"

namespace {

using SegmentsImageType = dataType::SegmentsImageType;
using SegmentIdType = dataType::SegmentIdType;

SegmentsImageType::Pointer cloneSegmentsImageMetadata(SegmentsImageType::Pointer source) {
    auto image = SegmentsImageType::New();
    image->SetRegions(source->GetLargestPossibleRegion());
    image->SetSpacing(source->GetSpacing());
    image->SetOrigin(source->GetOrigin());
    image->SetDirection(source->GetDirection());
    image->Allocate();
    return image;
}

SegmentsImageType::Pointer copySegmentsImage(SegmentsImageType::Pointer source) {
    auto image = cloneSegmentsImageMetadata(source);
    std::copy(source->GetBufferPointer(),
              source->GetBufferPointer() + source->GetLargestPossibleRegion().GetNumberOfPixels(),
              image->GetBufferPointer());
    return image;
}

void relabelInjectedBoundaryComponents(
    SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    SegmentsImageType::Pointer &displayLabels,
    BoundaryConsistentPartitionResult::SplitComponentMap &splitComponentIds) {
    displayLabels = copySegmentsImage(labels);
    splitComponentIds.clear();

    if (thresholdedBoundaries.IsNull()) {
        return;
    }

    const auto size = labels->GetLargestPossibleRegion().GetSize();
    const std::ptrdiff_t dimX = static_cast<std::ptrdiff_t>(size[0]);
    const std::ptrdiff_t dimY = static_cast<std::ptrdiff_t>(size[1]);
    const std::ptrdiff_t dimZ = static_cast<std::ptrdiff_t>(size[2]);
    const std::ptrdiff_t planeXY = dimX * dimY;
    const std::ptrdiff_t total = dimX * dimY * dimZ;

    SegmentIdType *displayBuffer = displayLabels->GetBufferPointer();
    const unsigned char *thresholdBuffer = thresholdedBoundaries->GetBufferPointer();
    for (std::ptrdiff_t index = 0; index < total; ++index) {
        if (thresholdBuffer[index] != 0) {
            displayBuffer[index] = 0;
        }
    }

    auto componentImage = cloneSegmentsImageMetadata(labels);
    componentImage->FillBuffer(0);
    SegmentIdType *componentBuffer = componentImage->GetBufferPointer();

    std::vector<unsigned char> visited(static_cast<std::size_t>(total), 0);
    std::vector<std::ptrdiff_t> queue;
    queue.reserve(1024);

    std::vector<SegmentIdType> componentOriginalLabels(1, 0);
    std::unordered_map<SegmentIdType, std::vector<SegmentIdType>> componentsByOriginalLabel;
    SegmentIdType nextComponentId = 1;

    const std::array<std::ptrdiff_t, 6> offsetX{{1, -1, 0, 0, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetY{{0, 0, 1, -1, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetZ{{0, 0, 0, 0, 1, -1}};

    for (std::ptrdiff_t seed = 0; seed < total; ++seed) {
        if (visited[seed] != 0 || displayBuffer[seed] == 0) {
            visited[seed] = 1;
            continue;
        }

        const SegmentIdType originalLabel = displayBuffer[seed];
        queue.clear();
        queue.push_back(seed);
        visited[seed] = 1;
        componentBuffer[seed] = nextComponentId;

        for (std::size_t queueIndex = 0; queueIndex < queue.size(); ++queueIndex) {
            const std::ptrdiff_t current = queue[queueIndex];
            const std::ptrdiff_t z = current / planeXY;
            const std::ptrdiff_t remainder = current % planeXY;
            const std::ptrdiff_t y = remainder / dimX;
            const std::ptrdiff_t x = remainder % dimX;

            for (size_t direction = 0; direction < offsetX.size(); ++direction) {
                const std::ptrdiff_t nx = x + offsetX[direction];
                const std::ptrdiff_t ny = y + offsetY[direction];
                const std::ptrdiff_t nz = z + offsetZ[direction];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ) {
                    continue;
                }

                const std::ptrdiff_t neighbor = nx + ny * dimX + nz * planeXY;
                if (visited[neighbor] != 0 || displayBuffer[neighbor] != originalLabel) {
                    continue;
                }

                visited[neighbor] = 1;
                componentBuffer[neighbor] = nextComponentId;
                queue.push_back(neighbor);
            }
        }

        componentOriginalLabels.push_back(originalLabel);
        componentsByOriginalLabel[originalLabel].push_back(nextComponentId);
        ++nextComponentId;
    }

    SegmentIdType nextFreshLabel = getMaximumOfUIntImage(labels) + 1;
    std::vector<SegmentIdType> componentToFinalLabel(nextComponentId, 0);
    for (auto &entry : componentsByOriginalLabel) {
        const SegmentIdType originalLabel = entry.first;
        auto &componentIds = entry.second;
        if (componentIds.size() == 1) {
            componentToFinalLabel[componentIds.front()] = originalLabel;
            continue;
        }

        auto &newLabels = splitComponentIds[originalLabel];
        newLabels.reserve(componentIds.size());
        for (SegmentIdType componentId : componentIds) {
            const SegmentIdType newLabel = nextFreshLabel++;
            componentToFinalLabel[componentId] = newLabel;
            newLabels.push_back(newLabel);
        }
    }

    for (std::ptrdiff_t index = 0; index < total; ++index) {
        const SegmentIdType componentId = componentBuffer[index];
        displayBuffer[index] = componentId == 0 ? 0 : componentToFinalLabel[componentId];
    }
}

SegmentsImageType::Pointer repairSplitLabelsWithWatershed(
    SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    SegmentsImageType::Pointer displayLabels,
    const BoundaryConsistentPartitionResult::SplitComponentMap &splitComponentIds,
    const WatershedRunOptions &repairOptions,
    DistanceMapAlgorithm distanceMapAlgorithm,
    int threadCount) {
    auto canonicalLabels = copySegmentsImage(labels);
    if (thresholdedBoundaries.IsNull() || splitComponentIds.empty()) {
        return canonicalLabels;
    }

    auto seeds = cloneSegmentsImageMetadata(labels);
    SegmentIdType *seedBuffer = seeds->GetBufferPointer();
    const SegmentIdType *labelBuffer = labels->GetBufferPointer();
    const SegmentIdType *displayBuffer = displayLabels->GetBufferPointer();
    const size_t voxelCount = labels->GetLargestPossibleRegion().GetNumberOfPixels();

    std::unordered_set<SegmentIdType> splitLabelSet;
    splitLabelSet.reserve(splitComponentIds.size());
    for (const auto &entry : splitComponentIds) {
        splitLabelSet.insert(entry.first);
    }

    for (size_t index = 0; index < voxelCount; ++index) {
        const SegmentIdType originalLabel = labelBuffer[index];
        if (originalLabel == 0) {
            seedBuffer[index] = 0;
            continue;
        }
        if (splitLabelSet.count(originalLabel) == 0) {
            seedBuffer[index] = originalLabel;
            continue;
        }
        seedBuffer[index] = displayBuffer[index];
    }

    itk::Image<float, 3>::Pointer distanceMap;
    auto thresholdCopy = thresholdedBoundaries;
    generateDistanceMap(thresholdCopy, distanceMap, 0, distanceMapAlgorithm, threadCount);

    itk::Image<float, 3>::Pointer invertedDistanceMap;
    invertDistanceMap(distanceMap, invertedDistanceMap);

    WatershedRunOptions options = repairOptions;
    options.showWatershedLines = false;
    runWatershed(invertedDistanceMap, seeds, canonicalLabels, options);
    return canonicalLabels;
}

} // namespace

void binaryThresholdImageFilterFloat(itk::Image<unsigned short, 3>::Pointer &inputImage,
                                     itk::Image<unsigned char, 3>::Pointer &outputImage,
                                     float thresholdValueMin) {
    using BinaryThresholdImageFilterType =
        itk::BinaryThresholdImageFilter<itk::Image<unsigned short, 3>, itk::Image<unsigned char, 3>>;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
}

unsigned int getMaximumOfUIntImage(itk::Image<unsigned int, 3>::Pointer &UIntImage) {
    using StatisticsUIntImageFilterType = itk::StatisticsImageFilter<itk::Image<unsigned int, 3>>;
    StatisticsUIntImageFilterType::Pointer statisticsUIntImageFilter = StatisticsUIntImageFilterType::New();
    statisticsUIntImageFilter->SetInput(UIntImage);
    statisticsUIntImageFilter->Update();
    return statisticsUIntImageFilter->GetMaximum();
}

void setBoundariesToValue(itk::Image<unsigned char, 3>::Pointer &CharImage, unsigned char value) {
    using CharIteratorType = itk::ImageRegionIterator<itk::Image<unsigned char, 3>>;

    std::vector<unsigned int> indexX(6, 0), indexY(6, 0), indexZ(6, 0);
    std::vector<unsigned int> sizeX(6, 0), sizeY(6, 0), sizeZ(6, 0);

    itk::Image<unsigned char, 3>::RegionType LargestRegion = CharImage->GetLargestPossibleRegion();
    itk::Image<unsigned char, 3>::SizeType ImageDimensions = LargestRegion.GetSize();
    std::vector<unsigned int> dim(3, 0);
    dim[0] = static_cast<unsigned int>(ImageDimensions[0]);
    dim[1] = static_cast<unsigned int>(ImageDimensions[1]);
    dim[2] = static_cast<unsigned int>(ImageDimensions[2]);

    indexX = {0, 0, 0, dim[0] - 1, 0, 0};
    indexY = {0, 0, 0, 0, 0, dim[1] - 1};
    indexZ = {0, dim[2] - 1, 0, 0, 0, 0};
    sizeX = {dim[0], dim[0], 1, 1, dim[0], dim[0]};
    sizeY = {dim[1], dim[1], dim[1], dim[1], 1, 1};
    sizeZ = {1, 1, dim[2], dim[2], dim[2], dim[2]};

    for (unsigned int i = 0; i < indexX.size(); ++i) {
        itk::Image<unsigned char, 3>::SizeType size_ROI;
        size_ROI[0] = sizeX.at(i);
        size_ROI[1] = sizeY.at(i);
        size_ROI[2] = sizeZ.at(i);

        itk::Image<unsigned char, 3>::IndexType index;
        index[0] = indexX.at(i);
        index[1] = indexY.at(i);
        index[2] = indexZ.at(i);

        itk::Image<unsigned char, 3>::RegionType imageBoundaryROI;
        imageBoundaryROI.SetSize(size_ROI);
        imageBoundaryROI.SetIndex(index);

        CharIteratorType it(CharImage, imageBoundaryROI);
        for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
            it.Set(value);
        }
    }
}

void generateDistanceMap(itk::Image<unsigned char, 3>::Pointer &edgeImage,
                         itk::Image<float, 3>::Pointer &distanceMap,
                         double varianceGaussianFilter,
                         DistanceMapAlgorithm algorithm,
                         int threadCount) {
    std::cout << "Generate Distance Map" << std::endl;

    if (algorithm == DistanceMapAlgorithm::FH) {
        const auto region = edgeImage->GetLargestPossibleRegion();
        const auto size = region.GetSize();
        const std::array<int, 3> dims = {
            static_cast<int>(size[0]),
            static_cast<int>(size[1]),
            static_cast<int>(size[2])
        };

        std::vector<distance_map_benchmark::BinaryVoxelType> mask;
        mask.reserve(region.GetNumberOfPixels());
        itk::ImageRegionConstIterator<itk::Image<unsigned char, 3>> iterator(edgeImage, region);
        for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
            mask.push_back(iterator.Get());
        }

        const auto fhBackground = distance_map_benchmark::runBoundaryAwareSquaredEdt(
            mask, dims, {1.0, 1.0, 1.0}, threadCount);

        std::vector<distance_map_benchmark::BinaryVoxelType> invertedMask(mask.size());
        for (std::size_t i = 0; i < mask.size(); ++i) {
            invertedMask[i] = (mask[i] == 0) ? 1 : 0;
        }
        const auto fhForeground = distance_map_benchmark::runBoundaryAwareSquaredEdt(
            invertedMask, dims, {1.0, 1.0, 1.0}, threadCount);

        distanceMap = itk::Image<float, 3>::New();
        distanceMap->SetRegions(region);
        distanceMap->SetSpacing(edgeImage->GetSpacing());
        distanceMap->SetOrigin(edgeImage->GetOrigin());
        distanceMap->SetDirection(edgeImage->GetDirection());
        distanceMap->Allocate();

        itk::ImageRegionIterator<itk::Image<float, 3>> outIterator(distanceMap, region);
        std::size_t outputIndex = 0;
        for (outIterator.GoToBegin(); !outIterator.IsAtEnd(); ++outIterator, ++outputIndex) {
            if (mask[outputIndex] == 0) {
                const float sq = fhBackground.distances[outputIndex];
                outIterator.Set(sq <= 0.0f ? 0.0f
                    : static_cast<float>(std::sqrt(static_cast<double>(sq))));
            } else {
                const float sq = fhForeground.distances[outputIndex];
                outIterator.Set(sq <= 0.0f ? 0.0f
                    : -static_cast<float>(std::sqrt(static_cast<double>(sq))));
            }
        }
    } else {
        using SignedMaurerDistanceMapImageFilterType =
            itk::SignedMaurerDistanceMapImageFilter<itk::Image<unsigned char, 3>, itk::Image<float, 3>>;
        SignedMaurerDistanceMapImageFilterType::Pointer distanceFilter = SignedMaurerDistanceMapImageFilterType::New();
        distanceFilter->SetInput(edgeImage);
        distanceFilter->SetBackgroundValue(0);
        distanceFilter->SquaredDistanceOff();
        distanceFilter->UseImageSpacingOff();

        distanceMap = distanceFilter->GetOutput();
        distanceFilter->Update();
    }

    if (varianceGaussianFilter > 0) {
        using GaussianFilterType = itk::DiscreteGaussianImageFilter<itk::Image<float, 3>, itk::Image<float, 3>>;
        GaussianFilterType::Pointer gaussianFilter = GaussianFilterType::New();
        gaussianFilter->SetVariance(varianceGaussianFilter);
        gaussianFilter->SetInput(distanceMap);
        gaussianFilter->SetUseImageSpacingOff();
        distanceMap = gaussianFilter->GetOutput();
        gaussianFilter->Update();
    }
}

float getMaximumOfFloatImage(itk::Image<float, 3>::Pointer &floatImage) {
    using StatisticsFloatImageFilterType = itk::StatisticsImageFilter<itk::Image<float, 3>>;
    StatisticsFloatImageFilterType::Pointer statisticsFloatImageFilter = StatisticsFloatImageFilterType::New();
    statisticsFloatImageFilter->SetInput(floatImage);
    statisticsFloatImageFilter->Update();
    return statisticsFloatImageFilter->GetMaximum();
}

void invertDistanceMap(itk::Image<float, 3>::Pointer &distanceMap,
                       itk::Image<float, 3>::Pointer &invertedDistanceMap) {
    using InvertIntensityImageFilterType =
        itk::InvertIntensityImageFilter<itk::Image<float, 3>, itk::Image<float, 3>>;
    InvertIntensityImageFilterType::Pointer invertFilter = InvertIntensityImageFilterType::New();

    float maxDistance = getMaximumOfFloatImage(distanceMap);
    float DistLowestLevel = std::ceil(maxDistance);

    invertFilter->SetInput(distanceMap);
    invertFilter->SetMaximum(DistLowestLevel + 1);
    invertFilter->Update();
    invertedDistanceMap = invertFilter->GetOutput();

    std::cout << "New Maximum should be: " << invertFilter->GetMaximum() << std::endl;
    std::cout << "Maximum after inverting the distancemap : " << getMaximumOfFloatImage(invertedDistanceMap) << std::endl;
}

void runWatershed(itk::Image<float, 3>::Pointer &invertedDistanceMap,
                  itk::Image<unsigned int, 3>::Pointer &seeds,
                  itk::Image<unsigned int, 3>::Pointer &watershedOut,
                  const WatershedRunOptions &options,
                  segment_puzzler::FastMarkerWatershedMetrics *fastMetrics) {
    std::cout << "Run watershed ...\n";
    switch (options.algorithm) {
        case WatershedAlgorithm::MorphologicalWatershedFromMarkers: {
            using WatershedFilterType =
                itk::MorphologicalWatershedFromMarkersImageFilter<itk::Image<float, 3>, itk::Image<unsigned int, 3>>;
            WatershedFilterType::Pointer watershedFilter = WatershedFilterType::New();
            watershedFilter->SetMarkWatershedLine(options.showWatershedLines);
            watershedFilter->SetFullyConnected(options.fullyConnected);
            watershedFilter->SetInput1(invertedDistanceMap);
            watershedFilter->SetInput2(seeds);
            watershedFilter->Update();
            watershedOut = watershedFilter->GetOutput();
            return;
        }
        case WatershedAlgorithm::FastMarkerWatershed: {
            segment_puzzler::FastMarkerWatershedOptions fastOptions;
            fastOptions.fullyConnected = options.fullyConnected;
            fastOptions.markWatershedLine = options.showWatershedLines;
            watershedOut = segment_puzzler::runFastMarkerWatershed3D(
                invertedDistanceMap, seeds, fastOptions, fastMetrics);
            return;
        }
    }
}

void insertBoundariesIntoWatershed(itk::Image<unsigned int, 3>::Pointer &watershed,
                                   itk::Image<unsigned char, 3>::Pointer &thresholdedBoundaries) {
    itk::ImageRegionIterator<itk::Image<unsigned int, 3>> wsIterator(
        watershed, watershed->GetLargestPossibleRegion());
    itk::ImageRegionIterator<itk::Image<unsigned char, 3>> thresholdIterator(
        thresholdedBoundaries, thresholdedBoundaries->GetLargestPossibleRegion());
    bool insertWSasHighestValue = false;
    unsigned int valueOfBoundaryInWS;
    if (insertWSasHighestValue) {
        valueOfBoundaryInWS = getMaximumOfUIntImage(watershed) + 1;
    } else {
        valueOfBoundaryInWS = 0;
    }
    std::cout << "Inserting boundaries into watershed with value: " << valueOfBoundaryInWS << "\n";
    for (thresholdIterator.GoToBegin(); !thresholdIterator.IsAtEnd(); ++thresholdIterator) {
        if (thresholdIterator.Get() >= 1) {
            wsIterator.Set(valueOfBoundaryInWS);
        }
        ++wsIterator;
    }
}

BoundaryConsistentPartitionResult deriveBoundaryConsistentPartition(
    dataType::SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    const WatershedRunOptions &repairOptions,
    bool repairCanonicalLabels,
    DistanceMapAlgorithm distanceMapAlgorithm,
    int threadCount) {
    BoundaryConsistentPartitionResult result;
    if (labels.IsNull()) {
        return result;
    }

    relabelInjectedBoundaryComponents(labels, thresholdedBoundaries, result.displayLabels, result.splitComponentIds);
    if (repairCanonicalLabels) {
        result.canonicalLabels = repairSplitLabelsWithWatershed(
            labels,
            thresholdedBoundaries,
            result.displayLabels,
            result.splitComponentIds,
            repairOptions,
            distanceMapAlgorithm,
            threadCount);
    } else {
        result.canonicalLabels = labels;
    }
    return result;
}

void binaryThresholdImageFilterFloat(itk::Image<float, 3>::Pointer &inputImage,
                                     itk::Image<float, 3>::Pointer &outputImage,
                                     float thresholdValueMin) {
    using BinaryThresholdImageFilterType =
        itk::BinaryThresholdImageFilter<itk::Image<float, 3>, itk::Image<float, 3>>;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
}

void castFloatToChar(itk::Image<float, 3>::Pointer &inputImage,
                     itk::Image<unsigned char, 3>::Pointer &outputType) {
    using CastType = itk::CastImageFilter<itk::Image<float, 3>, itk::Image<unsigned char, 3>>;
    CastType::Pointer castFilter = CastType::New();
    castFilter->SetInput(inputImage);
    castFilter->Update();
    outputType = castFilter->GetOutput();
}

void connectedComponentCharToUInt(itk::Image<unsigned char, 3>::Pointer &inputImage,
                                  itk::Image<unsigned int, 3>::Pointer &outputImage) {
    using ConnectedComponentImageFilterType =
        itk::ConnectedComponentImageFilter<itk::Image<unsigned char, 3>, itk::Image<unsigned int, 3>>;
    ConnectedComponentImageFilterType::Pointer componentFilter = ConnectedComponentImageFilterType::New();
    componentFilter->SetInput(inputImage);
    componentFilter->Update();
    outputImage = componentFilter->GetOutput();
}

void extractMinimaFromDistanceMap(itk::Image<float, 3>::Pointer &distanceMap,
                                  itk::Image<unsigned int, 3>::Pointer &seeds,
                                  double minimalHeight,
                                  distance_map_benchmark::SeedExtractorKind seedExtractorKind) {
    std::cout << "Extracting minima ...\n";
    seeds = distance_map_benchmark::extractSeedsFromDistanceImage(distanceMap, seedExtractorKind, minimalHeight);
    std::cout << "Number of seeds: " << getMaximumOfUIntImage(seeds) << std::endl;
}

void filterSmallSegmentSeeds(itk::Image<unsigned int, 3>::Pointer &watershedIn,
                             itk::Image<unsigned int, 3>::Pointer &watershedOut,
                             float volumeThreshold) {
    std::cout << "Filtering\n";
    using SegmentType = itk::Image<unsigned int, 3>;
    using LabelGeometryImageFilterType = itk::LabelGeometryImageFilter<SegmentType, SegmentType>;
    LabelGeometryImageFilterType::Pointer labelGeometryImageFilter = LabelGeometryImageFilterType::New();
    labelGeometryImageFilter->SetInput(watershedIn);
    labelGeometryImageFilter->Update();

    LabelGeometryImageFilterType::LabelsType allLabels = labelGeometryImageFilter->GetLabels();
    LabelGeometryImageFilterType::LabelsType::iterator allLabelsIt;

    using ChangeLabelImageFilterType = itk::ChangeLabelImageFilter<SegmentType, SegmentType>;
    std::map<unsigned int, unsigned int> changeMap;
    for (allLabelsIt = allLabels.begin(); allLabelsIt != allLabels.end(); ++allLabelsIt) {
        LabelGeometryImageFilterType::LabelPixelType labelValue = *allLabelsIt;
        auto labelVolume = labelGeometryImageFilter->GetVolume(labelValue);

        if (labelVolume < volumeThreshold) {
            changeMap[labelValue] = 0;
        }
    }
    std::cout << "Filtered " << changeMap.size() << " seeds! \n";

    ChangeLabelImageFilterType::Pointer changeLabelImageFilter = ChangeLabelImageFilterType::New();
    changeLabelImageFilter->SetInput(watershedIn);
    changeLabelImageFilter->SetChangeMap(changeMap);
    changeLabelImageFilter->Update();
    watershedOut = changeLabelImageFilter->GetOutput();
}
