#include "SignalControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"
#include "MainWindowWatershedControl.h"
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include <QFileDialog>
#include <QColorDialog>
#include <QInputDialog>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QtWidgets/QProgressDialog>
#include <QStandardPaths>
#include <src/qtUtils/QImageSelectionRadioButtons.h>
#include <QSettings>
#include <QThread>
#include <QApplication>

SignalControl::~SignalControl() {

}


void SignalControl::addImage(QString fileName, QString displayedName) {
    graphBase->currentlyCalculating = true;
    std::cout << "Adding file: " << fileName.toStdString() << std::endl;
    if (!fileName.isEmpty()) {
        itk::ImageIOBase::IOComponentType dataType;
        size_t signalIndexLocal;
        size_t signalIndexGlobal;
        bool loadSuccessFull = false;
        try {
            loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal);
        } catch (itk::ExceptionObject &e) {
            std::cout << "Error loading image: " << fileName.toStdString() << std::endl;
            std::cout << "Exception caught!" << std::endl;
            std::cout << e << std::endl;
        }
        if (loadSuccessFull) {
            globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
            allSignalList[signalIndexGlobal]->setLUTContinuous();
            if (displayedName == "") {
                displayedName = QFileInfo(fileName).baseName();
            }
            allSignalList[signalIndexGlobal]->setName(displayedName);
            allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
            graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        }
    }
    graphBase->currentlyCalculating = false;

}

SignalControl::SignalControl(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent, bool verboseIn) {
    setParent(parent);
    graphBase = graphBaseIn;
    verbose = verboseIn;
    allSignalList.reserve(10);
    segmentsGraph = nullptr;
    DEFAULT_SAVE_DIR = "";
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);


#ifdef SEGMENTSHORT
    pSegmentTypeSignalList = &shortSignalList;
    pSegmentTypeImageList = &shortImageList;
#endif

#ifdef SEGMENTUINT
    pSegmentTypeSignalList = &uIntSignalList;
    pSegmentTypeImageList = &uIntImageList;
#endif



//    signalControlLayout = new QVBoxLayout();
//    setLayout(signalControlLayout);
    this->setTabPosition(QTabWidget::South);

    setupSignalTreeWidget();
    setupProbabilityTreeWidget();
    setupRefinementWatershedTreeWidget();
    setupSegmentationTreeWidget();

}

void SignalControl::setupSignalTreeWidget() {
    QWidget *signalWidget = new QWidget();
    auto *signalWidgetLayout = new QVBoxLayout();
    signalWidget->setLayout(signalWidgetLayout);

    signalTreeWidget = new QTreeWidgetWithDragAndDrop();
    signalTreeWidget->setFocusPolicy(Qt::NoFocus);
    signalTreeWidget->setColumnCount(2);
    signalTreeWidget->setHeaderLabels({"Name", "Properties"});
    signalTreeWidget->header()->setStretchLastSection(false);
    signalTreeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
//    signalControlLayout->addWidget(signalTreeWidget);
    signalWidgetLayout->addWidget(signalTreeWidget);
//    connect(signalTreeWidget, SIGNAL(urlDropped(QString)), this, SLOT(addImage(QString)));
    connect(signalTreeWidget, SIGNAL(urlDropped(QString)), this, SLOT(loadFileFromDragAndDropTriggered(QString)));

    signalInputButtonsWidget = new QWidget();
    signalInputButtonsLayout = new QGridLayout();
    signalInputButtonsWidget->setLayout(signalInputButtonsLayout);
    addSignalButton = new QPushButton("Add Image File");
    addSegmentsButton = new QPushButton("Add Segments File");
    signalInputButtonsLayout->addWidget(addSignalButton, 0, 0);
    signalInputButtonsLayout->addWidget(addSegmentsButton, 1, 0);

//    signalControlLayout->addWidget(signalInputButtonsWidget);
    signalWidgetLayout->addWidget(signalInputButtonsWidget);

//    QTreeWidget::itemDoubleClicked()
    connect(addSegmentsButton, SIGNAL (clicked()), this, SLOT (addSegmentsPressed()));
    connect(addSignalButton, SIGNAL (clicked()), this, SLOT (addImagePressed()));

    connect(signalTreeWidget, SIGNAL (itemDoubleClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeDoubleClicked(QTreeWidgetItem * , int)));
    connect(signalTreeWidget, SIGNAL (itemClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeClicked(QTreeWidgetItem * , int)));

    this->addTab(signalWidget, "Overlays");
}


void SignalControl::setupProbabilityTreeWidget() {

    QWidget *probabilityWidget = new QWidget();
    auto *probabilityWidgetLayout = new QVBoxLayout();
    probabilityWidget->setLayout(probabilityWidgetLayout);

    probabilityTreeWidget = new QTreeWidget();
    probabilityTreeWidget->setFocusPolicy(Qt::NoFocus);
    probabilityTreeWidget->setColumnCount(2);
    probabilityTreeWidget->setHeaderLabels({"Name", "Properties"});
    probabilityTreeWidget->header()->setStretchLastSection(false);
    probabilityTreeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    probabilityWidgetLayout->addWidget(probabilityTreeWidget);

    probabilityButtonWidget = new QWidget();
    probabilityWidgetLayout->addWidget(probabilityButtonWidget);
    probabilityButtonWidgetLayout = new QGridLayout();
    probabilityButtonWidget->setLayout(probabilityButtonWidgetLayout);

    addMembraneProbabilityButton = new QPushButton("Add Boundaries");
    probabilityButtonWidgetLayout->addWidget(addMembraneProbabilityButton, 0, 0);

    runWatershedButton = new QPushButton("Run Watershed");
    probabilityButtonWidgetLayout->addWidget(runWatershedButton, 1, 0);

    selectROIRefinementButton = new QPushButton("Turn ROI-Selection WS On");
    probabilityButtonWidgetLayout->addWidget(selectROIRefinementButton, 2, 0);
    connect(selectROIRefinementButton, SIGNAL(clicked()), this, SLOT(selectROIRefinementPressed()));


    connect(addMembraneProbabilityButton, SIGNAL(clicked()), this, SLOT(loadMembraneProbabilityPressed()));
    connect(runWatershedButton, SIGNAL(clicked()), this, SLOT(runWatershed()));


    connect(probabilityTreeWidget, SIGNAL (itemDoubleClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeDoubleClicked(QTreeWidgetItem * , int)));
    connect(probabilityTreeWidget, SIGNAL (itemClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeClicked(QTreeWidgetItem * , int)));

    this->addTab(probabilityWidget, "Probabilities");
}


