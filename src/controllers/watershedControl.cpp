#include "watershedControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"
#include <itkImage.h>
#include "itkRegionOfInterestImageFilter.h"
#include "itkPasteImageFilter.h"


#include <src/viewers/itkSignal.h>
#include <QFileDialog>
#include <QColorDialog>
#include <src/segment_handling/graph.h>
#include <src/viewers/OrthoViewer.h>
#include <QInputDialog>
#include <QHeaderView>
#include <QAbstractItemView>
#include <src/viewers/fileIO.h>


#include <QTreeWidget>
#include <QMimeData>
#include <QDragEnterEvent>
#include <src/itkImageFilters/itkWatershedHelpers.h>
#include <src/viewers/itkSignalThresholdPreview.h>
#include <QtWidgets/QProgressDialog>
#include <QtCore/QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <QtWidgets/QMessageBox>
#include <QCheckBox>
#include <QSpinBox>


WatershedControl::~WatershedControl() {

}


void WatershedControl::addImage(QString fileName) {
    std::cout << "Adding file: " << fileName.toStdString() << std::endl;
    if (!fileName.isEmpty()) {
        itk::ImageIOBase::IOComponentType dataType;
        size_t signalIndexLocal;
        size_t signalIndexGlobal;
        bool loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal, false);
        if (loadSuccessFull) {
            globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
            allSignalList[signalIndexGlobal]->setLUTContinuous();
            allSignalList[signalIndexGlobal]->setName(QFileInfo(fileName).baseName());
            allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
            graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        }
    }
}

WatershedControl::WatershedControl(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent, bool verboseIn) {
    setParent(parent);
    graphBase = graphBaseIn;
    verbose = verboseIn;
    allSignalList.reserve(10);
    itkSignalSegmentsGraph = nullptr;
    pThresholdedMembrane = nullptr;
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);

    useROI = false;
    fx = 0;
    fy = 0;
    fz = 0;
    tx = 0;
    ty = 0;
    tz = 0;


#ifdef SEGMENTSHORT
    pSegmentTypeSignalList = &shortSignalList;
    pSegmentTypeImageList = &shortImageList;
#endif

#ifdef SEGMENTUINT
    pSegmentTypeSignalList = &uIntSignalList;
    pSegmentTypeImageList = &uIntImageList;
#endif

    this->setTabPosition(QTabWidget::South);

    setupSignalTreeWidget();
    setupThresholdWidget();
    setupDistanceMapWidget();
    setupSeedWidget();
    setupWatershedWidget();
}

