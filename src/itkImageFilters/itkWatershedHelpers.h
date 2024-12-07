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


void generateDistanceMap( itk::Image<unsigned char, 3>::Pointer &edgeImage, itk::Image<float, 3>::Pointer &distanceMap,
                          double varianceGaussianFilter){
    // calculation of the distance map
    std::cout << "Generate Distance Map" << std::endl;


    //TODO: does the signed distance really make sense here? - experiment with squared distance/chessmap distancemap
    typedef itk::SignedMaurerDistanceMapImageFilter<itk::Image<unsigned char, 3> ,itk::Image<float, 3> > SignedMaurerDistanceMapImageFilterType;
    SignedMaurerDistanceMapImageFilterType::Pointer distanceFilter = SignedMaurerDistanceMapImageFilterType::New();
    distanceFilter->SetInput(edgeImage);
    distanceFilter->SetBackgroundValue(0);
    distanceFilter->SquaredDistanceOff();
    distanceFilter->UseImageSpacingOff();

    if (varianceGaussianFilter > 0) {
        //Smoothing of the Distance Map
        typedef itk::DiscreteGaussianImageFilter<itk::Image<float, 3> , itk::Image<float, 3> > GaussianFilterType;
        GaussianFilterType::Pointer gaussianFilter = GaussianFilterType::New();
        gaussianFilter->SetVariance(varianceGaussianFilter);
        gaussianFilter->SetInput(distanceFilter->GetOutput());
        gaussianFilter->SetUseImageSpacingOff();

        distanceMap = gaussianFilter->GetOutput();
        gaussianFilter->Update();
    } else {
        distanceMap = distanceFilter->GetOutput();
        distanceFilter->Update();
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
                  bool showWatershedLines=false){
    std::cout << "Run watershed ...\n";
    typedef itk::MorphologicalWatershedFromMarkersImageFilter<itk::Image<float, 3> ,itk::Image<unsigned int, 3> > WatershedFilterType;
    typename WatershedFilterType::Pointer watershedFilter = WatershedFilterType::New();
    watershedFilter->SetMarkWatershedLine(showWatershedLines);
    watershedFilter->SetInput1(invertedDistanceMap);
    watershedFilter->SetInput2(seeds);
    watershedFilter->Update();
    watershedOut = watershedFilter->GetOutput();
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


void extractMinimaFromDistanceMap(itk::Image<float, 3> ::Pointer &distanceMap, itk::Image<unsigned int, 3> ::Pointer &seeds, double minimalHeight=1) {
    std::cout << "Extracting minima ...\n";
    typedef itk::HConvexImageFilter<itk::Image<float, 3> , itk::Image<float, 3> > HConvexImageFilterType;
    HConvexImageFilterType::Pointer hConvexImageFilter = HConvexImageFilterType::New();
    hConvexImageFilter->SetInput(distanceMap);
    hConvexImageFilter->SetHeight(minimalHeight);
    hConvexImageFilter->SetFullyConnected(true);

    itk::Image<float, 3> ::Pointer filteredDistanceMap = itk::Image<float, 3> ::New();
    filteredDistanceMap = hConvexImageFilter->GetOutput();
    hConvexImageFilter->Update();

    itk::Image<float, 3> ::Pointer thresholdedDistanceMapFloat = itk::Image<float, 3> ::New();
    itk::Image<unsigned char, 3> ::Pointer thresholdedDistanceMapChar = itk::Image<unsigned char, 3> ::New();
    binaryThresholdImageFilterFloat(filteredDistanceMap, thresholdedDistanceMapFloat, 0.1);
    castFloatToChar(thresholdedDistanceMapFloat, thresholdedDistanceMapChar);


    connectedComponentCharToUInt(thresholdedDistanceMapChar, seeds);
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