void SignalControl::setupRefinementWatershedTreeWidget() {
    QWidget *refinementWidget = new QWidget();
    auto *refinementWidgetLayout = new QVBoxLayout();
    refinementWidget->setLayout(refinementWidgetLayout);

    refinementWatershedTreeWidget = new QTreeWidgetWithDragAndDrop();
    refinementWatershedTreeWidget->setFocusPolicy(Qt::NoFocus);
    refinementWatershedTreeWidget->setColumnCount(2);
    refinementWatershedTreeWidget->setHeaderLabels({"Name", "Properties"});
    refinementWatershedTreeWidget->header()->setStretchLastSection(false);
    refinementWatershedTreeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
//    signalControlLayout->addWidget(refinementWatershedTreeWidget);
    refinementWidgetLayout->addWidget(refinementWatershedTreeWidget);
    connect(refinementWatershedTreeWidget, SIGNAL(urlDropped(QString)), this, SLOT(addRefinementWatershed(QString)));


    refinementWatershedInputButtonsWidget = new QWidget();
    refinementWatershedButtonLayout = new QGridLayout();
    refinementWatershedInputButtonsWidget->setLayout(refinementWatershedButtonLayout);
    addRefinementWatershedButton = new QPushButton("Add Refinement Watershed");
    mergeWithRefinementWatershedButton = new QPushButton("Merge with Refinement Watershed");
//    replaceSegmentByPositionButton = new QPushButton("Refine by Position (r)");
//    replaceSegmentBySegmentButton = new QPushButton("Refine by Segment (R)");
    setIdToTransparentInRefinementWSButton = new QPushButton("Set Id Transparent in RefinementWS");
    refinementWatershedButtonLayout->addWidget(addRefinementWatershedButton, 0, 0);
//    refinementWatershedButtonLayout->addWidget(replaceSegmentByPositionButton, 1, 0);
//    refinementWatershedButtonLayout->addWidget(replaceSegmentBySegmentButton, 2, 0);
    refinementWatershedButtonLayout->addWidget(mergeWithRefinementWatershedButton, 1, 0);
    refinementWatershedButtonLayout->addWidget(setIdToTransparentInRefinementWSButton, 2, 0);
    refinementWidgetLayout->addWidget(refinementWatershedInputButtonsWidget);


    connect(mergeWithRefinementWatershedButton, SIGNAL(clicked()), this,
            SLOT(mergeSegmentsWithRefinementWatershedClicked()));
//    signalControlLayout->addWidget(refinementWatershedInputButtonsWidget);
    connect(addRefinementWatershedButton, SIGNAL(clicked()), this, SLOT(addRefinementWatershedPressed()));
    connect(setIdToTransparentInRefinementWSButton, SIGNAL(clicked()), this, SLOT(setIdToTransparentInRefinementWS()));

    connect(refinementWatershedTreeWidget, SIGNAL (itemDoubleClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeDoubleClicked(QTreeWidgetItem * , int)));
    connect(refinementWatershedTreeWidget, SIGNAL (itemClicked(QTreeWidgetItem * , int)), this,
            SLOT(watershedClicked(QTreeWidgetItem * , int)));

    this->addTab(refinementWidget, "Refinements");

}

void SignalControl::loadFileFromDragAndDropTriggered(QString fileName) {
    std::cout << "SignalControl::loadFileFromDragAndDropTriggered: " << fileName.toStdString() << '\n';
    imageSelectionButtonWidget = new QImageSelectionRadioButtons(fileName, this);
    imageSelectionButtonWidget->show();
    connect(imageSelectionButtonWidget, &QImageSelectionRadioButtons::sendButton, this,
            &SignalControl::loadFileFromDragAndDrop, Qt::QueuedConnection);
    imageSelectionButtonWidget->exec();
}

void SignalControl::loadFileFromDragAndDrop(QString fileName, QString choiceOfImage) {
    imageSelectionButtonWidget->close();
    QMessageBox dialog;
//    QProgressDialog dialog;
//    dialog.setCancelButton(0);
//    dialog.setRange(0, 0);
    dialog.setWindowTitle("Loading");
    dialog.setText(QString("Loading file ..."));
    dialog.show();
//    QFutureWatcher<void> futureWatcher;
//    QFuture<void> future;
    bool validChoice = true;
    bool skipDialog = false; //TODO: unknown-to-me bug that appears when adding boundaries before graph. this should not be necessary?
    if (((choiceOfImage != "Segments") && (choiceOfImage != "Boundary")) &&
        graphBase->pWorkingSegmentsImage == nullptr) {
        QMessageBox msgBox;
        msgBox.setText("Please add the Segments first.");
        msgBox.exec();
    } else if (choiceOfImage == "Segments") {
        graphBase->pGraph->askForBackgroundStrategy();
//        dialog.setLabelText(QString("Setting up graph ..."));
//        future = QtConcurrent::run(this, &SignalControl::addSegmentsGraph, fileName);
        // run this from main thread
        addSegmentsGraph(fileName);
    } else if (choiceOfImage == "Image") {
//        dialog.setLabelText(QString("Loading image ..."));
//        future = QtConcurrent::run(this, &SignalControl::addImage, fileName, QString(""));
        addImage(fileName, QString(""));
    } else if (choiceOfImage == "Boundary") {
//        dialog.setLabelText(QString("Loading boundary ..."));
        if (graphBase->pWorkingSegmentsImage == nullptr) {
            // in this case user has option to construct empty graph from boundary image files
            loadMembraneProbability(fileName);
            skipDialog = true;
        } else {
//            future = QtConcurrent::run(this, &SignalControl::loadMembraneProbability, fileName, QString(""));
            loadMembraneProbability(fileName, QString(""));
        }
    } else if (choiceOfImage == "Refinement Watershed") {
//        dialog.setLabelText(QString("Loading refinement watershed ..."));
//        future = QtConcurrent::run(this, &SignalControl::addRefinementWatershed, fileName, QString(""));
        addRefinementWatershed(fileName, QString(""));
    } else if (choiceOfImage == "Segmentation") {
//        dialog.setLabelText(QString("Loading segmentation ..."));
//        future = QtConcurrent::run(this, &SignalControl::loadSegmentationVolume, fileName, QString(""));
        SignalControl::loadSegmentationVolume(fileName, QString(""));
    } else {
        validChoice = false;
        std::cout << "Unknown choice of image: " << choiceOfImage.toStdString() << "\n";
    }

    dialog.close();


//    if (validChoice && !skipDialog) {
//        futureWatcher.setFuture(future);
//        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//        dialog.exec();
//        future.waitForFinished();
//    }
}


