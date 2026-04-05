#include "SignalControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"
#include "MainWindowWatershedControl.h"
#include "src/viewers/OrthoViewer.h"
#include "src/qtUtils/TaskRunner.h"
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
#include <QTimer>
#include <clocale>

SignalControl::~SignalControl() {

}

void SignalControl::setGuiBusy(bool busy) {
    signalTreeWidget->setEnabled(!busy);
    probabilityTreeWidget->setEnabled(!busy);
    segmentationTreeWidget->setEnabled(!busy);
    refinementWatershedTreeWidget->setEnabled(!busy);

    addSignalButton->setEnabled(!busy);
    addSegmentsButton->setEnabled(!busy);
    addRefinementWatershedButton->setEnabled(!busy);
    mergeWithRefinementWatershedButton->setEnabled(!busy);
    setIdToTransparentInRefinementWSButton->setEnabled(!busy);
    addSegmentationButton->setEnabled(!busy);
    exportSegmentationButton->setEnabled(!busy);
    loadSegmentationButton->setEnabled(!busy);
    togglePaintBrushButton->setEnabled(!busy);
    setPaintIdButton->setEnabled(!busy);
    transferSegmentsWithVolumeButton->setEnabled(!busy);
    transferSegmentsWithRefinementButton->setEnabled(!busy);
    transferAllSegmentsButton->setEnabled(!busy);
    addMembraneProbabilityButton->setEnabled(!busy);
    runWatershedButton->setEnabled(!busy);
    selectROIRefinementButton->setEnabled(!busy);
}

void SignalControl::refreshViewers() {
    orthoViewer->refreshViewers();
}

QString SignalControl::resolvedDisplayName(const QString &fileName, const QString &displayedName) const {
    if (!displayedName.isEmpty()) {
        return displayedName;
    }
    return QFileInfo(fileName).baseName();
}

void SignalControl::invokeLoadCallbackLater(LoadCallback then, LoadResult result) {
    if (!then) {
        return;
    }

    // Queue chained loads until the next event-loop turn so TaskRunner has
    // finished its busy->false transition before the next task starts.
    QTimer::singleShot(0, this, [then = std::move(then), result = std::move(result)]() mutable {
        then(std::move(result));
    });
}

SignalControl::LoadedImageData SignalControl::loadImageData(QString fileName,
                                                            bool forceSegmentDataTypeUInt,
                                                            itk::ImageIOBase::IOComponentType forcedDataType) {
    LoadedImageData loadedImage;
    if (fileName.isEmpty()) {
        return loadedImage;
    }

    unsigned int dimension = 0;
    std::cout << "Reading: " << fileName.toStdString() << "\n";
    getDimensionAndDataTypeOfFile(fileName, dimension, loadedImage.dataType);
    std::cout << "Image dimension: " << dimension << "\n";

    if (dimension != 3) {
        throw std::logic_error("Image is not 3D!");
    }

    if (forceSegmentDataTypeUInt) {
        loadedImage.dataType = forcedDataType;
    }

    switch (loadedImage.dataType) {
        case itk::ImageIOBase::IOComponentType::UCHAR:
            loadedImage.image = ITKImageLoader<unsigned char>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::CHAR:
            loadedImage.image = ITKImageLoader<char>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::USHORT:
            loadedImage.image = ITKImageLoader<unsigned short>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::SHORT:
            loadedImage.image = ITKImageLoader<short>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::UINT:
            loadedImage.image = ITKImageLoader<unsigned int>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::INT:
            loadedImage.image = ITKImageLoader<int>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::ULONG:
            loadedImage.image = ITKImageLoader<unsigned long>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::LONG:
            loadedImage.image = ITKImageLoader<long>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::ULONGLONG:
            loadedImage.image = ITKImageLoader<unsigned long long>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::LONGLONG:
            loadedImage.image = ITKImageLoader<long long>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::FLOAT:
            loadedImage.image = ITKImageLoader<float>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::DOUBLE:
            loadedImage.image = ITKImageLoader<double>(fileName).GetPointer();
            break;
        case itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE:
        default:
            throw std::logic_error("SignalControl::loadImageData unknown component type encountered.");
    }

    return loadedImage;
}

