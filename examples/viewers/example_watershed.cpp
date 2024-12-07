
// read in boundary data
// run watershed pipeline:
// * threshold
// * distancemap
// * minima
// * marker controlled watershed


#include <QApplication>
#include <QtWidgets>
#include <src/viewers/fileIO.h>
#include <QObject>
#include <src/viewers/SliceViewer.h>
#include "itkBinaryThresholdImageFilter.h"
#include <src/viewers/OrthoViewer.h>

#include <itkSignedMaurerDistanceMapImageFilter.h>
#include <itkDiscreteGaussianImageFilter.h>


typedef itk::Image<unsigned char, 3> CharImageType;
typedef itk::Image<float, 3>         FloatImageType;
typedef itk::Image<short, 3>         ShortImageType;
typedef itk::Image<unsigned int, 3>  UIntImageType;

#include "itkHConvexImageFilter.h"
#include <itkConnectedComponentImageFilter.h>
#include "itkStatisticsImageFilter.h"
#include <itkMorphologicalWatershedFromMarkersImageFilter.h>
#include <itkInvertIntensityImageFilter.h>

float getMaximumOfFloatImage(FloatImageType::Pointer &floatImage){
    typedef itk::StatisticsImageFilter<FloatImageType> StatisticsFloatImageFilterType;
    StatisticsFloatImageFilterType::Pointer statisticsFloatImageFilter = StatisticsFloatImageFilterType::New();
    statisticsFloatImageFilter->SetInput(floatImage);
    statisticsFloatImageFilter->Update();
    return statisticsFloatImageFilter->GetMaximum();
}

