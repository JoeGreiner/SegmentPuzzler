#ifndef SEGMENTCOUPLER_ITKWATERSHEDHELPERS_H
#define SEGMENTCOUPLER_ITKWATERSHEDHELPERS_H

#include "itkImage.h"

#include "itkHConvexImageFilter.h"
#include <itkConnectedComponentImageFilter.h>
#include "itkStatisticsImageFilter.h"
#include <itkMorphologicalWatershedFromMarkersImageFilter.h>
#include <itkInvertIntensityImageFilter.h>

#include "itkBinaryThresholdImageFilter.h"

#include <itkSignedMaurerDistanceMapImageFilter.h>
#include <itkDiscreteGaussianImageFilter.h>
#include <itkLabelGeometryImageFilter.h>
#include <itkChangeLabelImageFilter.h>

#include "src/utils/DistanceMapFH3D.h"
#include "src/utils/DistanceMapSeedExtractors.h"
#include "src/utils/FastMarkerWatershed3D.h"

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

void binaryThresholdImageFilterFloat(itk::Image<unsigned short, 3> ::Pointer &inputImage,
                                     itk::Image<unsigned char, 3> ::Pointer &outputImage,
                                     float thresholdValueMin){

    typedef itk::BinaryThresholdImageFilter<itk::Image<unsigned short, 3> , itk::Image<unsigned char, 3> > BinaryThresholdImageFilterType;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
};


unsigned int getMaximumOfUIntImage(itk::Image<unsigned int, 3> ::Pointer &UIntImage){
    typedef itk::StatisticsImageFilter<itk::Image<unsigned int, 3> > StatisticsUIntImageFilterType;
    StatisticsUIntImageFilterType::Pointer statisticsUIntImageFilter = StatisticsUIntImageFilterType::New();
    statisticsUIntImageFilter->SetInput(UIntImage);
    statisticsUIntImageFilter->Update();
    return statisticsUIntImageFilter->GetMaximum();
}


void setBoundariesToValue(itk::Image<unsigned char, 3>::Pointer &CharImage, unsigned char value) {
    typedef itk::ImageRegionIterator<itk::Image<unsigned char, 3> > CharIteratorType;

    std::vector<unsigned int> indexX(6,0), indexY(6,0), indexZ(6,0);
    std::vector<unsigned int> sizeX(6,0), sizeY(6,0), sizeZ(6,0);

    itk::Image<unsigned char, 3> ::RegionType LargestRegion = CharImage->GetLargestPossibleRegion();
    itk::Image<unsigned char, 3> ::SizeType ImageDimensions = LargestRegion.GetSize();
    std::vector<unsigned int> dim(3,0);
    dim[0] = static_cast<unsigned int>(ImageDimensions[0]);
    dim[1] = static_cast<unsigned int>(ImageDimensions[1]);
    dim[2] = static_cast<unsigned int>(ImageDimensions[2]);

    //        bottom  top      left   right    front   back
    indexX = {0     , 0       , 0     ,dim[0]-1,0     ,0       };
    indexY = {0     , 0       , 0     ,0       ,0     ,dim[1]-1};
    indexZ = {0     , dim[2]-1, 0     ,0       ,0     ,0       };
    sizeX =  {dim[0], dim[0]  , 1     ,1       ,dim[0],dim[0]  };
    sizeY =  {dim[1], dim[1]  , dim[1],dim[1]  ,1     ,1       };
    sizeZ =  {1     , 1       , dim[2],dim[2]  ,dim[2],dim[2]  };

    for(unsigned int i=0; i<indexX.size(); ++i){
        itk::Image<unsigned char, 3> ::SizeType size_ROI;
        size_ROI[0] = sizeX.at(i);
        size_ROI[1] = sizeY.at(i);
        size_ROI[2] = sizeZ.at(i);

        itk::Image<unsigned char, 3> ::IndexType index;
        index[0] = indexX.at(i);
        index[1] = indexY.at(i);
        index[2] = indexZ.at(i);

        itk::Image<unsigned char, 3> ::RegionType imageBoundaryROI;
        imageBoundaryROI.SetSize(size_ROI);
        imageBoundaryROI.SetIndex(index);

        CharIteratorType it(CharImage, imageBoundaryROI);
        unsigned char BoundaryValue = value;
        for (it.GoToBegin(); !it.IsAtEnd(); ++it) { it.Set(BoundaryValue); }
    }
}