void SignalControl::selectROIRefinementPressed() {
    if (selectROIRefinementButton->text() == "Turn ROI-Selection WS On") {
        selectROIRefinementButton->setText("Turn ROI-Selection WS Off");
    } else {
        selectROIRefinementButton->setText("Turn ROI-Selection WS On");
    }

    graphBase->pOrthoViewer->xy->toggleROISelectonModeIsActive();
    graphBase->pOrthoViewer->xz->toggleROISelectonModeIsActive();
    graphBase->pOrthoViewer->zy->toggleROISelectonModeIsActive();
}

void SignalControl::transferSegmentsWithVolume() {
    bool ok;
    double volumeThreshold = QInputDialog::getDouble(this, tr("Transfer with volume"),
                                                     tr("Transfer all Segments with a volume greater than:"),
                                                     50000, 0, 1000000, 0, &ok);
    if (!ok) {
        return;
    }

    graphBase->pGraph->transferSegmentsWithVolumeCriterion(volumeThreshold);
    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void SignalControl::transferAllSegments() {
    graphBase->pGraph->transferSegmentsWithVolumeCriterion(1);
    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}


void SignalControl::transferSegmentsWithRefinementWS() {
    graphBase->pGraph->transferSegmentsWithRefinementOverlap();
    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}


void SignalControl::setupSegmentationTreeWidget() {
    // funcitonality: toggle visibility of segmentations
    // choose the active segmentation to transfer/delete to and from
    // rename segmentations

    // current active segmentation is implemented by a pointer set in graphbase
    // i.e., if the active segmentation is changed,
    // the pointer in graphbase should be pointing to the new segmentation volume

    QWidget *segmentationWidget = new QWidget();
    auto *segmentationWidgetLayout = new QVBoxLayout();
    segmentationWidget->setLayout(segmentationWidgetLayout);

    segmentationTreeWidget = new QTreeWidget();
    segmentationTreeWidget->setFocusPolicy(Qt::NoFocus);
    segmentationTreeWidget->setColumnCount(2);
    segmentationTreeWidget->setHeaderLabels({"Name", "Properties"});
    segmentationTreeWidget->header()->setStretchLastSection(false);
    segmentationTreeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    segmentationWidgetLayout->addWidget(segmentationTreeWidget);

    segmentationButtonWidget = new QWidget();
    segmentationButtonWidgetGridLayout = new QGridLayout();
    segmentationButtonWidget->setLayout(segmentationButtonWidgetGridLayout);

    addSegmentationButton = new QPushButton("New Segmentation Volume");
    exportSegmentationButton = new QPushButton("Export Selected Segmentation");
    loadSegmentationButton = new QPushButton("Load Segmentation");
    togglePaintBrushButton = new QPushButton("Turn Paintmode On");
    setPaintIdButton = new QPushButton("Set Paint Id");
    transferSegmentsWithVolumeButton = new QPushButton("Transfer Segments with Volume");
    transferSegmentsWithRefinementButton = new QPushButton("Transfer Segments with RefinementWS Overlap");
    transferAllSegmentsButton = new QPushButton("Transfer all Segments");
    // better: transfer with cell probability?

    segmentationButtonWidgetGridLayout->addWidget(addSegmentationButton, 0, 0);
    segmentationButtonWidgetGridLayout->addWidget(exportSegmentationButton, 1, 0);
    segmentationButtonWidgetGridLayout->addWidget(loadSegmentationButton, 2, 0);
    segmentationButtonWidgetGridLayout->addWidget(togglePaintBrushButton, 3, 0);
    segmentationButtonWidgetGridLayout->addWidget(setPaintIdButton, 4, 0);
    segmentationButtonWidgetGridLayout->addWidget(transferSegmentsWithVolumeButton, 5, 0);
    segmentationButtonWidgetGridLayout->addWidget(transferAllSegmentsButton, 6, 0);
    segmentationButtonWidgetGridLayout->addWidget(transferSegmentsWithRefinementButton, 7, 0);

    segmentationWidgetLayout->addWidget(segmentationButtonWidget);

    connect(addSegmentationButton, SIGNAL(clicked()), this, SLOT(createNewSegmentationVolume()));
    connect(loadSegmentationButton, SIGNAL(clicked()), this, SLOT(loadSegmentationVolumePressed()));
    connect(exportSegmentationButton, SIGNAL(clicked()), this, SLOT(exportSelectedSegmentation()));
    connect(togglePaintBrushButton, SIGNAL(clicked()), this, SLOT(togglePaintMode()));
    connect(setPaintIdButton, SIGNAL(clicked()), this, SLOT(setPaintId()));
    connect(transferSegmentsWithVolumeButton, SIGNAL(clicked()), this, SLOT(transferSegmentsWithVolume()));
    connect(transferSegmentsWithRefinementButton, SIGNAL(clicked()), this, SLOT(transferSegmentsWithRefinementWS()));
    connect(transferAllSegmentsButton, SIGNAL(clicked()), this, SLOT(transferAllSegments()));


    connect(segmentationTreeWidget, SIGNAL (itemDoubleClicked(QTreeWidgetItem * , int)), this,
            SLOT(treeDoubleClicked(QTreeWidgetItem * , int)));
    connect(segmentationTreeWidget, SIGNAL (itemClicked(QTreeWidgetItem * , int)), this,
            SLOT(segmentationClicked(QTreeWidgetItem * , int)));

    this->addTab(segmentationWidget, "Segmentations");
}


void SignalControl::treeDoubleClicked(QTreeWidgetItem *item, int) {
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


void SignalControl::treeClicked(QTreeWidgetItem *item, int) {
    std::string itemText = item->text(1).toStdString();
    if ((itemText == "active") || (itemText == "inactive")) {
        if (itemText == "inactive" && (item->checkState(0) == Qt::CheckState::Checked)) {
            setIsActive(item, true);
        } else if (itemText == "active" && (item->checkState(0) == Qt::CheckState::Unchecked)) {
            setIsActive(item, false);
        }
    }
}

void SignalControl::watershedClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);

    // set the focused watershed as refinement watershed
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);
    if (verbose) {
        std::cout << "Setting local WS Refinement index to: " << globalToLocalMapping[signalIndex] << std::endl;
    }
    graphBase->pRefinementWatershed = (*pSegmentTypeImageList)[globalToLocalMapping[signalIndex]];
    graphBase->pRefinementWatershedSignal = (*pSegmentTypeSignalList)[globalToLocalMapping[signalIndex]].get();
}