void invertDistanceMap(FloatImageType::Pointer &distanceMap, FloatImageType::Pointer &invertedDistanceMap){
    typedef itk::InvertIntensityImageFilter<FloatImageType,FloatImageType> InvertIntensityImageFilterType;
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

void runWatershed(FloatImageType::Pointer &invertedDistanceMap,
                  UIntImageType::Pointer &seeds,
                  UIntImageType::Pointer &watershedOut,
                  bool showWatershedLines=false){
    std::cout << "Run watershed ...\n";
    typedef itk::MorphologicalWatershedFromMarkersImageFilter<FloatImageType,UIntImageType> WatershedFilterType;
    typename WatershedFilterType::Pointer watershedFilter = WatershedFilterType::New();
    watershedFilter->SetMarkWatershedLine(showWatershedLines);
    watershedFilter->SetInput1(invertedDistanceMap);
    watershedFilter->SetInput2(seeds);
    watershedFilter->Update();
    watershedOut = watershedFilter->GetOutput();
}


void binaryThresholdImageFilterFloat(FloatImageType::Pointer &inputImage,
                                     FloatImageType::Pointer &outputImage,
                                     float thresholdValueMin){

    typedef itk::BinaryThresholdImageFilter<FloatImageType, FloatImageType> BinaryThresholdImageFilterType;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
};

void castFloatToChar(FloatImageType::Pointer &inputImage, CharImageType::Pointer &outputType){
    typedef itk::CastImageFilter<FloatImageType,CharImageType> CastType;
    CastType::Pointer castFilter = CastType::New();
    castFilter->SetInput(inputImage);
    castFilter->Update();
    outputType = castFilter->GetOutput();
}


void connectedComponentCharToUInt(CharImageType::Pointer &inputImage,
                                  UIntImageType::Pointer &outputImage){
    typedef itk::ConnectedComponentImageFilter<CharImageType,UIntImageType> ConnectedComponentImageFilterType;
    ConnectedComponentImageFilterType::Pointer componentFilter = ConnectedComponentImageFilterType::New();
    componentFilter->SetInput(inputImage);
    componentFilter->Update();
    outputImage = componentFilter->GetOutput();
}

unsigned int getMaximumOfUIntImage(UIntImageType::Pointer &UIntImage){
    typedef itk::StatisticsImageFilter<UIntImageType> StatisticsUIntImageFilterType;
    StatisticsUIntImageFilterType::Pointer statisticsUIntImageFilter = StatisticsUIntImageFilterType::New();
    statisticsUIntImageFilter->SetInput(UIntImage);
    statisticsUIntImageFilter->Update();
    return statisticsUIntImageFilter->GetMaximum();
}


void extractMinimaFromDistanceMap(FloatImageType::Pointer &distanceMap, UIntImageType::Pointer &seeds, double minimalHeight=1) {
    std::cout << "Extracting minima ...\n";
    typedef itk::HConvexImageFilter<FloatImageType, FloatImageType> HConvexImageFilterType;
    HConvexImageFilterType::Pointer hConvexImageFilter = HConvexImageFilterType::New();
    hConvexImageFilter->SetInput(distanceMap);
    hConvexImageFilter->SetHeight(minimalHeight);
    hConvexImageFilter->SetFullyConnected(true);

    FloatImageType::Pointer filteredDistanceMap = FloatImageType::New();
    filteredDistanceMap = hConvexImageFilter->GetOutput();
    hConvexImageFilter->Update();

    FloatImageType::Pointer thresholdedDistanceMapFloat = FloatImageType::New();
    CharImageType::Pointer thresholdedDistanceMapChar = CharImageType::New();
    binaryThresholdImageFilterFloat(filteredDistanceMap, thresholdedDistanceMapFloat, 0.1);
    castFloatToChar(thresholdedDistanceMapFloat, thresholdedDistanceMapChar);


    connectedComponentCharToUInt(thresholdedDistanceMapChar, seeds);
    std::cout << "Number of seeds: " << getMaximumOfUIntImage(seeds) << std::endl;
}

void generateDistanceMap( itk::Image<unsigned char, 3>::Pointer &edgeImage, itk::Image<float, 3>::Pointer &distanceMap,
                         double varianceGaussianFilter){
    // calculation of the distance map
    std::cout << "Generate Distance Map" << std::endl;
    typedef itk::Image<unsigned char, 3> CharImageType;
    typedef itk::Image<float, 3>         FloatImageType;


    //TODO: does the signed distance really make sense here? - experiment with squared distance/chessmap distancemap
    typedef itk::SignedMaurerDistanceMapImageFilter<CharImageType,FloatImageType> SignedMaurerDistanceMapImageFilterType;
    SignedMaurerDistanceMapImageFilterType::Pointer distanceFilter = SignedMaurerDistanceMapImageFilterType::New();
    distanceFilter->SetInput(edgeImage);
    distanceFilter->Update();
    distanceFilter->SetBackgroundValue(0);
    distanceFilter->SquaredDistanceOff();
    distanceFilter->UseImageSpacingOn();

    if (varianceGaussianFilter > 0) {
        //Smoothing of the Distance Map
        typedef itk::DiscreteGaussianImageFilter<FloatImageType, FloatImageType> GaussianFilterType;
        GaussianFilterType::Pointer gaussianFilter = GaussianFilterType::New();
        gaussianFilter->SetVariance(varianceGaussianFilter);
        gaussianFilter->SetInput(distanceFilter->GetOutput());

//    typedef itk::MinimumImageFilter<FloatImageType,FloatImageType,FloatImageType> MinimumFloatImageFilterType;
//    MinimumFloatImageFilterType::Pointer minFloatFilter = MinimumFloatImageFilterType::New();
//    minFloatFilter->SetInput1(distanceFilter->GetOutput());
//    minFloatFilter->SetInput2(gaussianFilter->GetOutput());
//    minFloatFilter->Update();
        distanceMap = gaussianFilter->GetOutput();
    } else {
        distanceMap = distanceFilter->GetOutput();
    }
}

void setBoundariesToValue(itk::Image<unsigned char, 3>::Pointer &CharImage, unsigned char value) {
    typedef itk::Image<unsigned char, 3> CharImageType;
    typedef itk::ImageRegionIterator<CharImageType> CharIteratorType;

    std::vector<unsigned int> indexX(6,0), indexY(6,0), indexZ(6,0);
    std::vector<unsigned int> sizeX(6,0), sizeY(6,0), sizeZ(6,0);

    CharImageType::RegionType LargestRegion = CharImage->GetLargestPossibleRegion();
    CharImageType::SizeType ImageDimensions = LargestRegion.GetSize();
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
        CharImageType::SizeType size_ROI;
        size_ROI[0] = sizeX.at(i);
        size_ROI[1] = sizeY.at(i);
        size_ROI[2] = sizeZ.at(i);

        CharImageType::IndexType index;
        index[0] = indexX.at(i);
        index[1] = indexY.at(i);
        index[2] = indexZ.at(i);

        CharImageType::RegionType imageBoundaryROI;
        imageBoundaryROI.SetSize(size_ROI);
        imageBoundaryROI.SetIndex(index);

        CharIteratorType it(CharImage, imageBoundaryROI);
        unsigned char BoundaryValue = value;
        for (it.GoToBegin(); !it.IsAtEnd(); ++it) { it.Set(BoundaryValue); }
    }
}