void generateDistanceMap(itk::Image<unsigned char, 3>::Pointer &edgeImage,
                         itk::Image<float, 3>::Pointer &distanceMap,
                         double varianceGaussianFilter,
                         DistanceMapAlgorithm algorithm = DistanceMapAlgorithm::Maurer,
                         int threadCount = 1){
    // calculation of the distance map
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
        typedef itk::SignedMaurerDistanceMapImageFilter<itk::Image<unsigned char, 3> ,itk::Image<float, 3> > SignedMaurerDistanceMapImageFilterType;
        SignedMaurerDistanceMapImageFilterType::Pointer distanceFilter = SignedMaurerDistanceMapImageFilterType::New();
        distanceFilter->SetInput(edgeImage);
        distanceFilter->SetBackgroundValue(0);
        distanceFilter->SquaredDistanceOff();
        distanceFilter->UseImageSpacingOff();

        distanceMap = distanceFilter->GetOutput();
        distanceFilter->Update();
    }

    if (varianceGaussianFilter > 0) {
        typedef itk::DiscreteGaussianImageFilter<itk::Image<float, 3> , itk::Image<float, 3> > GaussianFilterType;
        GaussianFilterType::Pointer gaussianFilter = GaussianFilterType::New();
        gaussianFilter->SetVariance(varianceGaussianFilter);
        gaussianFilter->SetInput(distanceMap);
        gaussianFilter->SetUseImageSpacingOff();

        distanceMap = gaussianFilter->GetOutput();
        gaussianFilter->Update();
    }
}


float getMaximumOfFloatImage(itk::Image<float, 3> ::Pointer &floatImage){
    typedef itk::StatisticsImageFilter<itk::Image<float, 3> > StatisticsFloatImageFilterType;
    StatisticsFloatImageFilterType::Pointer statisticsFloatImageFilter = StatisticsFloatImageFilterType::New();
    statisticsFloatImageFilter->SetInput(floatImage);
    statisticsFloatImageFilter->Update();
    return statisticsFloatImageFilter->GetMaximum();
}

void invertDistanceMap(itk::Image<float, 3> ::Pointer &distanceMap, itk::Image<float, 3> ::Pointer &invertedDistanceMap){
    typedef itk::InvertIntensityImageFilter<itk::Image<float, 3> ,itk::Image<float, 3> > InvertIntensityImageFilterType;
    InvertIntensityImageFilterType::Pointer invertFilter = InvertIntensityImageFilterType::New();

    float maxDistance = getMaximumOfFloatImage(distanceMap);
    float DistLowestLevel = ceil(maxDistance);

    invertFilter->SetInput(distanceMap);
    invertFilter->SetMaximum(DistLowestLevel + 1);
    invertFilter->Update();
    invertedDistanceMap = invertFilter->GetOutput();

    std::cout << "New Maximum should be: " << invertFilter->GetMaximum() << std::endl;
    std::string outAfter = "distanceMapInverted.flat";
    std::cout << "Maximum after inverting the distancemap : " << getMaximumOfFloatImage(invertedDistanceMap) << std::endl;

}

