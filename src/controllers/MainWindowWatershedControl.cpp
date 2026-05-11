#include "MainWindowWatershedControl.h"
#include <QStatusBar>
#include <src/controllers/watershedControl.h>
#include <QApplication>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/WindowStats.h"
#include "src/utils/AppLogger.h"

MainWindowWatershedControl::~MainWindowWatershedControl() = default;

namespace {

void showWindowWithinAvailableScreen(QMainWindow *window) {
    if (window == nullptr) {
        return;
    }

    window->show();
    QTimer::singleShot(0, window, [window]() {
        QScreen *screen = nullptr;
        if (window->windowHandle() != nullptr) {
            screen = window->windowHandle()->screen();
        }
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

        auto orthoViewer = window->findChild<OrthoViewer *>();
        if (orthoViewer) {
            const double fittedZoom = orthoViewer->computeFittedZoom();
            if (fittedZoom > 0) {
                const double initialZoom = fittedZoom;
                orthoViewer->xy->setZoom(initialZoom);
                orthoViewer->xz->setZoom(initialZoom);
                orthoViewer->zy->setZoom(initialZoom);
            }
            orthoViewer->refreshZoomLayout();
        }
    });
}

} // namespace

void MainWindowWatershedControl::closeFromExternalSignal() {
    SP_LOG_INFO("watershed", QStringLiteral("Closing watershed window from external signal"));
    close();
}
void MainWindowWatershedControl::receiveStatusMessage(QString string) {
    statusBar()->showMessage(string);
};

void MainWindowWatershedControl::setLinkedSignalControl(SignalControl* linkedSignalControlIn){
    linkedSignalControl = linkedSignalControlIn;
    myWatershedControl->linkedSignalControl = linkedSignalControlIn;
    SP_LOG_INFO("watershed", QStringLiteral("Linked watershed window to the main SignalControl"));
}

MainWindowWatershedControl::MainWindowWatershedControl(WatershedControl::OutputMode outputMode) {
    SP_LOG_INFO("watershed", QStringLiteral("Opening watershed window"));

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
    myOrthowindow->setShortcutLegendProfile(OrthoViewer::ShortcutLegendProfile::Watershed);

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
    windowStats::setupWindowTitleStatsTimer(this, "SegmentPuzzler Watershed");
    showWindowWithinAvailableScreen(this);
}