void WatershedControl::setupSignalTreeWidget() {
    signalTreeWidget = new QTreeWidget(this);
    signalInputButtonsWidget = new QWidget(this);
    signalInputButtonsLayout = new QGridLayout();
    thresholdBoundariesButton = new QPushButton("Threshold Boundaries");
    setupWidget(signalTreeWidget, signalInputButtonsWidget, signalInputButtonsLayout, thresholdBoundariesButton, "Boundaries");
    connect(thresholdBoundariesButton, SIGNAL (clicked()), this, SLOT (thresholdBoundariesPressed()));
    thresholdValueSlider = new QSlider(Qt::Horizontal, this);
//    thresholdValueSlider->setRange(0, 3000);
    thresholdBoundariesButton->parentWidget()->layout()->addWidget(thresholdValueSlider);
    connect(thresholdValueSlider, SIGNAL (valueChanged(int)),
            this, SLOT(forwardValueChangedSignal(int)));
}
void WatershedControl::forwardValueChangedSignal(int value) {
    std::cout << value << "\n";
    pBoundariesSignal->thresholdValue = value;
    for (auto &viewer : graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void WatershedControl::setupThresholdWidget() {
    thresholdTreeWidget = new QTreeWidget(this);
    thresholdButtonsWidget = new QWidget(this);
    thresholdButtonsLayout = new QGridLayout();
    calculateDistanceMapButton = new QPushButton("Calculate Distancemap");
    setupWidget(thresholdTreeWidget, thresholdButtonsWidget, thresholdButtonsLayout, calculateDistanceMapButton, "Threshold");
    connect(calculateDistanceMapButton, SIGNAL (clicked()), this, SLOT (calculateDistanceMapPressed()));

    togglePaintBoundaryModeButton = new QPushButton("Turn Paintmode On");
    calculateDistanceMapButton->parentWidget()->layout()->addWidget(togglePaintBoundaryModeButton);
    connect(togglePaintBoundaryModeButton, SIGNAL (clicked()), this, SLOT (togglePaintBoundaryMode()));
}


void WatershedControl::setupDistanceMapWidget() {
    distanceMapTreeWidget = new QTreeWidget(this);
    distanceMapButtonsWidget = new QWidget(this);
    distanceMapButtonsLayout = new QGridLayout(this);
    calculateSeedsButton = new QPushButton("Extract Seeds");
    setupWidget(distanceMapTreeWidget, distanceMapButtonsWidget, distanceMapButtonsLayout, calculateSeedsButton, "DistanceMap");
    connect(calculateSeedsButton, SIGNAL (clicked()), this, SLOT (extractSeedsPressed()));

}

void WatershedControl::setupWidget(QTreeWidget *treeWidget, QWidget *buttonsWidget, QGridLayout *buttonsLayout, QPushButton *button, QString tabName){
    QWidget *mainWidget = new QWidget(this);
    QVBoxLayout *mainWidgetLayout = new QVBoxLayout();
    mainWidget->setLayout(mainWidgetLayout);

    treeWidget->setFocusPolicy(Qt::NoFocus);
    treeWidget->setColumnCount(2);
    treeWidget->setHeaderLabels({"Name", "Properties"});
    treeWidget->header()->setStretchLastSection(false);
    treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mainWidgetLayout->addWidget(treeWidget);

    mainWidgetLayout->addWidget(buttonsWidget);
    buttonsWidget->setLayout(buttonsLayout);
    buttonsLayout->addWidget(button);

    connect(treeWidget, SIGNAL (itemDoubleClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeDoubleClicked(QTreeWidgetItem * , int)));
    connect(treeWidget, SIGNAL (itemClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeClicked(QTreeWidgetItem * , int)));

    this->addTab(mainWidget, tabName);
}

void WatershedControl::setupSeedWidget() {
    seedsTreeWidget = new QTreeWidget(this);
    seedsButtonsWidget = new QWidget(this);
    seedsButtonsLayout = new QGridLayout();
    runWatershedButton = new QPushButton("Run Watershed", this);



//    setupWidget(seedsTreeWidget, seedsButtonsWidget, seedsButtonsLayout, runWatershedButton, "Seeds");
    QWidget *mainWidget = new QWidget(this);
    QVBoxLayout *mainWidgetLayout = new QVBoxLayout();
    mainWidget->setLayout(mainWidgetLayout);

    seedsTreeWidget->setFocusPolicy(Qt::NoFocus);
    seedsTreeWidget->setColumnCount(2);
    seedsTreeWidget->setHeaderLabels({"Name", "Properties"});
    seedsTreeWidget->header()->setStretchLastSection(false);
    seedsTreeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mainWidgetLayout->addWidget(seedsTreeWidget);

    mainWidgetLayout->addWidget(seedsButtonsWidget);

    checkBoxFiltering = new QCheckBox("Activate Segment Size Filtering", this);
    checkBoxFiltering->setChecked(true);
    seedsButtonsLayout->addWidget(checkBoxFiltering);

    sizeFilteringInput = new QSpinBox(mainWidget);
    sizeFilteringInput->setMinimum(100);
    sizeFilteringInput->setMaximum(10000000);
    sizeFilteringInput->setValue(5000);
    sizeFilteringInput->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);

    seedsButtonsLayout->addWidget(sizeFilteringInput);


    seedsButtonsWidget->setLayout(seedsButtonsLayout);
    seedsButtonsLayout->addWidget(runWatershedButton);



    connect(seedsTreeWidget, SIGNAL (itemDoubleClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeDoubleClicked(QTreeWidgetItem * , int)));
    connect(seedsTreeWidget, SIGNAL (itemClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeClicked(QTreeWidgetItem * , int)));

    this->addTab(mainWidget, "Seeds");


    connect(runWatershedButton, SIGNAL (clicked()), this, SLOT (watershedPressed()));
}


void WatershedControl::setupWatershedWidget() {
    watershedTreeWidget = new QTreeWidget(this);
//    watershedTreeWidget->setAccessibleName("yellow");
    watershedButtonsWidget = new QWidget(this);
    watershedButtonsLayout = new QGridLayout();
    exportSegmentButton = new QPushButton("Export Segments", this);
    setupWidget(watershedTreeWidget, watershedButtonsWidget, watershedButtonsLayout, exportSegmentButton, "Watershed");
    connect(exportSegmentButton, SIGNAL (clicked()), this, SLOT (exportSegmentsPressed()));
}


void WatershedControl::treeDoubleClicked(QTreeWidgetItem *item, int) {
    if (item->text(0) == "Color") {
        setUserColor(item);
    } else if (item->text(0) == "Norm") {
        setUserNorm(item);
    } else if (item->text(0) == "Alpha") {
        setUserAlpha(item);
    } else if ((item->text(1) == "active") || (item->text(1) == "inactive")) {
        setDescription(item);

    }
}


void WatershedControl::treeClicked(QTreeWidgetItem *item, int) {
    std::string itemText = item->text(1).toStdString();
    if ((itemText == "active") || (itemText == "inactive")) {
        if (itemText == "inactive" && (item->checkState(0) == Qt::CheckState::Checked)) {
            setIsActive(item, true);
        } else if (itemText == "active" && (item->checkState(0) == Qt::CheckState::Unchecked)) {
            setIsActive(item, false);
        }
    }
}

void WatershedControl::setUserColor(QTreeWidgetItem *item) {
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);
    if (verbose) {
        printf("Setting Color: short: %d uChar: %d segments: %d %i", isShort, isUChar, isSegments, signalIndex);
    }

    QColor newColor = QColorDialog::getColor();
    QImage colorIcon = QImage(30, 30, QImage::Format_RGBA8888);
    colorIcon.fill(newColor);
    item->setIcon(1, QPixmap::fromImage(colorIcon));
    item->parent()->setIcon(1, QPixmap::fromImage(colorIcon));
    std::string colorString = std::to_string(newColor.red())
                              + " " + std::to_string(newColor.green())
                              + " " + std::to_string(newColor.blue());
    item->setText(1, QString::fromStdString(colorString));

    allSignalList[signalIndex]->setMainColor(newColor);

    for (auto &viewer : graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void WatershedControl::setDescription(QTreeWidgetItem *item) {
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);
    if (verbose) {
        printf("Setting Name: short: %d uChar: %d segments: %d %i", isShort, isUChar, isSegments, signalIndex);
    }

    bool inputSuccessful;
    QString newName =
            QInputDialog::getText(this, "Set New Description", "Enter new Descriptor:", QLineEdit::Normal, QString(),
                                  &inputSuccessful);

    if (inputSuccessful) {
        if (item->text(0) == "Segments") { // TODO: put some unique identifier besides name/descriptor for segments
            std::cout << "TODO: Fix segment descriptor change!\n";
        } else {
            item->setText(0, newName);
            allSignalList[signalIndex]->setName(newName);
        }
    }
}


void WatershedControl::togglePaintMode() {
//    if (togglePaintBrushButton->text() == "Turn Paintmode On") {
//        togglePaintBrushButton->setText("Turn Paintmode Off");
//    } else {
//        togglePaintBrushButton->setText("Turn Paintmode On");
//    }
//
//    graphBase->pOrthoViewer->xy->togglePaintMode();
//    graphBase->pOrthoViewer->zy->togglePaintMode();
//    graphBase->pOrthoViewer->xz->togglePaintMode();
}

void WatershedControl::togglePaintBoundaryMode() {
    if (togglePaintBoundaryModeButton->text() == "Turn Paintmode On") {
        togglePaintBoundaryModeButton->setText("Turn Paintmode Off");
    } else {
        togglePaintBoundaryModeButton->setText("Turn Paintmode On");
    }

    graphBase->pOrthoViewer->xy->togglePaintBoundaryMode();
    graphBase->pOrthoViewer->zy->togglePaintBoundaryMode();
    graphBase->pOrthoViewer->xz->togglePaintBoundaryMode();
}



void WatershedControl::setIsActive(QTreeWidgetItem *item, bool isActiveIn) {
    if (verbose) { std::cout << "Setting item isActive to: " << isActiveIn << std::endl; }
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);

    if (isActiveIn) {
        item->setText(1, "active");
    } else {
        item->setText(1, "inactive");
    }

    allSignalList[signalIndex]->setIsActive(isActiveIn);

    for (auto &viewer : graphBase->viewerList) {
        viewer->setSliceIndex(viewer->getSliceIndex()); // update slice indices of newly activated signals
        viewer->recalculateQImages();
    }

}