void SignalControl::setIdToTransparentInRefinementWS() {
    if (graphBase->pRefinementWatershed != nullptr) {
        int inputVal = QInputDialog::getInt(this, "Value to set transparent", "Value to set transparent", 0, 0);
        std::cout << "Set " << inputVal << " to transparent in selected refinement watershed.\n";
        graphBase->pRefinementWatershedSignal->setLUTValueToTransparent(inputVal);
        for (auto &viewer: graphBase->viewerList) {
            viewer->recalculateQImages();
        }
    }
}

void SignalControl::segmentationClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);

    // set the focused watershed as refinement watershedƒ
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);
    if (verbose) {
        std::cout << "Setting active segmentation volume to: " << globalToLocalMapping[signalIndex] << std::endl;
    }
    graphBase->pSelectedSegmentation = (*pSegmentTypeImageList)[globalToLocalMapping[signalIndex]];
    graphBase->pSelectedSegmentationSignal = (*pSegmentTypeSignalList)[globalToLocalMapping[signalIndex]].get();
    dataType::SegmentIdType maxId = graphBase->pGraph->getLargestIdInSegmentVolume(graphBase->pSelectedSegmentation);
    std::cout << "Max Id in selected segmentation: " << maxId << "\n";
    graphBase->selectedSegmentationMaxSegmentId = maxId;
}

void SignalControl::setUserColor(QTreeWidgetItem *item) {
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

    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void SignalControl::setDescription(QTreeWidgetItem *item) {
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


void SignalControl::exportSelectedSegmentation() {
    // TODO: decide computer-wide vs application instance wide usage of settings
    QSettings MySettings;
    const QString DEFAULT_SAVE_DIR_KEY("default_save_dir");
    //    QString default_save_dir = MySettings.value(DEFAULT_SAVE_DIR_KEY).toString();
    QString path =
            QFileDialog::getSaveFileName(this, "Export Segmentation", DEFAULT_SAVE_DIR,
                                         "Same Type as Watershed!!! (*.nrrd *.shlat *.uilat)");
    std::cout << path.toStdString() << "\n";
    if (!path.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_SAVE_DIR_KEY, CurrentDir.absoluteFilePath(path));
        try {
            graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(graphBase->pSelectedSegmentation,
                                                                           path.toStdString());
        } catch (itk::ExceptionObject &err) {
            std::cerr << "ExceptionObject caught during writing!" << std::endl;
            std::cerr << err << std::endl;
        }

    }
}

void SignalControl::setPaintId(){
    if (graphBase->pWorkingSegmentsImage == nullptr) {
        QMessageBox msgBox;
        msgBox.setText("OrthoViewer not initialised yet. Returning.");
        msgBox.exec();
        return;
    }
    auto maxId = std::numeric_limits<dataType::SegmentIdType>::max();
    //    cast to int max
    if (maxId > std::numeric_limits<int>::max()) {
        maxId = std::numeric_limits<int>::max();
    }
    bool ok;
    int paintId = QInputDialog::getInt(this, "Set Paint Id", "Set Paint Id",
                                       graphBase->pGraph->getNextFreeId(graphBase->pSelectedSegmentation),
                                       0, static_cast<int>(maxId), 1, &ok);
    if (!ok) {
        return;
    }
    std::cout << "Setting paint id to: " << paintId << std::endl;
    graphBase->pOrthoViewer->xy->labelOfClickedSegmentation = paintId;
    graphBase->pOrthoViewer->zy->labelOfClickedSegmentation = paintId;
    graphBase->pOrthoViewer->xz->labelOfClickedSegmentation = paintId;
    graphBase->pOrthoViewer->xy->setPaintId(paintId);
    graphBase->pOrthoViewer->zy->setPaintId(paintId);
    graphBase->pOrthoViewer->xz->setPaintId(paintId);
}

void SignalControl::togglePaintMode() {
    if (togglePaintBrushButton->text() == "Turn Paintmode On") {
        togglePaintBrushButton->setText("Turn Paintmode Off");
    } else {
        togglePaintBrushButton->setText("Turn Paintmode On");
    }

    graphBase->pOrthoViewer->xy->togglePaintMode();
    graphBase->pOrthoViewer->zy->togglePaintMode();
    graphBase->pOrthoViewer->xz->togglePaintMode();
}


void SignalControl::setIsActive(QTreeWidgetItem *item, bool isActiveIn) {
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

    for (auto &viewer: graphBase->viewerList) {
        viewer->setSliceIndex(viewer->getSliceIndex()); // update slice indices of newly activated signals
        viewer->recalculateQImages();
    }

}

void SignalControl::getSignalPropsFromItem(QTreeWidgetItem *item, bool &isShort, bool &isUChar, bool &isSegments,
                                           bool &isEdge,
                                           unsigned int &signalIndex) {
    isShort = getIsShort(item);
    isUChar = getIsUChar(item);
    isSegments = getIsSegments(item);
    isEdge = getIsEdge(item);
    signalIndex = getSignalIndex(item);
}

void SignalControl::setUserNorm(QTreeWidgetItem *item) {
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

    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void SignalControl::setUserAlpha(QTreeWidgetItem *item) {
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);

    unsigned char alpha = QInputDialog::getInt(this, "Alpha", "Alpha", 255, 0, 255);

    std::string alphaString = std::to_string(alpha);
    item->setText(1, QString::fromStdString(alphaString));

    allSignalList[signalIndex]->setAlpha(alpha);

    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}


void SignalControl::addSegmentsGraph(QString &fileName) {
    std::cout << "SignalControl created in thread: "
              << QThread::currentThread()->objectName().toStdString()
              << " (ID: " << QThread::currentThreadId()
              << ")" << std::endl;
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    graphBase->currentlyCalculating = true;

    itk::ImageIOBase::IOComponentType dataType;
    size_t signalIndexLocal;
    size_t signalIndexGlobal;
    bool loadSuccessFull;
    try {
        loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal, false, true);
    } catch (const std::exception &e) {
        std::cout << "Exception in loadImage: " << e.what() << std::endl;
    }

    if (loadSuccessFull) {
        initializeGraph(signalIndexLocal, signalIndexGlobal);
    } else {
        QMessageBox::critical(this, "Error", "Failed to load the image.");
    }

    graphBase->currentlyCalculating = false;
}
//
//void SignalControl::addSegmentsGraph(QString &fileName) {
//    graphBase->currentlyCalculating = true;
//    itk::ImageIOBase::IOComponentType dataType;
//    size_t signalIndexLocal; // index inside the corrosponding array, i.e. shortSignalList
//    size_t signalIndexGlobal; // index inside the corrosponding treeview list
//    bool loadSuccessFull = false;
//    try {
//        loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal, false, true);
//    } catch (const std::exception& e){
//        std::cout << e.what() << std::endl;
//    }
//    std::cout << "Done loading file!\n";
//
//    if (loadSuccessFull) {
//        initializeGraph(signalIndexLocal, signalIndexGlobal);
//    }
//    std::cout << "Done add segment function!\n";
//    graphBase->currentlyCalculating = false;
//}

