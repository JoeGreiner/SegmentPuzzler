#include "MainWindowWatershedControl.h"
#include <QStatusBar>
#include <src/controllers/watershedControl.h>
#include <QApplication>
#include <QScreen>
#include <QTimer>
#include "src/qtUtils/TaskRunner.h"

MainWindowWatershedControl::~MainWindowWatershedControl() = default;

namespace {

void showWindowWithinAvailableScreen(QMainWindow *window) {
    if (window == nullptr) {
        return;
    }

    window->show();
    QTimer::singleShot(0, window, [window]() {
        QScreen *screen = window->screen();
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }

        if (screen == nullptr) {
            return;
        }

        const QRect availableGeometry = screen->availableGeometry();
        const QRect frameGeometry = window->frameGeometry();
        const QRect contentGeometry = window->geometry();

        const int leftFrameMargin = contentGeometry.left() - frameGeometry.left();
        const int topFrameMargin = contentGeometry.top() - frameGeometry.top();
        const int rightFrameMargin = frameGeometry.right() - contentGeometry.right();
        const int bottomFrameMargin = frameGeometry.bottom() - contentGeometry.bottom();

        QRect targetGeometry = availableGeometry.adjusted(leftFrameMargin,
                                                          topFrameMargin,
                                                          -rightFrameMargin,
                                                          -bottomFrameMargin);
        if (targetGeometry.width() < 1 || targetGeometry.height() < 1) {
            targetGeometry = availableGeometry;
        }

        window->setGeometry(targetGeometry);
    });
}

} // namespace

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

MainWindowWatershedControl::MainWindowWatershedControl(WatershedControl::OutputMode outputMode) {

    std::shared_ptr<GraphBase> graphBase = std::make_shared<GraphBase>();

    graphBase->ignoredSegmentLabels = {};
    graphBase->pWorkingSegmentsImage = dataType::SegmentsImageType::Pointer();
    graphBase->pEdgesInitialSegmentsImage = dataType::EdgeImageType::Pointer();
    graphBase->colorLookUpEdgesStatus = std::unordered_map<char, std::vector<unsigned char>>();
    graphBase->edgeStatus = std::unordered_map<dataType::MappedEdgeIdType, char>();

    ownedGraph = std::make_unique<Graph>(graphBase);
    graphBase->pGraph = ownedGraph.get();
    Graph *graph = ownedGraph.get();

    taskRunner = std::make_unique<TaskRunner>(this, this);
    myOrthowindow = new OrthoViewer(graphBase, taskRunner.get());

    myWatershedControl = new WatershedControl(graphBase, myOrthowindow, taskRunner.get(), outputMode);
    auto horizontalSplitter = new QSplitter();
    horizontalSplitter->addWidget(myWatershedControl);
    horizontalSplitter->addWidget(myOrthowindow);
    horizontalSplitter->setStretchFactor(0, 1);
    horizontalSplitter->setStretchFactor(1, 3);

    setCentralWidget(horizontalSplitter);

    // ==== WATERSHED STUFF ===== ///


//    myWatershedControl->addBoundaries("/Users/joachimgreiner/Documents/memcrop.nrrd");

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
    showWindowWithinAvailableScreen(this);
}