void WatershedControl::getSignalPropsFromItem(QTreeWidgetItem *item, bool &isShort, bool &isUChar, bool &isSegments,
                                           bool &isEdge,
                                           unsigned int &signalIndex) {
    isShort = getIsShort(item);
    isUChar = getIsUChar(item);
    isSegments = getIsSegments(item);
    isEdge = getIsEdge(item);
    signalIndex = getSignalIndex(item);
}

void WatershedControl::setUserNorm(QTreeWidgetItem *item) {
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);

//        std::string test item->get;
    std::cout << "index: " << signalIndex << std::endl;
    int normLower = QInputDialog::getInt(this, "Min Normalization", "Min Normalization", 0);
    int normUpper = QInputDialog::getInt(this, "Max Normalization", "Max Normalization", 255, normLower);

    QString normString = QString("%1").arg(normLower) + " " + QString("%1").arg(normUpper);
    item->setText(1, normString);

    allSignalList[signalIndex]->setNorm(normLower, normUpper);

    for (auto &viewer : graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void WatershedControl::setUserAlpha(QTreeWidgetItem *item) {
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);

    unsigned char alpha = QInputDialog::getInt(this, "Alpha", "Alpha", 255, 0, 255);

    std::string alphaString = std::to_string(alpha);
    item->setText(1, QString::fromStdString(alphaString));

    allSignalList[signalIndex]->setAlpha(alpha);

    for (auto &viewer : graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}


void WatershedControl::transferWatershedToGraph() {
    size_t signalIndexLocal; // index inside the corrosponding array, i.e. shortSignalList
    size_t signalIndexGlobal; // index inside the corrosponding treeview list

    std::unique_ptr<itkSignal<unsigned int>> pSignal2(new itkSignal<unsigned int>(pWatershed));
    signalIndexLocal = uIntSignalList.size();
    uIntImageList.push_back(pWatershed);
    uIntSignalList.push_back(std::move(pSignal2));
    signalIndexGlobal = allSignalList.size();
    itkSignalBase *pSignal = uIntSignalList.at(signalIndexLocal).get();
    allSignalList.push_back(pSignal);

    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;

    allSignalList.at(signalIndexGlobal)->setName("Watershed");
    allSignalList.at(signalIndexGlobal)->setupTreeWidget(watershedTreeWidget, signalIndexGlobal);
    allSignalList.at(signalIndexGlobal)->setLUTCategorical();
//    allSignalList.at(signalIndexGlobal)->setLUTValueToTransparent(0);

    itkSignalSegmentsGraph = (*pSegmentTypeSignalList)[signalIndexLocal].get();
    graphBase->pWorkingSegments = itkSignalSegmentsGraph;
    graphBase->pWorkingSegmentsImage = itkSignalSegmentsGraph->pImage;

    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graphBase->pGraph->constructFromVolume(itkSignalSegmentsGraph->pImage);
    //TODO: Add feature calculation
//    graphBase->pGraph->calculateNodeFeatures();
//    if(FeatureList::GroundTruthLabelComputed) graphBase->pGraph->propagateMergeFlagToEdges();
//    graphBase->pGraph->calculateEdgeFeatures();
//    graphBase->pGraph->calculateUnionFeatures();
    // set highestid/background to black
    allSignalList[signalIndexGlobal]->setLUTValueToBlack(graphBase->ignoredSegmentLabels.front());
    graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);

    graphBase->pEdgesInitialSegmentsITKSignal->setName("Edges");
    graphBase->pEdgesInitialSegmentsITKSignal->setupTreeWidget(signalTreeWidget, allSignalList.size());
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    graphBase->pEdgesInitialSegmentsITKSignal->setIsActive(false);
    int lastItemIndex = signalTreeWidget->topLevelItemCount() - 1;
    signalTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
    signalTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
    allSignalList.push_back(graphBase->pEdgesInitialSegmentsITKSignal);
    graphBase->pOrthoViewer->addSignal(graphBase->pEdgesInitialSegmentsITKSignal);
    graphBase->pOrthoViewer->setViewToMiddleOfStack();
}



