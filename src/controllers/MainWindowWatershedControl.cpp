#include "MainWindowWatershedControl.h"
#include <QStatusBar>
#include <src/controllers/watershedControl.h>
#include <QApplication>
#include <QScreen>

void MainWindowWatershedControl::closeFromExternalSignal() {
    close();
}
void MainWindowWatershedControl::receiveStatusMessage(QString string) {
    statusBar()->showMessage(string);
};

void MainWindowWatershedControl::setLinkedSignalControl(SignalControl* linkedSignalControlIn){
    linkedSignalControl = linkedSignalControlIn;
    myWatershedControl->linkedSignalControl = linkedSignalControlIn;
}

MainWindowWatershedControl::MainWindowWatershedControl() {

    std::shared_ptr<GraphBase> graphBase = std::make_shared<GraphBase>();

    graphBase->ignoredSegmentLabels = {};
    graphBase->pWorkingSegmentsImage = dataType::SegmentsImageType::Pointer();
    graphBase->pEdgesInitialSegmentsImage = dataType::EdgeImageType::Pointer();
    graphBase->colorLookUpEdgesStatus = std::unordered_map<char, std::vector<unsigned char>>();
    graphBase->edgeStatus = std::unordered_map<dataType::MappedEdgeIdType, char>();

    Graph *graph = new Graph(graphBase);
    graphBase->pGraph = graph;

    auto myOrthowindow = new OrthoViewer(graphBase);
    graphBase->pOrthoViewer = myOrthowindow;

    myWatershedControl = new WatershedControl(graphBase);
    auto horizontalSplitter = new QSplitter();
    horizontalSplitter->addWidget(myWatershedControl);
    horizontalSplitter->addWidget(myOrthowindow);
    horizontalSplitter->setStretchFactor(0, 1);
    horizontalSplitter->setStretchFactor(1, 3);

    setCentralWidget(horizontalSplitter);

    // ==== WATERSHED STUFF ===== ///


//    myWatershedControl->addBoundaries("/Users/joachimgreiner/Documents/memcrop.nrrd");
//    graphBase->pOrthoViewer->setViewToMiddleOfStack();

//    myWatershedControl->thresholdBoundaries();
//    myWatershedControl->calculateDistanceMap();
//    myWatershedControl->extractSeeds();
//    myWatershedControl->watershed();  

//
//
//    std::cout << "Thresholding!\n";
//
//    itk::Image<unsigned char, 3>::Pointer pThresholdedMembrane;
//
//    binaryThresholdImageFilterFloat(pMembraneProbability,pThresholdedMembrane, 128);
//
//
//    std::cout << "Setting boundaries to 1.\n";
//
//    setBoundariesToValue(pThresholdedMembrane, 1);
//
//    std::cout << "Calculating Distancemap.\n";
//    typedef itk::Image<float, 3>         FloatImageType;
//    FloatImageType::Pointer pDistanceMap = FloatImageType::New();
//    double distanceMapSmoothingVariance = 0;
//    generateDistanceMap(pThresholdedMembrane, pDistanceMap, distanceMapSmoothingVariance);
//
//    typedef itk::Image<unsigned int, 3>  UIntImageType;
//
//    // create seeds
//    UIntImageType::Pointer seeds;
////        createSeedsFromDistanceMap(distanceMap, seeds);
//    double minimalMinimaHeight = 4;
//    extractMinimaFromDistanceMap(pDistanceMap, seeds, minimalMinimaHeight);
//
//    // the inverted distancemap serves as an input for the watershed algorithm
//    FloatImageType::Pointer invertedDistanceMap = FloatImageType::New();
//    invertDistanceMap(pDistanceMap, invertedDistanceMap);
//
//    // run watershed on seeds
//    UIntImageType::Pointer watershedImage = UIntImageType::New();
//    runWatershed(invertedDistanceMap, seeds, watershedImage);
//
//    std::cout << "Done!\n";
//
//    myWatershedControl->addSegmentsGraph(watershedImage);

    connect(myOrthowindow, &OrthoViewer::sendStatusMessage, this, &MainWindowWatershedControl::receiveStatusMessage);
    connect(myWatershedControl, &WatershedControl::sendClosingSignal, this, &MainWindowWatershedControl::closeFromExternalSignal);

    QRect rec =    QGuiApplication::primaryScreen()->geometry();
    unsigned int screenWidth = rec.width();
    unsigned int screenHeight = rec.height();
    printf("width: %d height: %d\n", screenWidth, screenHeight);
    this->resize(0.9*screenWidth, 0.9*screenHeight); //have to do this
    this->showMaximized();
}