int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    QFile f("../qdarkstyle/style.qss");
    if (!f.exists()) {
        printf("Unable to set stylesheet, file not found\n");
    } else {
        f.open(QFile::ReadOnly | QFile::Text);
        QTextStream ts(&f);
        a.setStyleSheet(ts.readAll());
    }

    auto *myMainWindow = new QMainWindow();

    std::shared_ptr<GraphBase> graphBase = std::make_shared<GraphBase>();

    auto* orthoViewer = new OrthoViewer(graphBase);
    graphBase->pOrthoViewer = orthoViewer;

    myMainWindow->setCentralWidget(orthoViewer);


    QString fileName = QFileDialog::getOpenFileName(myMainWindow,
                                                    "Open Images");

    itk::Image<float, 3>::Pointer pMembraneProbability = ITKImageLoader<float>(fileName);


    std::cout << "Thresholding!\n";

    itk::Image<unsigned char, 3>::Pointer pThresholdedMembrane;


    typedef itk::BinaryThresholdImageFilter<itk::Image<float, 3>, itk::Image<unsigned char, 3>> BinaryThresholdImageFilterType;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(pMembraneProbability);
    thresholdFilter->SetLowerThreshold(128);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    pThresholdedMembrane = thresholdFilter->GetOutput();

    std::cout << "Setting boundaries to 1.\n";

    setBoundariesToValue(pThresholdedMembrane, 1);

    std::cout << "Calculating Distancemap.\n";
    typedef itk::Image<float, 3>         FloatImageType;
    FloatImageType::Pointer pDistanceMap = FloatImageType::New();
    double distanceMapSmoothingVariance = 0;
    generateDistanceMap(pThresholdedMembrane, pDistanceMap, distanceMapSmoothingVariance);

    typedef itk::Image<unsigned int, 3>  UIntImageType;

    // create seeds
    UIntImageType::Pointer seeds;
//        createSeedsFromDistanceMap(distanceMap, seeds);
    double minimalMinimaHeight = 4;
    extractMinimaFromDistanceMap(pDistanceMap, seeds, minimalMinimaHeight);

    // the inverted distancemap serves as an input for the watershed algorithm
    FloatImageType::Pointer invertedDistanceMap = FloatImageType::New();
    invertDistanceMap(pDistanceMap, invertedDistanceMap);

    // run watershed on seeds
    UIntImageType::Pointer watershedImage = UIntImageType::New();
    runWatershed(invertedDistanceMap, seeds, watershedImage);

    std::cout << "Done!\n";

    std::unique_ptr<itkSignal<unsigned int>> pSignal2(new itkSignal<unsigned int>(watershedImage));
    pSignal2->setLUTCategorical();
    orthoViewer->addSignal(pSignal2.get());
    myMainWindow->show();
    return a.exec();
}