bool WatershedControl::loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                              size_t &signalIndexLocalOut, size_t &signalIndexGlobalOut, bool forceShapeOfSegments,
                              bool forceSegmentDataType) {
    bool loadingWasSuccessful = false;
    if (!fileName.isEmpty()) {
        unsigned int dimension;
        getDimensionAndDataTypeOfFile(fileName, dimension, dataTypeOut);
        std::cout << "Image dimension: " << dimension << "\n";
        if (dimension == 3) {
            if (forceSegmentDataType) {
                dataTypeOut = itk::ImageIOBase::IOComponentType::UINT;
            }

            switch (dataTypeOut) {
                case itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE: {
                    throw std::logic_error("Unknown datatype");
                }

                case itk::ImageIOBase::IOComponentType::UCHAR: {
                    itk::Image<unsigned char, 3>::Pointer pImage = ITKImageLoader<unsigned char>(fileName);
                    std::unique_ptr<itkSignal<unsigned char>> pSignal2(new itkSignal<unsigned char>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uCharSignalList.size();
                        uCharImageList.push_back(pImage);
                        uCharSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uCharSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::CHAR: {
                    itk::Image<char, 3>::Pointer pImage = ITKImageLoader<char>(fileName);
                    std::unique_ptr<itkSignal<char>> pSignal2(new itkSignal<char>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = charSignalList.size();
                        charImageList.push_back(pImage);
                        charSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = charSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::USHORT: {
                    itk::Image<unsigned short, 3>::Pointer pImage = ITKImageLoader<unsigned short>(fileName);
                    std::unique_ptr<itkSignal<unsigned short>> pSignal2(new itkSignal<unsigned short>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uShortSignalList.size();
                        uShortImageList.push_back(pImage);
                        uShortSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uShortSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::SHORT: {
                    itk::Image<short, 3>::Pointer pImage = ITKImageLoader<short>(fileName);
                    std::unique_ptr<itkSignal<short>> pSignal2(new itkSignal<short>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = shortSignalList.size();
                        shortImageList.push_back(pImage);
                        shortSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = shortSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::UINT: {
                    itk::Image<unsigned int, 3>::Pointer pImage = ITKImageLoader<unsigned int>(fileName);
                    std::unique_ptr<itkSignal<unsigned int>> pSignal2(new itkSignal<unsigned int>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uIntSignalList.size();
                        uIntImageList.push_back(pImage);
                        uIntSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uIntSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::INT: {
                    itk::Image<int, 3>::Pointer pImage = ITKImageLoader<int>(fileName);
                    std::unique_ptr<itkSignal<int>> pSignal2(new itkSignal<int>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = intSignalList.size();
                        intImageList.push_back(pImage);
                        intSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = intSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::ULONG: {
                    itk::Image<unsigned long, 3>::Pointer pImage = ITKImageLoader<unsigned long>(fileName);
                    std::unique_ptr<itkSignal<unsigned long>> pSignal2(new itkSignal<unsigned long>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uLongSignalList.size();
                        uLongImageList.push_back(pImage);
                        uLongSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uLongSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::LONG: {
                    itk::Image<long, 3>::Pointer pImage = ITKImageLoader<long>(fileName);
                    std::unique_ptr<itkSignal<long>> pSignal2(new itkSignal<long>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = longSignalList.size();
                        longImageList.push_back(pImage);
                        longSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = longSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::ULONGLONG: {
                    itk::Image<unsigned long long, 3>::Pointer pImage = ITKImageLoader<unsigned long long>(fileName);
                    std::unique_ptr<itkSignal<unsigned long long>> pSignal2(new itkSignal<unsigned long long>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uLongLongSignalList.size();
                        uLongLongImageList.push_back(pImage);
                        uLongLongSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uLongLongSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::LONGLONG: {
                    itk::Image<long long, 3>::Pointer pImage = ITKImageLoader<long long>(fileName);
                    std::unique_ptr<itkSignal<long long>> pSignal2(new itkSignal<long long>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = longLongSignalList.size();
                        longLongImageList.push_back(pImage);
                        longLongSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = longLongSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::FLOAT: {
                    itk::Image<float, 3>::Pointer pImage = ITKImageLoader<float>(fileName);
                    std::unique_ptr<itkSignal<float>> pSignal2(new itkSignal<float>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = floatSignalList.size();
                        floatImageList.push_back(pImage);
                        floatSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = floatSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::DOUBLE: {
                    itk::Image<double, 3>::Pointer pImage = ITKImageLoader<double>(fileName);
                    std::unique_ptr<itkSignal<double>> pSignal2(new itkSignal<double>(pImage));
                    if (pSignal2->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = doubleSignalList.size();
                        doubleImageList.push_back(pImage);
                        doubleSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = doubleSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " " << itkSignalSegmentsGraph->getDimY() << " "
                                  << itkSignalSegmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                default: {
                    throw std::logic_error("WatershedControl::loadImage Unknown component type encountered.");
                }

            }


        } else {
            throw (std::logic_error("Image is not 3D!"));
        }
    }
    return loadingWasSuccessful;
}

void WatershedControl::addImagePressed() {
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Images"));
    if (!fileName.isEmpty()) {
        addImage(fileName);
    }
}

void WatershedControl::addBoundariesFromFile(QString fileName) {
    if (!fileName.isEmpty()) {
        std::cout << "Adding Boundaries: " << fileName.toStdString() << std::endl;
        QSettings MySettings;
        QDir CurrentDir;
        const QString DEFAULT_SAVE_DIR_KEY("default_save_dir");
        MySettings.setValue(DEFAULT_SAVE_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        std::cout << fileName.toStdString() << "\n";
        if (!fileName.isEmpty()) {
            dataType::BoundaryImageType::Pointer pBoundariesIn = ITKImageLoader<dataType::BoundaryVoxelType>(fileName);
            addBoundaries(pBoundariesIn);
        }
    }
}



void WatershedControl::addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn,
                   int fxIn, int txIn, int fyIn, int tyIn, int fzIn, int tzIn){

    useROI = true;
    fx = fxIn;
    fy = fyIn;
    fz = fzIn;
    tx = txIn;
    ty = tyIn;
    tz = tzIn;

    std::cout << "Adding Boundaries: \n";
    std::cout << "fx-tx, fy-ty, fz-tz: " << fx << "-" << tx << ", " << fy << "-" << ty << ", " <<  fz << "-" << tz << "\n";

    fxIn = fxIn < 0 ? 0 : fxIn;
    fyIn = fyIn < 0 ? 0 : fyIn;
    fzIn = fzIn < 0 ? 0 : fzIn;

    auto originalSize = pBoundariesIn->GetLargestPossibleRegion().GetSize();
    std::cout << "Original Size: " <<  originalSize[0] << " " << originalSize[1] << " " << originalSize[2] << "\n";

    int maxFx = originalSize[0] >= 2 ? originalSize[0]-2 : 0;
    int maxFy = originalSize[1] >= 2 ? originalSize[1]-2 : 0;
    int maxFz = originalSize[2] >= 2 ? originalSize[2]-2 : 0;

    int maxTx = originalSize[0];
    int maxTy = originalSize[1];
    int maxTz = originalSize[2];

    fxIn = fxIn > maxFx ? maxFx : fxIn;
    fyIn = fyIn > maxFy ? maxFy : fyIn;
    fzIn = fzIn > maxFz ? maxFz : fzIn;

    txIn = txIn > fxIn ? txIn : fxIn+2;
    tyIn = tyIn > fyIn ? tyIn : fyIn+2;
    tzIn = tzIn > fzIn ? tzIn : fzIn+2;

    txIn = txIn <= maxTx ? txIn : maxTx;
    tyIn = tyIn <= maxTy ? tyIn : maxTy;
    tzIn = tzIn <= maxTz ? tzIn : maxTz;

    std::cout << "After Boundarycheck: \n";
    std::cout << "fx-tx, fy-ty, fz-tz: " << fxIn << "-" << txIn << ", " << fyIn << "-" << tyIn << ", " << fzIn << "-"
              << tzIn << "\n";

    // update roi to the corrected values
    fx = fxIn;
    fy = fyIn;
    fz = fzIn;
    tx = txIn;
    ty = tyIn;
    tz = tzIn;


    //TODO: Check that ROI is inside the possible region
    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::BoundaryImageType, dataType::BoundaryImageType>;
    ROIExtractionFilterType::Pointer ROIExtractionFilter = ROIExtractionFilterType::New();
    ROIExtractionFilter->SetInput(pBoundariesIn);


    dataType::BoundaryImageType::IndexType pBoundariesROIIndex;
    pBoundariesROIIndex.at(0) = fxIn;
    pBoundariesROIIndex.at(1) = fyIn;
    pBoundariesROIIndex.at(2) = fzIn;

    dataType::BoundaryImageType::SizeType pBoundariesROISize;
    pBoundariesROISize.at(0) = txIn-fxIn;
    pBoundariesROISize.at(1) = tyIn-fyIn;
    pBoundariesROISize.at(2) = tzIn-fzIn;

    dataType::BoundaryImageType::RegionType pBoundariesROI(pBoundariesROIIndex, pBoundariesROISize);
    ROIExtractionFilter->SetRegionOfInterest(pBoundariesROI);

    dataType::BoundaryImageType::Pointer pBoundariesROIImage = ROIExtractionFilter->GetOutput();
    ROIExtractionFilter->Update();

    // if min and max are 0 and 1, respectively, resize to 0 ... 2, so that the threshold preview works
    dataType::BoundaryVoxelType min, max;
    auto it = itk::ImageRegionIterator<dataType::BoundaryImageType>(pBoundariesROIImage, pBoundariesROIImage->GetLargestPossibleRegion());
    min = it.Get();
    max = it.Get();
    while(!it.IsAtEnd()){
        if(it.Get() < min){
            min = it.Get();
        }
        if (it.Get() > max){
            max = it.Get();
        }
        ++it;
    }
    std::cout << "Min: " << min << " Max: " << max << "\n";

    if (min == 0 && max == 1){
        std::cout << "Resizing to 0 ... 2\n";
        it = itk::ImageRegionIterator<dataType::BoundaryImageType>(pBoundariesROIImage, pBoundariesROIImage->GetLargestPossibleRegion());
        while(!it.IsAtEnd()){
            it.Set(it.Get() * 2);
            ++it;
        }
    }

    addBoundaries(pBoundariesROIImage);
}

void WatershedControl::addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn) {
    size_t signalIndexLocal, signalIndexGlobal;
    signalIndexLocal = uShortSignalList.size();
    signalIndexGlobal = allSignalList.size();
    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;

//    std::unique_ptr<itkSignal<dataType::BoundaryVoxelType >> pBoundariesSignal(new itkSignal<dataType::BoundaryVoxelType >(pBoundariesIn));
    std::unique_ptr<itkSignalThresholdPreview<dataType::BoundaryVoxelType >> pBoundariesSignalTmp(new itkSignalThresholdPreview<dataType::BoundaryVoxelType >(pBoundariesIn));
    pBoundariesSignal = std::move(pBoundariesSignalTmp);
    //    std::unique_ptr<itkSignal<dataType::BoundaryVoxelType >> pBoundariesSignal(new itkSignalThresholdPreview<dataType::BoundaryVoxelType >(pBoundariesIn));

    uShortImageList.push_back(pBoundariesIn);
//    uShortSignalList.push_back(std::move(pBoundariesSignal));

//    itkSignalBase *pBaseSignal = uShortSignalList[signalIndexLocal].get();
    allSignalList.push_back(pBoundariesSignal.get());

    allSignalList[signalIndexGlobal]->setLUTContinuous();
    allSignalList[signalIndexGlobal]->setName("Boundaries");
    allSignalList[signalIndexGlobal]->setNorm(0,1);
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    graphBase->pOrthoViewer->addSignal(allSignalList.at(signalIndexGlobal));
    graphBase->pOrthoViewer->setViewToMiddleOfStack();
    // TODO: Make union to hold any datatype?
    pBoundaries = uShortImageList[signalIndexLocal];

    int minValue = pBoundariesSignal->minimumValue;
    int maxValue = pBoundariesSignal->maximumValue;
    thresholdValueSlider->setRange(minValue, maxValue);
    thresholdValueSlider->setValue(static_cast<int>((minValue + maxValue)/2));
}


void WatershedControl::thresholdBoundariesPressed() {
//    QProgressDialog dialog;
//    dialog.setCancelButton(0);
//    dialog.setLabelText(QString("Thresholding ..."));
//    dialog.setRange(0, 0);
//
//    graphBase->currentlyCalculating = true;
//    QFutureWatcher<void> futureWatcher;
//    QFuture<void> future = QtConcurrent::run(this, &WatershedControl::thresholdBoundaries);
//    futureWatcher.setFuture(future);
//    QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//    dialog.exec();
//    future.waitForFinished();

//    should be called from the main thread, is updating UI -- better (for later) -- separate the calculation from the UI update
    graphBase->currentlyCalculating = true;
    thresholdBoundaries();
    graphBase->currentlyCalculating = false;

}

void WatershedControl::thresholdBoundaries() {
    std::cout << "Thresholding!\n";
//    int thresholdValue = QInputDialog::getInt(this, "Threshold Value", "Threshold Value", 0);

    binaryThresholdImageFilterFloat(pBoundaries, pThresholdedMembrane, thresholdValueSlider->value());

    size_t signalIndexLocal, signalIndexGlobal;

    std::unique_ptr<itkSignal<unsigned char>> pThresholdedMembraneSignal(new itkSignal<unsigned char>(pThresholdedMembrane));
    signalIndexLocal = uCharSignalList.size();
    uCharImageList.push_back(pThresholdedMembrane);
    uCharSignalList.push_back(std::move(pThresholdedMembraneSignal));
    signalIndexGlobal = allSignalList.size();
    itkSignalBase *pSignal = uCharSignalList[signalIndexLocal].get();
    allSignalList.push_back(pSignal);

    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;

    allSignalList[signalIndexGlobal]->setName("Thresholded Boundaries");
    allSignalList[signalIndexGlobal]->setupTreeWidget(thresholdTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    graphBase->pOrthoViewer->xy->pThresholdedBoundaries = pThresholdedMembrane;
    graphBase->pOrthoViewer->xz->pThresholdedBoundaries = pThresholdedMembrane;
    graphBase->pOrthoViewer->zy->pThresholdedBoundaries = pThresholdedMembrane;
    graphBase->pOrthoViewer->xy->pThresholdedBoundariesSignal = pSignal;
    graphBase->pOrthoViewer->xz->pThresholdedBoundariesSignal = pSignal;
    graphBase->pOrthoViewer->zy->pThresholdedBoundariesSignal = pSignal;
    graphBase->pOrthoViewer->setViewToMiddleOfStack();

    // automatically set it to the next tab
    this->setCurrentIndex(1);
}

void WatershedControl::extractSeedsPressed() {
    if (pDistanceMap != nullptr) {

        QProgressDialog dialog;
        dialog.setCancelButton(0);
        dialog.setLabelText(QString("Extracting Seeds ..."));
        dialog.setRange(0, 0);
        graphBase->currentlyCalculating = true;
        QFutureWatcher<void> futureWatcher;
        QFuture<void> future = QtConcurrent::run(this, &WatershedControl::extractSeeds);
        futureWatcher.setFuture(future);
        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
        dialog.exec();
        future.waitForFinished();
        graphBase->currentlyCalculating = false;

    } else {
        QMessageBox msgBox;
        msgBox.setText("Please generate a distance map first.");
        msgBox.exec();
    }
}
void WatershedControl::extractSeeds() {
    std::cout << "Extracting Seeds!\n";

    double minimalMinimaHeight = 1;
    extractMinimaFromDistanceMap(pDistanceMap, pSeeds, minimalMinimaHeight);
    size_t signalIndexLocal, signalIndexGlobal;

    std::unique_ptr<itkSignal<unsigned int>> pSeedsSignal(new itkSignal<unsigned int>(pSeeds));
    signalIndexLocal = uIntSignalList.size();
    uIntImageList.push_back(pSeeds);
    uIntSignalList.push_back(std::move(pSeedsSignal));
    signalIndexGlobal = allSignalList.size();
    itkSignalBase *pSignal = uIntSignalList[signalIndexLocal].get();
    allSignalList.push_back(pSignal);

    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;

    allSignalList[signalIndexGlobal]->setName("Seeds");
    allSignalList[signalIndexGlobal]->setupTreeWidget(seedsTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);


    // automatically set it to the next tab
    this->setCurrentIndex(3);
}

void WatershedControl::watershedPressed() {
    if (pSeeds != nullptr) {
//        QProgressDialog dialog;
//        dialog.setCancelButton(0);
//        dialog.setLabelText(QString("Watershedding ..."));
//        dialog.setRange(0, 0);
//
//        graphBase->currentlyCalculating = true;
//        QFutureWatcher<void> futureWatcher;
//        QFuture<void> future = QtConcurrent::run(this, &WatershedControl::watershed);
//        futureWatcher.setFuture(future);
//        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//        dialog.exec();
//        future.waitForFinished();
//        graphBase->currentlyCalculating = false;
            graphBase->currentlyCalculating = true;
            watershed();
            graphBase->currentlyCalculating = false;

    } else {
        QMessageBox msgBox;
        msgBox.setText("Please generate seeds first.");
        msgBox.exec();
    }
}

void WatershedControl::watershed() {
    std::cout << "Running Watershed!\n";

    // the inverted distancemap serves as an input for the watershed algorithm
    itk::Image<float, 3>::Pointer invertedDistanceMap = itk::Image<float, 3>::New();
    invertDistanceMap(pDistanceMap, invertedDistanceMap);

    // run watershed on seeds
    runWatershed(invertedDistanceMap, pSeeds, pWatershed);

    if (checkBoxFiltering->isChecked()){
        std::cout << "Filtering Segments!\n";
        std::cout << "Minimum Size: " << sizeFilteringInput->value();
        filterSmallSegmentSeeds(pWatershed, pSeeds, sizeFilteringInput->value());
        runWatershed(invertedDistanceMap, pSeeds, pWatershed);
    }


    insertBoundariesIntoWatershed(pWatershed, pThresholdedMembrane);

    transferWatershedToGraph();

    // automatically set it to the next tab
    this->setCurrentIndex(4);
}

void WatershedControl::exportSegmentsPressed() {
    if (graphBase->pWorkingSegmentsImage != nullptr) {

//        QProgressDialog dialog;
//        dialog.setCancelButton(0);
//        dialog.setLabelText(QString("Exporting Segments ..."));
//        dialog.setRange(0, 0);
//
//        graphBase->currentlyCalculating = true;
//        QFutureWatcher<void> futureWatcher;
//        QFuture<void> future = QtConcurrent::run(this, &WatershedControl::exportSegments);
//        futureWatcher.setFuture(future);
//        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//        dialog.exec();
//        future.waitForFinished();
//        graphBase->currentlyCalculating = false;

        graphBase->currentlyCalculating = true;
        exportSegments();
        graphBase->currentlyCalculating = false;

    } else {
        QMessageBox msgBox;
        msgBox.setText("Please generate a watershed first.");
        msgBox.exec();
    }
}

void WatershedControl::exportSegments() {
    std::cout << "Exporting Segments!\n";
    if (useROI) {
        std::cout << "Padding exported image to match original shape!\n";

        // create a new image with fitting size, spacing, and origin
        dataType::SegmentsImageType::Pointer paddedWorkingSegmentsImage = dataType::SegmentsImageType::New();
        paddedWorkingSegmentsImage->SetRegions(
                linkedSignalControl->graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
        paddedWorkingSegmentsImage->SetSpacing(linkedSignalControl->graphBase->pWorkingSegmentsImage->GetSpacing());
        paddedWorkingSegmentsImage->SetOrigin(linkedSignalControl->graphBase->pWorkingSegmentsImage->GetOrigin());
        paddedWorkingSegmentsImage->Allocate(true);

        // paste in the created watershed
        using PasteImageFilterType = itk::PasteImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
        PasteImageFilterType::Pointer pasteImageFilter = PasteImageFilterType::New();
        pasteImageFilter->SetSourceImage(graphBase->pWorkingSegmentsImage);
        pasteImageFilter->SetSourceRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
        pasteImageFilter->SetDestinationImage(paddedWorkingSegmentsImage);

        // set in correct starting index for the paste
        dataType::SegmentsImageType::IndexType destinationIndex;
        std::cout << "destination index: " << fx << " " << fy << " " << fz << " \n";
        destinationIndex.at(0) = fx;
        destinationIndex.at(1) = fy;
        destinationIndex.at(2) = fz;
        pasteImageFilter->SetDestinationIndex(destinationIndex);

        paddedWorkingSegmentsImage = pasteImageFilter->GetOutput();
        pasteImageFilter->Update();

        linkedSignalControl->receiveNewRefinementWatershed(paddedWorkingSegmentsImage);
    } else {
        linkedSignalControl->receiveNewRefinementWatershed(graphBase->pWorkingSegmentsImage);
    }

    emit sendClosingSignal();
}


void WatershedControl::calculateDistanceMapPressed() {
    if (pThresholdedMembrane != nullptr) {
//        QProgressDialog dialog;
//        dialog.setCancelButton(0);
//        dialog.setLabelText(QString("Calculating Distance Map ..."));
//        dialog.setRange(0, 0);
//
//        graphBase->currentlyCalculating = true;
//        QFutureWatcher<void> futureWatcher;
//        QFuture<void> future = QtConcurrent::run(this, &WatershedControl::calculateDistanceMap);
//        futureWatcher.setFuture(future);
//        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//        dialog.exec();
//        future.waitForFinished();
//        graphBase->currentlyCalculating = false;

        graphBase->currentlyCalculating = true;
        calculateDistanceMap();
        graphBase->currentlyCalculating = false;
    } else {
        QMessageBox msgBox;
        msgBox.setText("Please threshold the boundaries first.");
        msgBox.exec();
    }
}
void WatershedControl::calculateDistanceMap() {
    std::cout << "Calculating DistanceMap!\n";

//    setBoundariesToValue(pThresholdedMembrane, 1);

    double distanceMapSmoothingVariance = 0;
    generateDistanceMap(pThresholdedMembrane, pDistanceMap, distanceMapSmoothingVariance);

    size_t signalIndexLocal, signalIndexGlobal;

    std::unique_ptr<itkSignal<float>> pDistanceMapSignal(new itkSignal<float>(pDistanceMap));
    signalIndexLocal = floatSignalList.size();
    floatImageList.push_back(pDistanceMap);
    floatSignalList.push_back(std::move(pDistanceMapSignal));
    signalIndexGlobal = allSignalList.size();
    itkSignalBase *pSignal = floatSignalList[signalIndexLocal].get();
    allSignalList.push_back(pSignal);

    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;

    allSignalList[signalIndexGlobal]->setName("Distance Map");
    allSignalList[signalIndexGlobal]->setupTreeWidget(distanceMapTreeWidget, signalIndexGlobal);
    graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);

    // automatically set it to the next tab
    this->setCurrentIndex(2);
}


unsigned int WatershedControl::getSignalIndex(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    for (int i = 0; i < baseItem->childCount(); ++i) {
        if (baseItem->child(i)->text(0) == "SignalIndex") {
            return baseItem->child(i)->text(1).toInt();
        }
    }
    throw std::logic_error("signal index not found!");
    return 0;
}

bool WatershedControl::getIsUChar(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    for (int i = 0; i < baseItem->childCount(); ++i) {
        if (baseItem->child(i)->text(0) == "data type") {
            return baseItem->child(i)->text(1) == "char";
        }
    }
    return false;
}

bool WatershedControl::getIsSegments(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    return baseItem->text(0) == "Segments";
}

bool WatershedControl::getIsEdge(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    return baseItem->text(0) == "Edges";
}

bool WatershedControl::getIsShort(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    for (int i = 0; i < baseItem->childCount(); ++i) {
        if (baseItem->child(i)->text(0) == "data type") {
            return baseItem->child(i)->text(1) == "short";
        }
    }
    return false;
}