void SignalControl::addEmptySegmentsFromBoundary() {
    std::cout << "Adding empty segments/graph volume based on boundary volume\n";
    // 3 indices:
    // signalIndexLocal = index inside <dtype> array
    // signalIndexGlobal = index inside allsignal array
    GraphSegmentImageType::Pointer pImage = dataType::SegmentsImageType::New();
    pImage->SetRegions(graphBase->pSelectedBoundary->GetLargestPossibleRegion());
    pImage->SetSpacing(graphBase->pSelectedBoundary->GetSpacing());
    pImage->SetOrigin(graphBase->pSelectedBoundary->GetOrigin());
    pImage->Allocate(true);

    //TODO: Make uinbtimagelist etc dependent on whatever is on datatype/segmentidtype
    size_t signalIndexLocal = (*pSegmentTypeImageList).size();
    (*pSegmentTypeImageList).push_back(pImage);

    std::unique_ptr<itkSignal<GraphSegmentType>> pSignal2(new itkSignal<GraphSegmentType>(pImage));
    (*pSegmentTypeSignalList).push_back(std::move(pSignal2));

    size_t signalIndexGlobal = allSignalList.size();
    itkSignalBase *pSignal = (*pSegmentTypeSignalList)[signalIndexLocal].get();
    allSignalList.push_back(pSignal);

    initializeGraph(signalIndexLocal, signalIndexGlobal);

    std::cout << "Done add segment function!\n";
}

void SignalControl::initializeGraph(size_t signalIndexLocal, size_t signalIndexGlobal) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
    GraphSegmentImageType::Pointer pSegments = (*pSegmentTypeImageList)[signalIndexLocal];
    segmentsGraph = (*pSegmentTypeSignalList)[signalIndexLocal].get();
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName("Segments");
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    std::cout << "Done setup tree!\n";

    graphBase->pWorkingSegments = segmentsGraph;
    graphBase->pWorkingSegmentsImage = segmentsGraph->pImage;

    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);


    graphBase->pGraph->constructFromVolume(segmentsGraph->pImage);

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

//    Important: Make sure painting threads are called from the main thread!
//    graphBase->pOrthoViewer->setViewToMiddleOfStack();
    QMetaObject::invokeMethod(graphBase->pOrthoViewer, "setViewToMiddleOfStack", Qt::QueuedConnection);


    createNewSegmentationVolume();
}

void SignalControl::runWatershed() {
    std::cout << "Running Watershed Widget" << "\n";
    if (graphBase->pSelectedBoundary == nullptr) {
        QMessageBox msgBox;
        msgBox.setText("Please add boundaries first.");
        msgBox.exec();
    } else {
        auto *myMainWindow = new MainWindowWatershedControl();
        myMainWindow->setLinkedSignalControl(this);
        if (graphBase->ROI_set) {
            myMainWindow->myWatershedControl->addBoundaries(graphBase->pSelectedBoundary,
                                                            graphBase->ROI_fx, graphBase->ROI_tx,
                                                            graphBase->ROI_fy, graphBase->ROI_ty,
                                                            graphBase->ROI_fz, graphBase->ROI_tz);
            myMainWindow->show();
        } else {
            myMainWindow->myWatershedControl->addBoundaries(graphBase->pSelectedBoundary);
            myMainWindow->show();

        }

        if (selectROIRefinementButton->text() == "Turn ROI-Selection WS Off") {
            selectROIRefinementButton->setText("Turn ROI-Selection WS On");
            graphBase->pOrthoViewer->xy->turnROISelectonModeInactive();
            graphBase->pOrthoViewer->xz->turnROISelectonModeInactive();
            graphBase->pOrthoViewer->zy->turnROISelectonModeInactive();
        }
    }
}

void SignalControl::receiveNewRefinementWatershed(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage) {
    size_t signalIndexLocal, signalIndexGlobal;
    bool loadSuccessFull;
    loadSuccessFull = insertImageSegmenttype(pImage, signalIndexLocal, signalIndexGlobal);
    if (loadSuccessFull) {
        globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
        allSignalList[signalIndexGlobal]->setLUTCategorical();
        allSignalList[signalIndexGlobal]->setName("Refined Watershed");
        allSignalList[signalIndexGlobal]->setupTreeWidget(refinementWatershedTreeWidget, signalIndexGlobal);
        allSignalList[signalIndexGlobal]->setIsActive(false);
        allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
        int lastItemIndex = refinementWatershedTreeWidget->topLevelItemCount() - 1;
        refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
        refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
        refinementWatershedTreeWidget->setCurrentItem(refinementWatershedTreeWidget->topLevelItem(lastItemIndex));
        graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        graphBase->pRefinementWatershed = (*pSegmentTypeImageList).at(signalIndexLocal);
        graphBase->pRefinementWatershedSignal = (*pSegmentTypeSignalList).at(signalIndexLocal).get();

        // Set ROI to currently set watershed ROI
        // this is used to prevent out-of-ROI refinements through user input
        if (graphBase->ROI_set) {
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_set = true;
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_fx = graphBase->ROI_fx;
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_fy = graphBase->ROI_fy;
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_fz = graphBase->ROI_fz;
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_tx = graphBase->ROI_tx;
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_ty = graphBase->ROI_ty;
            (*pSegmentTypeSignalList).at(signalIndexLocal)->ROI_tz = graphBase->ROI_tz;
        }
    }
}