void runWatershed(itk::Image<float, 3> ::Pointer &invertedDistanceMap,
                  itk::Image<unsigned int, 3> ::Pointer &seeds,
                  itk::Image<unsigned int, 3> ::Pointer &watershedOut,
                  const WatershedRunOptions &options = WatershedRunOptions(),
                  segment_puzzler::FastMarkerWatershedMetrics *fastMetrics = nullptr){
    std::cout << "Run watershed ...\n";
    switch (options.algorithm) {
        case WatershedAlgorithm::MorphologicalWatershedFromMarkers: {
            typedef itk::MorphologicalWatershedFromMarkersImageFilter<itk::Image<float, 3> ,itk::Image<unsigned int, 3> > WatershedFilterType;
            typename WatershedFilterType::Pointer watershedFilter = WatershedFilterType::New();
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

void insertBoundariesIntoWatershed(itk::Image<unsigned int, 3> ::Pointer &watershed,
                                   itk::Image<unsigned char, 3> ::Pointer &thresholdedBoundaries){

    itk::ImageRegionIterator<itk::Image<unsigned int, 3>> wsIterator(watershed,
            watershed->GetLargestPossibleRegion());
    itk::ImageRegionIterator<itk::Image<unsigned char, 3>> thresholdIterator(thresholdedBoundaries,
            thresholdedBoundaries->GetLargestPossibleRegion());
    bool insertWSasHighestValue = false;
    unsigned int valueOfBoundaryInWS;
    if (insertWSasHighestValue){
        valueOfBoundaryInWS = getMaximumOfUIntImage(watershed) + 1;
    } else {
        valueOfBoundaryInWS = 0;
    }
//    unsigned int valueOfBoundaryInWS = 0;
    std::cout << "Inserting boundaries into watershed with value: " << valueOfBoundaryInWS << "\n";
    for (thresholdIterator.GoToBegin(); !thresholdIterator.IsAtEnd(); ++thresholdIterator) {
        if(thresholdIterator.Get() >= 1){
            wsIterator.Set(valueOfBoundaryInWS);
        }
        ++wsIterator;
    }
}



void binaryThresholdImageFilterFloat(itk::Image<float, 3> ::Pointer &inputImage,
                                     itk::Image<float, 3> ::Pointer &outputImage,
                                     float thresholdValueMin){

    typedef itk::BinaryThresholdImageFilter<itk::Image<float, 3> , itk::Image<float, 3> > BinaryThresholdImageFilterType;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
};

void castFloatToChar(itk::Image<float, 3> ::Pointer &inputImage, itk::Image<unsigned char, 3> ::Pointer &outputType){
    typedef itk::CastImageFilter<itk::Image<float, 3> ,itk::Image<unsigned char, 3> > CastType;
    CastType::Pointer castFilter = CastType::New();
    castFilter->SetInput(inputImage);
    castFilter->Update();
    outputType = castFilter->GetOutput();
}


void connectedComponentCharToUInt(itk::Image<unsigned char, 3> ::Pointer &inputImage,
                                  itk::Image<unsigned int, 3> ::Pointer &outputImage){
    typedef itk::ConnectedComponentImageFilter<itk::Image<unsigned char, 3> ,itk::Image<unsigned int, 3> > ConnectedComponentImageFilterType;
    ConnectedComponentImageFilterType::Pointer componentFilter = ConnectedComponentImageFilterType::New();
    componentFilter->SetInput(inputImage);
    componentFilter->Update();
    outputImage = componentFilter->GetOutput();
}


void extractMinimaFromDistanceMap(itk::Image<float, 3> ::Pointer &distanceMap,
                                  itk::Image<unsigned int, 3> ::Pointer &seeds,
                                  double minimalHeight=1,
                                  distance_map_benchmark::SeedExtractorKind seedExtractorKind =
                                      distance_map_benchmark::SeedExtractorKind::HConvex) {
    std::cout << "Extracting minima ...\n";
    seeds = distance_map_benchmark::extractSeedsFromDistanceImage(distanceMap, seedExtractorKind, minimalHeight);
    std::cout << "Number of seeds: " << getMaximumOfUIntImage(seeds) << std::endl;
}


void filterSmallSegmentSeeds(itk::Image<unsigned int, 3> ::Pointer &watershedIn, itk::Image<unsigned int, 3> ::Pointer &watershedOut, float volumeThreshold=2000){
    std::cout << "Filtering\n";
    using SegmentType = itk::Image<unsigned int, 3>;
    typedef itk::LabelGeometryImageFilter<SegmentType, SegmentType> LabelGeometryImageFilterType;
    typename LabelGeometryImageFilterType::Pointer labelGeometryImageFilter = LabelGeometryImageFilterType::New();
    labelGeometryImageFilter->SetInput( watershedIn);
    labelGeometryImageFilter->Update();

    LabelGeometryImageFilterType::LabelsType allLabels = labelGeometryImageFilter->GetLabels();
    LabelGeometryImageFilterType::LabelsType::iterator allLabelsIt;

    typedef itk::ChangeLabelImageFilter<SegmentType, SegmentType>  ChangeLabelImageFilterType;
    std::map<unsigned int, unsigned int> changeMap;
    for( allLabelsIt = allLabels.begin(); allLabelsIt != allLabels.end(); allLabelsIt++ )
    {
        LabelGeometryImageFilterType::LabelPixelType labelValue = *allLabelsIt;
        auto labelVolume = labelGeometryImageFilter->GetVolume(labelValue);

        if(labelVolume < volumeThreshold){
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

#endif //SEGMENTCOUPLER_ITKWATERSHEDHELPERS_H