bool SignalControl::insertLoadedImage(const LoadedImageData &loadedImage,
                                      size_t &signalIndexGlobalOut,
                                      bool forceShapeOfSegments) {
    switch (loadedImage.dataType) {
        case itk::ImageIOBase::IOComponentType::UCHAR:
            return insertTypedImage<unsigned char>(
                dynamic_cast<itk::Image<unsigned char, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::CHAR:
            return insertTypedImage<char>(
                dynamic_cast<itk::Image<char, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::USHORT:
            return insertTypedImage<unsigned short>(
                dynamic_cast<itk::Image<unsigned short, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::SHORT:
            return insertTypedImage<short>(
                dynamic_cast<itk::Image<short, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::UINT:
            return insertTypedImage<unsigned int>(
                dynamic_cast<itk::Image<unsigned int, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::INT:
            return insertTypedImage<int>(
                dynamic_cast<itk::Image<int, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::ULONG:
            return insertTypedImage<unsigned long>(
                dynamic_cast<itk::Image<unsigned long, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::LONG:
            return insertTypedImage<long>(
                dynamic_cast<itk::Image<long, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::ULONGLONG:
            return insertTypedImage<unsigned long long>(
                dynamic_cast<itk::Image<unsigned long long, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::LONGLONG:
            return insertTypedImage<long long>(
                dynamic_cast<itk::Image<long long, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::FLOAT:
            return insertTypedImage<float>(
                dynamic_cast<itk::Image<float, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::DOUBLE:
            return insertTypedImage<double>(
                dynamic_cast<itk::Image<double, 3> *>(loadedImage.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments);
        case itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE:
        default:
            throw std::logic_error("SignalControl::insertLoadedImage unknown component type encountered.");
    }
}

void SignalControl::registerImageSignal(size_t signalIndexGlobal, const QString &name) {
    allSignalList[signalIndexGlobal]->setLUTContinuous();
    allSignalList[signalIndexGlobal]->setName(name);
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
}

void SignalControl::registerSegmentationSignal(size_t signalIndexGlobal, const QString &name) {
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName(name);
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setIsActive(true);

    graphBase->pSelectedSegmentation = dynamic_cast<GraphSegmentImageType *>(
        allSignalList[signalIndexGlobal]->getImageBase().GetPointer());
    graphBase->pSelectedSegmentationSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(
        allSignalList[signalIndexGlobal]);
    graphBase->selectedSegmentationMaxSegmentId =
        graphBase->pGraph->getLargestIdInSegmentVolume(graphBase->pSelectedSegmentation);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
}

void SignalControl::registerBoundarySignal(size_t signalIndexGlobal, const QString &name) {
    graphBase->pSelectedBoundary = dynamic_cast<dataType::BoundaryImageType *>(
        allSignalList[signalIndexGlobal]->getImageBase().GetPointer());
    allSignalList[signalIndexGlobal]->setLUTContinuous();
    allSignalList[signalIndexGlobal]->setName(name);
    allSignalList[signalIndexGlobal]->setupTreeWidget(probabilityTreeWidget, signalIndexGlobal);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
}

void SignalControl::registerRefinementSignal(size_t signalIndexGlobal, const QString &name) {
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName(name);
    allSignalList[signalIndexGlobal]->setupTreeWidget(refinementWatershedTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setIsActive(false);
    int lastItemIndex = refinementWatershedTreeWidget->topLevelItemCount() - 1;
    refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
    refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
    refinementWatershedTreeWidget->setCurrentItem(refinementWatershedTreeWidget->topLevelItem(lastItemIndex));
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    graphBase->pRefinementWatershed = dynamic_cast<GraphSegmentImageType *>(
        allSignalList[signalIndexGlobal]->getImageBase().GetPointer());
    graphBase->pRefinementWatershedSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(
        allSignalList[signalIndexGlobal]);
}

void SignalControl::registerSegmentsGraphSignal(size_t signalIndexGlobal) {
    segmentsGraph = allSignalList[signalIndexGlobal];
    auto *typedSegmentsSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(segmentsGraph);
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName("Segments");
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);

    graphBase->pWorkingSegments = typedSegmentsSignal;
    graphBase->pWorkingSegmentsImage = typedSegmentsSignal->pImage;

    allSignalList[signalIndexGlobal]->setLUTValueToBlack(graphBase->ignoredSegmentLabels.front());
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);

    graphBase->pEdgesInitialSegmentsITKSignal->setName("Edges");
    graphBase->pEdgesInitialSegmentsITKSignal->setupTreeWidget(signalTreeWidget, allSignalList.size());
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    graphBase->pEdgesInitialSegmentsITKSignal->setIsActive(false);
    int lastItemIndex = signalTreeWidget->topLevelItemCount() - 1;
    signalTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
    signalTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
    allSignalList.push_back(graphBase->pEdgesInitialSegmentsITKSignal);
    orthoViewer->addSignal(graphBase->pEdgesInitialSegmentsITKSignal);

    orthoViewer->setViewToMiddleOfStack();
    createNewSegmentationVolume();
}

void SignalControl::addImageAsync(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    taskRunner->run(
        [this, fileName]() { return loadImageData(fileName); },
        [this, fileName, displayedName, then = std::move(then)](LoadedImageData loadedImage) mutable {
            size_t signalIndexGlobal = 0;
            bool ok = insertLoadedImage(loadedImage, signalIndexGlobal, true);
            if (ok) {
                registerImageSignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName));
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::loadSegmentationVolumeAsync(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    taskRunner->run(
        [fileName]() mutable { return ITKImageLoader<GraphSegmentType>(fileName); },
        [this, fileName, displayedName, then = std::move(then)](GraphSegmentImageType::Pointer pImage) mutable {
            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(pImage, signalIndexGlobal, graphBase->pWorkingSegmentsImage != nullptr);
            if (ok) {
                registerSegmentationSignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName));
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::addRefinementWatershedAsync(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    taskRunner->run(
        [fileName]() mutable { return ITKImageLoader<GraphSegmentType>(fileName); },
        [this, fileName, displayedName, then = std::move(then)](GraphSegmentImageType::Pointer pImage) mutable {
            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(pImage, signalIndexGlobal, true);
            if (ok) {
                registerRefinementSignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName));
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::addSegmentsGraphAsync(QString fileName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    taskRunner->run(
        [this, fileName]() mutable {
            // Modifies graphBase on the worker thread. Safe only because
            // one task runs at a time and the GUI is disabled (no concurrent access).
            auto pImage = ITKImageLoader<GraphSegmentType>(fileName);
            graphBase->ignoredSegmentLabels.clear();
            graphBase->edgeStatus.clear();
            graphBase->colorLookUpEdgesStatus.clear();
            graphBase->pWorkingSegmentsImage = pImage;
            graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
            graphBase->pGraph->constructFromVolume(pImage);
            return pImage;
        },
        [this, then = std::move(then)](GraphSegmentImageType::Pointer pImage) mutable {
            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(pImage, signalIndexGlobal, false);
            if (ok) {
                registerSegmentsGraphSignal(signalIndexGlobal);
            } else {
                QMessageBox::critical(this, "Error", "Failed to load the image.");
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::loadMembraneProbabilityAsync(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const bool createEmptySegments = graphBase->pWorkingSegmentsImage == nullptr;
    taskRunner->run(
        [this, fileName, createEmptySegments]() mutable {
            BoundaryLoadResult result;
            result.boundaryImage = ITKImageLoader<dataType::BoundaryVoxelType>(fileName);
            if (createEmptySegments) {
                result.emptySegmentsImage = dataType::SegmentsImageType::New();
                result.emptySegmentsImage->SetRegions(result.boundaryImage->GetLargestPossibleRegion());
                result.emptySegmentsImage->SetSpacing(result.boundaryImage->GetSpacing());
                result.emptySegmentsImage->SetOrigin(result.boundaryImage->GetOrigin());
                result.emptySegmentsImage->Allocate(true);

                // Modifies graphBase on the worker thread. Safe only because
                // one task runs at a time and the GUI is disabled (no concurrent access).
                graphBase->ignoredSegmentLabels.clear();
                graphBase->edgeStatus.clear();
                graphBase->colorLookUpEdgesStatus.clear();
                graphBase->pWorkingSegmentsImage = result.emptySegmentsImage;
                graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
                graphBase->pGraph->constructFromVolume(result.emptySegmentsImage);
                result.createdEmptySegments = true;
            }
            return result;
        },
        [this, fileName, displayedName, createEmptySegments, then = std::move(then)](BoundaryLoadResult result) mutable {
            size_t signalIndexGlobal = 0;
            bool ok = insertTypedImage<dataType::BoundaryVoxelType>(
                result.boundaryImage,
                signalIndexGlobal,
                !createEmptySegments);
            if (ok) {
                if (result.createdEmptySegments) {
                    size_t segmentIndexGlobal = 0;
                    const bool insertedSegments = insertImageSegmenttype(result.emptySegmentsImage, segmentIndexGlobal, false);
                    if (insertedSegments) {
                        registerSegmentsGraphSignal(segmentIndexGlobal);
                    }
                }
                registerBoundarySignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName));
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::addImage(QString fileName, QString displayedName) {
    std::cout << "Adding file: " << fileName.toStdString() << std::endl;
    addImageAsync(fileName, displayedName);
}

SignalControl::SignalControl(std::shared_ptr<GraphBase> graphBaseIn,
                             OrthoViewer *orthoViewerIn,
                             TaskRunner *taskRunnerIn,
                             QWidget *parent,
                             bool verboseIn) {
    setParent(parent);
    graphBase = graphBaseIn;
    orthoViewer = orthoViewerIn;
    taskRunner = taskRunnerIn;
    verbose = verboseIn;
    allSignalList.reserve(10);
    segmentsGraph = nullptr;
    DEFAULT_SAVE_DIR = "";
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);



//    signalControlLayout = new QVBoxLayout();
//    setLayout(signalControlLayout);
    this->setTabPosition(QTabWidget::South);

    setupSignalTreeWidget();
    setupProbabilityTreeWidget();
    setupRefinementWatershedTreeWidget();
    setupSegmentationTreeWidget();

    connect(taskRunner, &TaskRunner::busyChanged, this, &SignalControl::setGuiBusy);

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
    connect(signalTreeWidget, &QTreeWidgetWithDragAndDrop::urlDropped, this, &SignalControl::loadFileFromDragAndDropTriggered);

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
    connect(addSegmentsButton, &QPushButton::clicked, this, &SignalControl::addSegmentsPressed);
    connect(addSignalButton, &QPushButton::clicked, this, &SignalControl::addImagePressed);

    connect(signalTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(signalTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::treeClicked);

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
    connect(selectROIRefinementButton, &QPushButton::clicked, this, &SignalControl::selectROIRefinementPressed);


    connect(addMembraneProbabilityButton, &QPushButton::clicked, this, &SignalControl::loadMembraneProbabilityPressed);
    connect(runWatershedButton, &QPushButton::clicked, this, &SignalControl::runWatershed);


    connect(probabilityTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(probabilityTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::treeClicked);

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
    connect(refinementWatershedTreeWidget, &QTreeWidgetWithDragAndDrop::urlDropped, this, qOverload<QString>(&SignalControl::addRefinementWatershed));


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


    connect(mergeWithRefinementWatershedButton, &QPushButton::clicked, this, &SignalControl::mergeSegmentsWithRefinementWatershedClicked);
//    signalControlLayout->addWidget(refinementWatershedInputButtonsWidget);
    connect(addRefinementWatershedButton, &QPushButton::clicked, this, &SignalControl::addRefinementWatershedPressed);
    connect(setIdToTransparentInRefinementWSButton, &QPushButton::clicked, this, &SignalControl::setIdToTransparentInRefinementWS);

    connect(refinementWatershedTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(refinementWatershedTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::watershedClicked);

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
    if (((choiceOfImage != "Segments") && (choiceOfImage != "Boundary")) &&
        graphBase->pWorkingSegmentsImage == nullptr) {
        QMessageBox msgBox;
        msgBox.setText("Please add the Segments first.");
        msgBox.exec();
    } else if (choiceOfImage == "Segments") {
        askForBackgroundStrategy();
        addSegmentsGraph(fileName);
    } else if (choiceOfImage == "Image") {
        addImage(fileName, QString(""));
    } else if (choiceOfImage == "Boundary") {
        if (graphBase->pWorkingSegmentsImage == nullptr) {
            // in this case user has option to construct empty graph from boundary image files
            loadMembraneProbability(fileName);
        } else {
            loadMembraneProbability(fileName, QString(""));
        }
    } else if (choiceOfImage == "Refinement Watershed") {
        addRefinementWatershed(fileName, QString(""));
    } else if (choiceOfImage == "Segmentation") {
        SignalControl::loadSegmentationVolume(fileName, QString(""));
    } else {
        std::cout << "Unknown choice of image: " << choiceOfImage.toStdString() << "\n";
    }
}


void SignalControl::selectROIRefinementPressed() {
    if (selectROIRefinementButton->text() == "Turn ROI-Selection WS On") {
        selectROIRefinementButton->setText("Turn ROI-Selection WS Off");
    } else {
        selectROIRefinementButton->setText("Turn ROI-Selection WS On");
    }

    orthoViewer->xy->toggleROISelectonModeIsActive();
    orthoViewer->xz->toggleROISelectonModeIsActive();
    orthoViewer->zy->toggleROISelectonModeIsActive();
}

void SignalControl::transferSegmentsWithVolume() {
    bool ok;
    double volumeThreshold = QInputDialog::getDouble(this, tr("Transfer with volume"),
                                                     tr("Transfer all Segments with a volume greater than:"),
                                                     50000, 0, 1000000, 0, &ok);
    if (!ok) {
        return;
    }

    taskRunner->run(
        [this, volumeThreshold]() { graphBase->pGraph->transferSegmentsWithVolumeCriterion(volumeThreshold); },
        [this]() { refreshViewers(); });
}

void SignalControl::transferAllSegments() {
    taskRunner->run(
        [this]() { graphBase->pGraph->transferSegmentsWithVolumeCriterion(1); },
        [this]() { refreshViewers(); });
}


void SignalControl::transferSegmentsWithRefinementWS() {
    taskRunner->run(
        [this]() { graphBase->pGraph->transferSegmentsWithRefinementOverlap(); },
        [this]() { refreshViewers(); });
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

    connect(addSegmentationButton, &QPushButton::clicked, this, &SignalControl::createNewSegmentationVolume);
    connect(loadSegmentationButton, &QPushButton::clicked, this, &SignalControl::loadSegmentationVolumePressed);
    connect(exportSegmentationButton, &QPushButton::clicked, this, &SignalControl::exportSelectedSegmentation);
    connect(togglePaintBrushButton, &QPushButton::clicked, this, &SignalControl::togglePaintMode);
    connect(setPaintIdButton, &QPushButton::clicked, this, &SignalControl::setPaintId);
    connect(transferSegmentsWithVolumeButton, &QPushButton::clicked, this, &SignalControl::transferSegmentsWithVolume);
    connect(transferSegmentsWithRefinementButton, &QPushButton::clicked, this, &SignalControl::transferSegmentsWithRefinementWS);
    connect(transferAllSegmentsButton, &QPushButton::clicked, this, &SignalControl::transferAllSegments);


    connect(segmentationTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(segmentationTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::segmentationClicked);

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
    graphBase->pRefinementWatershed = dynamic_cast<GraphSegmentImageType*>(
        allSignalList[signalIndex]->getImageBase().GetPointer());
    graphBase->pRefinementWatershedSignal = dynamic_cast<itkSignal<GraphSegmentType>*>(allSignalList[signalIndex]);
}


void SignalControl::setIdToTransparentInRefinementWS() {
    if (graphBase->pRefinementWatershed != nullptr) {
        int inputVal = QInputDialog::getInt(this, "Value to set transparent", "Value to set transparent", 0, 0);
        std::cout << "Set " << inputVal << " to transparent in selected refinement watershed.\n";
        graphBase->pRefinementWatershedSignal->setLUTValueToTransparent(inputVal);
        refreshViewers();
    }
}

void SignalControl::segmentationClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);

    // set the focused watershed as refinement watershedƒ
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);
    graphBase->pSelectedSegmentation = dynamic_cast<GraphSegmentImageType*>(
        allSignalList[signalIndex]->getImageBase().GetPointer());
    graphBase->pSelectedSegmentationSignal = dynamic_cast<itkSignal<GraphSegmentType>*>(allSignalList[signalIndex]);
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

    refreshViewers();
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
            {
                auto spacing = graphBase->pSelectedSegmentation->GetSpacing();
                auto origin = graphBase->pSelectedSegmentation->GetOrigin();
                auto direction = graphBase->pSelectedSegmentation->GetDirection();
                std::cout << "exportSelectedSegmentation: About to write pSelectedSegmentation:\n";
                std::cout << "  Spacing: [" << spacing[0] << ", " << spacing[1] << ", " << spacing[2] << "]\n";
                std::cout << "  Origin:  [" << origin[0] << ", " << origin[1] << ", " << origin[2] << "]\n";
                std::cout << "  Direction: [["
                          << direction[0][0] << ", " << direction[0][1] << ", " << direction[0][2] << "], ["
                          << direction[1][0] << ", " << direction[1][1] << ", " << direction[1][2] << "], ["
                          << direction[2][0] << ", " << direction[2][1] << ", " << direction[2][2] << "]]\n";
                std::cout << "  LC_NUMERIC locale: " << std::setlocale(LC_NUMERIC, nullptr) << "\n";
            }
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
    orthoViewer->xy->labelOfClickedSegmentation = paintId;
    orthoViewer->zy->labelOfClickedSegmentation = paintId;
    orthoViewer->xz->labelOfClickedSegmentation = paintId;
    orthoViewer->xy->setPaintId(paintId);
    orthoViewer->zy->setPaintId(paintId);
    orthoViewer->xz->setPaintId(paintId);
}

void SignalControl::togglePaintMode() {
    if (togglePaintBrushButton->text() == "Turn Paintmode On") {
        togglePaintBrushButton->setText("Turn Paintmode Off");
    } else {
        togglePaintBrushButton->setText("Turn Paintmode On");
    }

    orthoViewer->xy->togglePaintMode();
    orthoViewer->zy->togglePaintMode();
    orthoViewer->xz->togglePaintMode();
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

    for (auto *viewer: orthoViewer->viewerList) {
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

    refreshViewers();
}

void SignalControl::setUserAlpha(QTreeWidgetItem *item) {
    bool isShort, isUChar, isSegments, isEdge;
    unsigned int signalIndex;
    getSignalPropsFromItem(item, isShort, isUChar, isSegments, isEdge, signalIndex);

    unsigned char alpha = QInputDialog::getInt(this, "Alpha", "Alpha", 255, 0, 255);

    std::string alphaString = std::to_string(alpha);
    item->setText(1, QString::fromStdString(alphaString));

    allSignalList[signalIndex]->setAlpha(alpha);

    refreshViewers();
}


void SignalControl::addSegmentsGraph(QString fileName) {
    addSegmentsGraphAsync(fileName);
}

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

    {
        auto srcSpacing = graphBase->pSelectedBoundary->GetSpacing();
        auto dstSpacing = pImage->GetSpacing();
        std::cout << "addEmptySegmentsFromBoundary: Source (boundary) spacing: [" << srcSpacing[0] << ", " << srcSpacing[1] << ", " << srcSpacing[2] << "]\n";
        std::cout << "addEmptySegmentsFromBoundary: New image spacing:         [" << dstSpacing[0] << ", " << dstSpacing[1] << ", " << dstSpacing[2] << "]\n";
    }

    std::unique_ptr<itkSignal<GraphSegmentType>> pSignal2(new itkSignal<GraphSegmentType>(pImage));
    size_t signalIndexGlobal = allSignalList.size();
    itkSignalBase *pRaw = pSignal2.get();
    ownedSignals.push_back(std::move(pSignal2));
    allSignalList.push_back(pRaw);

    graphBase->ignoredSegmentLabels.clear();
    graphBase->edgeStatus.clear();
    graphBase->colorLookUpEdgesStatus.clear();
    graphBase->pWorkingSegmentsImage = pImage;
    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graphBase->pGraph->constructFromVolume(pImage);
    registerSegmentsGraphSignal(signalIndexGlobal);

    std::cout << "Done add segment function!\n";
}

void SignalControl::initializeGraph(size_t signalIndexGlobal) {
    registerSegmentsGraphSignal(signalIndexGlobal);
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
            orthoViewer->xy->turnROISelectonModeInactive();
            orthoViewer->xz->turnROISelectonModeInactive();
            orthoViewer->zy->turnROISelectonModeInactive();
        }
    }
}

void SignalControl::receiveNewRefinementWatershed(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage) {
    size_t signalIndexGlobal;
    bool loadSuccessFull = insertImageSegmenttype(pImage, signalIndexGlobal);
    if (loadSuccessFull) {
        auto *typedSignal = dynamic_cast<itkSignal<GraphSegmentType>*>(allSignalList[signalIndexGlobal]);
        allSignalList[signalIndexGlobal]->setLUTCategorical();
        allSignalList[signalIndexGlobal]->setName("Refined Watershed");
        allSignalList[signalIndexGlobal]->setupTreeWidget(refinementWatershedTreeWidget, signalIndexGlobal);
        allSignalList[signalIndexGlobal]->setIsActive(false);
        allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
        int lastItemIndex = refinementWatershedTreeWidget->topLevelItemCount() - 1;
        refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
        refinementWatershedTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
        refinementWatershedTreeWidget->setCurrentItem(refinementWatershedTreeWidget->topLevelItem(lastItemIndex));
        orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        graphBase->pRefinementWatershed = dynamic_cast<GraphSegmentImageType*>(
            allSignalList[signalIndexGlobal]->getImageBase().GetPointer());
        graphBase->pRefinementWatershedSignal = typedSignal;

        // Set ROI to currently set watershed ROI
        // this is used to prevent out-of-ROI refinements through user input
        if (graphBase->ROI_set) {
            typedSignal->ROI_set = true;
            typedSignal->ROI_fx = graphBase->ROI_fx;
            typedSignal->ROI_fy = graphBase->ROI_fy;
            typedSignal->ROI_fz = graphBase->ROI_fz;
            typedSignal->ROI_tx = graphBase->ROI_tx;
            typedSignal->ROI_ty = graphBase->ROI_ty;
            typedSignal->ROI_tz = graphBase->ROI_tz;
        }
    }
}


void SignalControl::loadMembraneProbability(QString fileName, QString displayedName) {
    std::cout << "Loading boundaries: " << fileName.toStdString() << "\n";
    bool segmentsAreNotAdded = graphBase->pWorkingSegmentsImage == nullptr;
    QMessageBox::StandardButton reply = QMessageBox::Yes;
    if (segmentsAreNotAdded) {
        reply = QMessageBox::question(this,
                                      "No Segmentation Added",
                                      "No Segments added. Do you want to create empty segments based on the added boundary image?",
                                      QMessageBox::Yes | QMessageBox::No);
    }
    if (!segmentsAreNotAdded || reply == QMessageBox::Yes) {
        loadMembraneProbabilityAsync(fileName, displayedName);
    }
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

    {
        auto srcSpacing = graphBase->pWorkingSegmentsImage->GetSpacing();
        auto srcOrigin = graphBase->pWorkingSegmentsImage->GetOrigin();
        auto srcDirection = graphBase->pWorkingSegmentsImage->GetDirection();
        auto dstSpacing = pImage->GetSpacing();
        auto dstOrigin = pImage->GetOrigin();
        auto dstDirection = pImage->GetDirection();
        std::cout << "createNewSegmentationVolume: Source (pWorkingSegmentsImage):\n";
        std::cout << "  Spacing: [" << srcSpacing[0] << ", " << srcSpacing[1] << ", " << srcSpacing[2] << "]\n";
        std::cout << "  Origin:  [" << srcOrigin[0] << ", " << srcOrigin[1] << ", " << srcOrigin[2] << "]\n";
        std::cout << "  Direction: [["
                  << srcDirection[0][0] << ", " << srcDirection[0][1] << ", " << srcDirection[0][2] << "], ["
                  << srcDirection[1][0] << ", " << srcDirection[1][1] << ", " << srcDirection[1][2] << "], ["
                  << srcDirection[2][0] << ", " << srcDirection[2][1] << ", " << srcDirection[2][2] << "]]\n";
        std::cout << "createNewSegmentationVolume: Destination (new segmentation):\n";
        std::cout << "  Spacing: [" << dstSpacing[0] << ", " << dstSpacing[1] << ", " << dstSpacing[2] << "]\n";
        std::cout << "  Origin:  [" << dstOrigin[0] << ", " << dstOrigin[1] << ", " << dstOrigin[2] << "]\n";
        std::cout << "  Direction: [["
                  << dstDirection[0][0] << ", " << dstDirection[0][1] << ", " << dstDirection[0][2] << "], ["
                  << dstDirection[1][0] << ", " << dstDirection[1][1] << ", " << dstDirection[1][2] << "], ["
                  << dstDirection[2][0] << ", " << dstDirection[2][1] << ", " << dstDirection[2][2] << "]]\n";
    }

    graphBase->pSelectedSegmentation = pImage;
    graphBase->selectedSegmentationMaxSegmentId = 0;

    std::unique_ptr<itkSignal<GraphSegmentType>> pSignal2(new itkSignal<GraphSegmentType>(pImage));
    auto *typedSignal = pSignal2.get();
    size_t signalIndexGlobal = allSignalList.size();
    allSignalList.push_back(typedSignal);
    ownedSignals.push_back(std::move(pSignal2));
    graphBase->pSelectedSegmentationSignal = typedSignal;

    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    allSignalList[signalIndexGlobal]->setName("Segmentation");
    allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);

    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
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
    loadSegmentationVolumeAsync(fileName, displayName);
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
        askForBackgroundStrategy();

        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        addSegmentsGraph(fileName);

    }
    signalInputButtonsLayout->removeWidget(addSegmentsButton);
}


void SignalControl::addRefinementWatershedPressed() {
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_save_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Segments"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        addRefinementWatershed(fileName);
    }



//        addRefinementWatershed(fileName);
}

void SignalControl::mergeSegmentsWithRefinementWatershedClicked() {
    taskRunner->run(
        [this]() { graphBase->pGraph->mergeSegmentsWithRefinementWatershed(); },
        [this]() { refreshViewers(); });
}

void SignalControl::addRefinementWatershed(QString fileName) {
    addRefinementWatershed(fileName, "");
}

void SignalControl::addRefinementWatershed(QString fileName, QString displayedName) {
    addRefinementWatershedAsync(fileName, displayedName);
}

void SignalControl::askForBackgroundStrategy() {
    QBackgroundIdRadioBox dialog;
    dialog.exec();
    graphBase->pGraph->setBackgroundIdStrategy(dialog.getStrategy().toStdString());
}

bool SignalControl::insertImageSegmenttype(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage,
                                           size_t &signalIndexGlobalOut,
                                           bool forceShapeOfSegments) {
    return insertTypedImage<dataType::SegmentIdType>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
}

bool SignalControl::loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                              size_t &signalIndexGlobalOut, bool forceShapeOfSegments,
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
                    auto pImage = ITKImageLoader<unsigned char>(fileName);
                    loadingWasSuccessful = insertTypedImage<unsigned char>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::CHAR: {
                    auto pImage = ITKImageLoader<char>(fileName);
                    loadingWasSuccessful = insertTypedImage<char>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::USHORT: {
                    auto pImage = ITKImageLoader<unsigned short>(fileName);
                    loadingWasSuccessful = insertTypedImage<unsigned short>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::SHORT: {
                    auto pImage = ITKImageLoader<short>(fileName);
                    loadingWasSuccessful = insertTypedImage<short>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::UINT: {
                    auto pImage = ITKImageLoader<unsigned int>(fileName);
                    loadingWasSuccessful = insertTypedImage<unsigned int>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::INT: {
                    auto pImage = ITKImageLoader<int>(fileName);
                    loadingWasSuccessful = insertTypedImage<int>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::ULONG: {
                    auto pImage = ITKImageLoader<unsigned long>(fileName);
                    loadingWasSuccessful = insertTypedImage<unsigned long>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::LONG: {
                    auto pImage = ITKImageLoader<long>(fileName);
                    loadingWasSuccessful = insertTypedImage<long>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::ULONGLONG: {
                    auto pImage = ITKImageLoader<unsigned long long>(fileName);
                    loadingWasSuccessful = insertTypedImage<unsigned long long>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::LONGLONG: {
                    auto pImage = ITKImageLoader<long long>(fileName);
                    loadingWasSuccessful = insertTypedImage<long long>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::FLOAT: {
                    auto pImage = ITKImageLoader<float>(fileName);
                    loadingWasSuccessful = insertTypedImage<float>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
                    break;
                }

                case itk::ImageIOBase::IOComponentType::DOUBLE: {
                    auto pImage = ITKImageLoader<double>(fileName);
                    loadingWasSuccessful = insertTypedImage<double>(pImage, signalIndexGlobalOut, forceShapeOfSegments);
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
            addImage(fileName);
        }
    }
}


void SignalControl::loadMembraneProbabilityPressed() {
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_save_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Open Images"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        if (graphBase->pWorkingSegmentsImage == nullptr) {
            loadMembraneProbability(fileName);
        } else {
            loadMembraneProbability(fileName);
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