void SignalControl::loadMembraneProbability(QString fileName, QString displayedName) {
    std::cout << "Loading boundaries: " << fileName.toStdString() << "\n";
    if (!fileName.isEmpty()) {
        std::cout << "Adding Boundaries: " << fileName.toStdString() << std::endl;

        bool segmentsAreNotAdded = graphBase->pWorkingSegmentsImage == nullptr;
        QMessageBox::StandardButton reply = QMessageBox::Yes;
        if (segmentsAreNotAdded) {
            reply = QMessageBox::question(this, "No Segmentation Added",
                                          "No Segments added. Do you want to create empty segments based on the added boundary image?",
                                          QMessageBox::Yes | QMessageBox::No);
        }
        if (!segmentsAreNotAdded || (reply == QMessageBox::Yes)) {
            itk::ImageIOBase::IOComponentType dataType;
            size_t signalIndexLocal;
            size_t signalIndexGlobal;
            bool forceShapeOfSegments;
            if (graphBase->pWorkingSegmentsImage == nullptr) {
                forceShapeOfSegments = false;
            } else {
                forceShapeOfSegments = true;
            }
            bool forceSegmentDataType = true;
            itk::ImageIOBase::IOComponentType forcedDataType = itk::ImageIOBase::IOComponentType::USHORT;
            // TODO: support more datatypes. maybe sth like a union can do the trick here?
            bool loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal,
                                             forceShapeOfSegments, forceSegmentDataType, forcedDataType);

            if (loadSuccessFull) {
                graphBase->pSelectedBoundary = uShortImageList.at(signalIndexLocal);

                if (graphBase->pWorkingSegmentsImage == nullptr) {
                    //TODO:add prompt asking if it should create the seg?
                    addEmptySegmentsFromBoundary();
                }

                std::cout << "datatype: " << dataType << "\n";
                globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
                allSignalList[signalIndexGlobal]->setLUTContinuous();
                if (displayedName == "") {
                    displayedName = QFileInfo(fileName).baseName();
                }
                allSignalList[signalIndexGlobal]->setName(displayedName);
                allSignalList[signalIndexGlobal]->setupTreeWidget(probabilityTreeWidget, signalIndexGlobal);
                graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);

            }
        }

    }
//    graphBase->currentlyCalculating = false;
}


void SignalControl::createNewSegmentationVolume() {
    std::cout << "Adding Segmentation Volume\n";
    // 3 indices:
    // signalIndexLocal = index inside <dtype> array
    // signalIndexGlobal = index inside allsignal array
    GraphSegmentImageType::Pointer pImage = dataType::SegmentsImageType::New();
    pImage->SetRegions(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
    pImage->SetSpacing(graphBase->pWorkingSegmentsImage->GetSpacing());
    pImage->SetOrigin(graphBase->pWorkingSegmentsImage->GetOrigin());
    pImage->Allocate(true);

    //TODO: Make uinbtimagelist etc dependent on whatever is on datatype/segmentidtype
    size_t signalIndexLocal = (*pSegmentTypeImageList).size();
    (*pSegmentTypeImageList).push_back(pImage);
    graphBase->pSelectedSegmentation = pImage;

    std::unique_ptr<itkSignal<GraphSegmentType>> pSignal2(new itkSignal<GraphSegmentType>(pImage));
    (*pSegmentTypeSignalList).push_back(std::move(pSignal2));
    graphBase->pSelectedSegmentationSignal = (*pSegmentTypeSignalList)[signalIndexLocal].get();

    size_t signalIndexGlobal = allSignalList.size();
    itkSignalBase *pSignal = (*pSegmentTypeSignalList)[signalIndexLocal].get();
    allSignalList.push_back(pSignal);


    globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;

    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    allSignalList[signalIndexGlobal]->setName("Segmentation");
    allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);

    graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
}

void SignalControl::loadSegmentationVolumePressed() {
    std::cout << "Loading Segmentation Volume\n";
    // 3 indices:
    // signalIndexLocal = index inside <dtype> array
    // signalIndexGlobal = index inside allsignal array

    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();

    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Images"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        loadSegmentationVolume(fileName); //TODO: add Progressbar?
    }
}

void SignalControl::loadSegmentationVolume(QString fileName, QString displayName) {
    graphBase->currentlyCalculating = true;
    if (!fileName.isEmpty()) {
        itk::ImageIOBase::IOComponentType dataType;
        size_t signalIndexLocal;
        size_t signalIndexGlobal;
        bool forceShapeOfSegments = graphBase->pWorkingSegmentsImage != nullptr;
        bool forceSegmentDataTypeUInt = true;
        bool loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal, forceShapeOfSegments,
                                         forceSegmentDataTypeUInt);
        if (loadSuccessFull) {
            globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
            allSignalList[signalIndexGlobal]->setLUTCategorical();

            if (displayName == "") {
                displayName = QFileInfo(fileName).baseName();
            }
            allSignalList[signalIndexGlobal]->setName(displayName);

            allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
            allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);
            allSignalList[signalIndexGlobal]->setIsActive(true);

            graphBase->pSelectedSegmentation = (*pSegmentTypeImageList)[signalIndexLocal];
            graphBase->pSelectedSegmentationSignal = (*pSegmentTypeSignalList)[signalIndexLocal].get();
            dataType::SegmentIdType maxId = graphBase->pGraph->getLargestIdInSegmentVolume(
                    graphBase->pSelectedSegmentation);
            std::cout << "Max Id in selected segmentation: " << maxId << "\n";
            graphBase->selectedSegmentationMaxSegmentId = maxId;

            graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        }
    }
    graphBase->currentlyCalculating = false;
}


void SignalControl::addSegmentsPressed() {
    if (graphBase->pWorkingSegmentsImage != nullptr) {
        QMessageBox msgBox;
        msgBox.setText("Segments were already added, please restart SegmentPuzzler for a new project.");
        msgBox.exec();
        return;
    }
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Segments"), default_load_dir);
    if (!fileName.isEmpty()) {
        graphBase->pGraph->askForBackgroundStrategy();

        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
//        QProgressDialog dialog;
//        dialog.setCancelButton(0);
//        dialog.setLabelText(QString("Setting up graph ..."));
//        dialog.setRange(0, 0);
        //TODO: Running it concurrently gives raise to the following error:
        // QPainter::begin: Paint device returned engine == 0, type: 3
        // QPainter::setPen: Painter not active
//        QFutureWatcher<void> futureWatcher;
//        QFuture<void> future = QtConcurrent::run(this, &SignalControl::addSegmentsGraph, fileName);
//        futureWatcher.setFuture(future);
//        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//        dialog.exec();
//        future.waitForFinished();

//        invke with main thread not
        QMetaObject::invokeMethod(this, "addSegmentsGraph", Qt::QueuedConnection, Q_ARG(QString, fileName));

    }
    signalInputButtonsLayout->removeWidget(addSegmentsButton);
}


void SignalControl::addRefinementWatershedPressed() {
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Segments"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
//        QProgressDialog dialog;
//        dialog.setCancelButton(0);
//        dialog.setLabelText(QString("Adding Refinement Watershed ..."));
//        dialog.setMinimumWidth(QFontMetrics(dialog.font()).horizontalAdvance(dialog.labelText()) + 50);
//        dialog.setRange(0, 0);
//        dialog.exec();
        QMessageBox dialog;
        dialog.setText("Adding Refinement Watershed ...");
        dialog.show();


//        QFutureWatcher<void> futureWatcher;
//        QFuture<void> future = QtConcurrent::run(this, &SignalControl::addRefinementWatershed, fileName, QString(""));
//        futureWatcher.setFuture(future);
//        QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//        future.waitForFinished();

        addRefinementWatershed(fileName);
        dialog.close();
    }



//        addRefinementWatershed(fileName);
}

void SignalControl::mergeSegmentsWithRefinementWatershedClicked() {
    graphBase->pGraph->mergeSegmentsWithRefinementWatershed();
    for (auto &viewer: graphBase->viewerList) {
        viewer->recalculateQImages();
    }
}

void SignalControl::addRefinementWatershed(QString fileName, QString displayedName) {
    graphBase->currentlyCalculating = true;
    if (!fileName.isEmpty()) {
        itk::ImageIOBase::IOComponentType dataType;
        size_t signalIndexLocal;
        size_t signalIndexGlobal;
        bool loadSuccessFull = loadImage(fileName, dataType, signalIndexLocal, signalIndexGlobal, true, true);
        if (loadSuccessFull) {
            globalToLocalMapping[signalIndexGlobal] = signalIndexLocal;
            allSignalList[signalIndexGlobal]->setLUTCategorical();
            if (displayedName == "") {
                displayedName = QFileInfo(fileName).baseName();
            }
            allSignalList[signalIndexGlobal]->setName(displayedName);
            allSignalList[signalIndexGlobal]->setupTreeWidget(refinementWatershedTreeWidget, signalIndexGlobal);
            allSignalList[signalIndexGlobal]->setIsActive(false);
            int lastItemIndex = refinementWatershedTreeWidget->topLevelItemCount() - 1;
            refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
            refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
            refinementWatershedTreeWidget->setCurrentItem(refinementWatershedTreeWidget->topLevelItem(lastItemIndex));
            graphBase->pOrthoViewer->addSignal(allSignalList[signalIndexGlobal]);
            graphBase->pRefinementWatershed = (*pSegmentTypeImageList).at(signalIndexLocal);
            graphBase->pRefinementWatershedSignal = (*pSegmentTypeSignalList).at(signalIndexLocal).get();
        }
    }
    graphBase->currentlyCalculating = false;

}

bool SignalControl::insertImageSegmenttype(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage,
                                           size_t &signalIndexLocalOut, size_t &signalIndexGlobalOut,
                                           bool forceShapeOfSegments) {
    bool loadingWasSuccessful = false;
    std::unique_ptr<itkSignal<unsigned int>> pSignal2(new itkSignal<unsigned int>(pImage));
    pSignal2->isShapeMatched(segmentsGraph);
    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
        signalIndexLocalOut = uIntSignalList.size();
        uIntImageList.push_back(pImage);
        uIntSignalList.push_back(std::move(pSignal2));
        signalIndexGlobalOut = allSignalList.size();
        itkSignalBase *pSignal = uIntSignalList[signalIndexLocalOut].get();
        allSignalList.push_back(pSignal);
        loadingWasSuccessful = true;
    } else {
        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                  << segmentsGraph->getDimZ() << "]\n";
        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                  << pSignal2->getDimZ() << "]\n";
        std::cout << "Dimension mismatch! Image is not added.\n";
    }
    return loadingWasSuccessful;
}

bool SignalControl::loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                              size_t &signalIndexLocalOut, size_t &signalIndexGlobalOut, bool forceShapeOfSegments,
                              bool forceSegmentDataTypeUInt, itk::ImageIOBase::IOComponentType forcedDataType) {
    bool loadingWasSuccessful = false;
    if (!fileName.isEmpty()) {
        unsigned int dimension;
        std::cout << "Reading: " << fileName.toStdString() << "\n";
        getDimensionAndDataTypeOfFile(fileName, dimension, dataTypeOut);
        std::cout << "Image dimension: " << dimension << "\n";

        QSettings MySettings;
        QDir CurrentDir;
        const QString DEFAULT_SAVE_DIR_KEY("default_save_dir");
        MySettings.setValue(DEFAULT_SAVE_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        DEFAULT_SAVE_DIR = CurrentDir.absoluteFilePath(fileName);
        std::cout << fileName.toStdString() << "\n";

        bool forceOnly3D = false;
        if ((dimension == 3) | !forceOnly3D) {
            if (forceSegmentDataTypeUInt) {
                dataTypeOut = forcedDataType;
            }

            switch (dataTypeOut) {
                case itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE: {
                    throw std::logic_error("Unknown datatype");
                }

                case itk::ImageIOBase::IOComponentType::UCHAR: {
                    itk::Image<unsigned char, 3>::Pointer pImage = ITKImageLoader<unsigned char>(fileName);
                    std::unique_ptr<itkSignal<unsigned char>> pSignal2(new itkSignal<unsigned char>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uCharSignalList.size();
                        uCharImageList.push_back(pImage);
                        uCharSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uCharSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::CHAR: {
                    itk::Image<char, 3>::Pointer pImage = ITKImageLoader<char>(fileName);
                    std::unique_ptr<itkSignal<char>> pSignal2(new itkSignal<char>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = charSignalList.size();
                        charImageList.push_back(pImage);
                        charSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = charSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::USHORT: {
                    itk::Image<unsigned short, 3>::Pointer pImage = ITKImageLoader<unsigned short>(fileName);
                    std::unique_ptr<itkSignal<unsigned short>> pSignal2(new itkSignal<unsigned short>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uShortSignalList.size();
                        uShortImageList.push_back(pImage);
                        uShortSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uShortSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::SHORT: {
                    itk::Image<short, 3>::Pointer pImage = ITKImageLoader<short>(fileName);
                    std::unique_ptr<itkSignal<short>> pSignal2(new itkSignal<short>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = shortSignalList.size();
                        shortImageList.push_back(pImage);
                        shortSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = shortSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::UINT: {
                    itk::Image<unsigned int, 3>::Pointer pImage = ITKImageLoader<unsigned int>(fileName);
                    std::unique_ptr<itkSignal<unsigned int>> pSignal2(new itkSignal<unsigned int>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uIntSignalList.size();
                        uIntImageList.push_back(pImage);
                        uIntSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uIntSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::INT: {
                    itk::Image<int, 3>::Pointer pImage = ITKImageLoader<int>(fileName);
                    std::unique_ptr<itkSignal<int>> pSignal2(new itkSignal<int>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = intSignalList.size();
                        intImageList.push_back(pImage);
                        intSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = intSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::ULONG: {
                    itk::Image<unsigned long, 3>::Pointer pImage = ITKImageLoader<unsigned long>(fileName);
                    std::unique_ptr<itkSignal<unsigned long>> pSignal2(new itkSignal<unsigned long>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uLongSignalList.size();
                        uLongImageList.push_back(pImage);
                        uLongSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uLongSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::LONG: {
                    itk::Image<long, 3>::Pointer pImage = ITKImageLoader<long>(fileName);
                    std::unique_ptr<itkSignal<long>> pSignal2(new itkSignal<long>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = longSignalList.size();
                        longImageList.push_back(pImage);
                        longSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = longSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::ULONGLONG: {
                    itk::Image<unsigned long long, 3>::Pointer pImage = ITKImageLoader<unsigned long long>(fileName);
                    std::unique_ptr<itkSignal<unsigned long long>> pSignal2(new itkSignal<unsigned long long>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = uLongLongSignalList.size();
                        uLongLongImageList.push_back(pImage);
                        uLongLongSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = uLongLongSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::LONGLONG: {
                    itk::Image<long long, 3>::Pointer pImage = ITKImageLoader<long long>(fileName);
                    std::unique_ptr<itkSignal<long long>> pSignal2(new itkSignal<long long>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = longLongSignalList.size();
                        longLongImageList.push_back(pImage);
                        longLongSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = longLongSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::FLOAT: {
                    itk::Image<float, 3>::Pointer pImage = ITKImageLoader<float>(fileName);
                    std::unique_ptr<itkSignal<float>> pSignal2(new itkSignal<float>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = floatSignalList.size();
                        floatImageList.push_back(pImage);
                        floatSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = floatSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                case itk::ImageIOBase::IOComponentType::DOUBLE: {
                    itk::Image<double, 3>::Pointer pImage = ITKImageLoader<double>(fileName);
                    std::unique_ptr<itkSignal<double>> pSignal2(new itkSignal<double>(pImage));
                    if (pSignal2->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
                        signalIndexLocalOut = doubleSignalList.size();
                        doubleImageList.push_back(pImage);
                        doubleSignalList.push_back(std::move(pSignal2));
                        signalIndexGlobalOut = allSignalList.size();
                        itkSignalBase *pSignal = doubleSignalList[signalIndexLocalOut].get();
                        allSignalList.push_back(pSignal);
                        loadingWasSuccessful = true;
                    } else {
                        std::cout << "Segments: [" << segmentsGraph->getDimX() << " " << segmentsGraph->getDimY() << " "
                                  << segmentsGraph->getDimZ() << "]\n";
                        std::cout << "Segments: [" << pSignal2->getDimX() << " " << pSignal2->getDimY() << " "
                                  << pSignal2->getDimZ() << "]\n";
                        std::cout << "Dimension mismatch! Image is not added.\n";
                    }
                    break;
                }

                default: {
                    throw std::logic_error("SignalControl::loadImage Unknown component type encountered.");
                }
            }


        } else {
            throw (std::logic_error("Image is not 3D!"));
        }
    }
    return loadingWasSuccessful;
}

void SignalControl::addImagePressed() {

    if (graphBase->pWorkingSegmentsImage == nullptr) {
        QMessageBox msgBox;
        msgBox.setText("Please add the segmentation first.");
        msgBox.exec();
    } else {
        QSettings MySettings;
        const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
        QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
        QString fileName = QFileDialog::getOpenFileName(this,
                                                        tr("Open Images"), default_load_dir);
        if (!fileName.isEmpty()) {
            QDir CurrentDir;
            MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
//            QProgressDialog dialog;
//            dialog.setCancelButton(0);
//            dialog.setLabelText(QString("Loading Image ..."));
//            dialog.setRange(0, 0);
//            dialog.exec();
            QMessageBox dialog;
            dialog.setText("Loading Image ...");
            dialog.show();


//            QFutureWatcher<void> futureWatcher;
//            QFuture<void> future = QtConcurrent::run(this, &SignalControl::addImage, fileName, QString(""));
//            futureWatcher.setFuture(future);
//            QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//            future.waitForFinished();

            addImage(fileName);
            dialog.close();

        }
    }
}


void SignalControl::loadMembraneProbabilityPressed() {
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Images"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        if (graphBase->pWorkingSegmentsImage == nullptr) {
            //TODO: this doesnt run with the progressbar for some unknown-to-me reason. fix that i guess ...
            loadMembraneProbability(fileName);
        } else {
//            QProgressDialog dialog;
//            dialog.setCancelButton(0);
//            dialog.setLabelText(QString("Loading Boundaries ..."));
//            dialog.setRange(0, 0);
//            dialog.exec();
            QMessageBox dialog;
            dialog.setText("Loading Boundaries ...");
            dialog.show();

//            QFutureWatcher<void> futureWatcher;
//            QFuture<void> future = QtConcurrent::run(this, &SignalControl::loadMembraneProbability, fileName,
//                                                     QString(""));
//            futureWatcher.setFuture(future);
//            QObject::connect(&futureWatcher, SIGNAL(finished()), &dialog, SLOT(cancel()));
//            future.waitForFinished();

            loadMembraneProbability(fileName);
            dialog.close();
        }
    }
}

unsigned int SignalControl::getSignalIndex(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    for (int i = 0; i < baseItem->childCount(); ++i) {
        if (baseItem->child(i)->text(0) == "SignalIndex") {
            return baseItem->child(i)->text(1).toInt();
        }
    }
    throw std::logic_error("signal index not found!");
    return 0;
}

bool SignalControl::getIsUChar(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    for (int i = 0; i < baseItem->childCount(); ++i) {
        if (baseItem->child(i)->text(0) == "data type") {
            return baseItem->child(i)->text(1) == "char";
        }
    }
    return false;
}

bool SignalControl::getIsSegments(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    return baseItem->text(0) == "Segments";
}

bool SignalControl::getIsEdge(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    return baseItem->text(0) == "Edges";
}

bool SignalControl::getIsShort(QTreeWidgetItem *item) {
    QTreeWidgetItem *baseItem = (item->parent() != nullptr) ? item->parent() : item;
    for (int i = 0; i < baseItem->childCount(); ++i) {
        if (baseItem->child(i)->text(0) == "data type") {
            return baseItem->child(i)->text(1) == "short";
        }
    }
    return false;
}

