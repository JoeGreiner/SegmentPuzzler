#include "watershedControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"
#include <itkImage.h>
#include "itkRegionOfInterestImageFilter.h"
#include "itkPasteImageFilter.h"


#include <src/viewers/itkSignal.h>
#include <QFileDialog>
#include <QColorDialog>
#include <src/segment_handling/Graph.h>
#include <src/viewers/OrthoViewer.h>
#include <QInputDialog>
#include <QHeaderView>
#include <QAbstractItemView>
#include <src/viewers/fileIO.h>


#include <QTreeWidget>
#include <src/itkImageFilters/itkWatershedHelpers.h>
#include <src/utils/utils.h>
#include <itkMultiThreaderBase.h>
#include <QThread>
#include <src/viewers/itkSignalThresholdPreview.h>
#include <QtWidgets/QMessageBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QSignalBlocker>
#include <itkCastImageFilter.h>
#include "src/qtUtils/TaskRunner.h"
#include "src/utils/SignalNameUtils.h"
#include <algorithm>

namespace {

int defaultWatershedThreadCount() {
    const int idealThreadCount = QThread::idealThreadCount();
    return idealThreadCount > 0 ? idealThreadCount : 1;
}

int clampWatershedThreadCount(int requestedThreadCount) {
    const int idealThreadCount = QThread::idealThreadCount();
    if (idealThreadCount > 0) {
        return std::clamp(requestedThreadCount, 1, idealThreadCount);
    }
    return std::max(1, requestedThreadCount);
}

} // namespace

WatershedControl::~WatershedControl() {
}

void WatershedControl::setThreadCount(int n) {
    workerThreadCount = clampWatershedThreadCount(n);
}

void WatershedControl::setWatershedAlgorithm(WatershedAlgorithm algorithm) {
    const int count = watershedAlgorithmComboBox->count();
    for (int i = 0; i < count; ++i) {
        if (watershedAlgorithmComboBox->itemData(i).toInt() == static_cast<int>(algorithm)) {
            watershedAlgorithmComboBox->setCurrentIndex(i);
            return;
        }
    }
}

void WatershedControl::setGuiBusy(bool busy) {
    signalTreeWidget->setEnabled(!busy);

    thresholdBoundariesButton->setEnabled(!busy);
    calculateDistanceMapButton->setEnabled(!busy);
    calculateSeedsButton->setEnabled(!busy);
    runWatershedButton->setEnabled(!busy);
    createRefinementButton->setEnabled(!busy);
    togglePaintBoundaryModeButton->setEnabled(!busy);
    thresholdValueSlider->setEnabled(!busy);
    thresholdValueSpinBox->setEnabled(!busy);
    checkBoxFiltering->setEnabled(!busy);
    sizeFilteringInput->setEnabled(!busy);
    thresholdAlgorithmComboBox->setEnabled(!busy);
    distanceMapAlgorithmComboBox->setEnabled(!busy);
    seedAlgorithmComboBox->setEnabled(!busy);
    watershedAlgorithmComboBox->setEnabled(!busy);
    boundaryInputComboBox->setEnabled(!busy);
    thresholdInputComboBox->setEnabled(!busy);
    distanceMapInputComboBox->setEnabled(!busy);
    watershedDistanceMapInputComboBox->setEnabled(!busy);
    watershedSeedsInputComboBox->setEnabled(!busy);
    watershedThresholdInputComboBox->setEnabled(!busy);
    finalOutputInputComboBox->setEnabled(!busy);
    if (!busy) {
        updateStepEnablement();
    }
}

void WatershedControl::refreshViewers() {
    orthoViewer->refreshViewers();
}


void WatershedControl::thresholdBoundariesAsync(std::function<void()> then) {
    const int thresholdValue = thresholdValueSlider->value();
    const ThresholdAlgorithm thresholdAlgorithm = selectedThresholdAlgorithm();
    const dataType::BoundaryImageType::Pointer thresholdInput = selectedBoundaryInput();
    const QString signalName =
        QString("Thresholded Boundaries [%1]").arg(thresholdAlgorithmLabel(thresholdAlgorithm));
    taskRunner->runWithLabel(
        QStringLiteral("Thresholding boundaries..."),
        [thresholdValue, thresholdAlgorithm, thresholdInput]() {
            auto thresholdInputCopy = thresholdInput;
            itk::Image<unsigned char, 3>::Pointer thresholded;
            double t = utils::tic("Threshold boundaries");
            switch (thresholdAlgorithm) {
                case ThresholdAlgorithm::BinaryThreshold:
                    binaryThresholdImageFilterFloat(thresholdInputCopy, thresholded, thresholdValue);
                    break;
            }
            utils::toc(t, "Threshold boundaries done:");
            return thresholded;
        },
        [this, signalName](itk::Image<unsigned char, 3>::Pointer thresholded) {
            pThresholdedMembrane = thresholded;
            auto pThresholdedMembraneSignal = std::make_unique<itkSignal<unsigned char>>(pThresholdedMembrane);
            itkSignalBase *pSignal = pThresholdedMembraneSignal.get();
            registerSignal(std::move(pThresholdedMembraneSignal),
                           SignalStage::Threshold,
                           signalName,
                           /*categorical=*/true,
                           /*transparentZero=*/true);
            orthoViewer->xy->pThresholdedBoundaries = pThresholdedMembrane;
            orthoViewer->xz->pThresholdedBoundaries = pThresholdedMembrane;
            orthoViewer->zy->pThresholdedBoundaries = pThresholdedMembrane;
            orthoViewer->xy->pThresholdedBoundariesSignal = pSignal;
            orthoViewer->xz->pThresholdedBoundariesSignal = pSignal;
            orthoViewer->zy->pThresholdedBoundariesSignal = pSignal;
            orthoViewer->setViewToMiddleOfStack();
            updateStepEnablement();
            if (boundarySignalIndex >= 0)
                deactivateSignalsByIndices({static_cast<size_t>(boundarySignalIndex)});
        },
        std::move(then));
}

void WatershedControl::calculateDistanceMapAsync(std::function<void()> then) {
    const DistanceMapAlgorithm distanceMapAlgorithm = selectedDistanceMapAlgorithm();
    const itk::Image<unsigned char, 3>::Pointer thresholdInput = selectedThresholdInput();
    const QString signalName =
        QString("Distance Map [%1]").arg(distanceMapAlgorithmLabel(distanceMapAlgorithm));
    taskRunner->runWithLabel(
        QStringLiteral("Calculating distance map..."),
        [thresholdInput, distanceMapAlgorithm, threadCount = workerThreadCount]() {
            itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);
            auto thresholdInputCopy = thresholdInput;
            itk::Image<float, 3>::Pointer distanceMap;
            double t = utils::tic("Generate distance map");
            generateDistanceMap(thresholdInputCopy, distanceMap, 0, distanceMapAlgorithm, threadCount);
            utils::toc(t, "Generate distance map done:");
            return distanceMap;
        },
        [this, signalName](itk::Image<float, 3>::Pointer distanceMap) {
            pDistanceMap = distanceMap;
            registerSignal(std::make_unique<itkSignal<float>>(pDistanceMap), SignalStage::DistanceMap, signalName);
            updateStepEnablement();
            deactivateSignalsByIndices(thresholdOutputSignalIndices);
        },
        std::move(then));
}

void WatershedControl::extractSeedsAsync(std::function<void()> then) {
    const distance_map_benchmark::SeedExtractorKind seedAlgorithm = selectedSeedAlgorithm();
    const itk::Image<float, 3>::Pointer distanceMapInput = selectedDistanceMapInput();
    const QString signalName = QString("Seeds [%1]").arg(seedAlgorithmLabel(seedAlgorithm));
    taskRunner->runWithLabel(
        QStringLiteral("Extracting seeds..."),
        [distanceMapInput, seedAlgorithm, threadCount = workerThreadCount]() {
            itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);
            auto distanceMapInputCopy = distanceMapInput;
            itk::Image<unsigned int, 3>::Pointer seeds;
            double t = utils::tic("Extract seeds");
            extractMinimaFromDistanceMap(distanceMapInputCopy, seeds, 1, seedAlgorithm);
            utils::toc(t, "Extract seeds done:");
            return seeds;
        },
        [this, signalName](itk::Image<unsigned int, 3>::Pointer seeds) {
            pSeeds = seeds;
            registerSignal(std::make_unique<itkSignal<unsigned int>>(pSeeds),
                           SignalStage::Seeds,
                           signalName,
                           /*categorical=*/true,
                           /*transparentZero=*/true);
            updateStepEnablement();
            deactivateSignalsByIndices(distanceMapOutputSignalIndices);
        },
        std::move(then));
}

void WatershedControl::watershedAsync(std::function<void()> then) {
    const bool filterEnabled = checkBoxFiltering->isChecked();
    const int minSegmentSize = sizeFilteringInput->value();
    const WatershedAlgorithm watershedAlgorithm = selectedWatershedAlgorithm();
    const itk::Image<float, 3>::Pointer distanceMapInput = selectedWatershedDistanceMapInput();
    const itk::Image<unsigned int, 3>::Pointer seedsInput = selectedWatershedSeedsInput();
    const itk::Image<unsigned char, 3>::Pointer thresholdInput = selectedWatershedThresholdInput();
    const QString signalName =
        QString("Watershed [%1]").arg(watershedAlgorithmLabel(watershedAlgorithm));

    removeRegisteredEdgeSignal();

    taskRunner->runWithLabel(
        QStringLiteral("Running watershed..."),
        [filterEnabled, minSegmentSize, watershedAlgorithm, distanceMapInput, seedsInput, thresholdInput, threadCount = workerThreadCount]() {
            itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);
            auto distanceMapInputCopy = distanceMapInput;
            auto thresholdInputCopy = thresholdInput;
            WatershedRunOptions watershedOptions;
            watershedOptions.algorithm = watershedAlgorithm;
            itk::Image<float, 3>::Pointer invertedDistanceMap = itk::Image<float, 3>::New();
            double t = utils::tic("Invert distance map");
            invertDistanceMap(distanceMapInputCopy, invertedDistanceMap);
            utils::toc(t, "Invert distance map done:");

            itk::Image<unsigned int, 3>::Pointer watershedImage;
            itk::Image<unsigned int, 3>::Pointer seedsForWatershed = seedsInput;
            t = utils::tic("Run watershed");
            runWatershed(invertedDistanceMap, seedsForWatershed, watershedImage, watershedOptions);
            utils::toc(t, "Run watershed done:");

            if (filterEnabled) {
                t = utils::tic("Filter small seeds");
                filterSmallSegmentSeeds(watershedImage, seedsForWatershed, minSegmentSize);
                utils::toc(t, "Filter small seeds done:");
                t = utils::tic("Run watershed (filtered)");
                runWatershed(invertedDistanceMap, seedsForWatershed, watershedImage, watershedOptions);
                utils::toc(t, "Run watershed (filtered) done:");
            }

            t = utils::tic("Insert boundaries");
            insertBoundariesIntoWatershed(watershedImage, thresholdInputCopy);
            utils::toc(t, "Insert boundaries done:");
            return watershedImage;
        },
        [this, signalName](itk::Image<unsigned int, 3>::Pointer watershedImage) {
            pWatershed = watershedImage;
            auto pSignal2 = std::make_unique<itkSignal<unsigned int>>(pWatershed);
            auto *watershedSignal = pSignal2.get();
            registerSignal(std::move(pSignal2), SignalStage::Watershed, signalName, /*categorical=*/true);
            rebuildGraphFromSegmentsImage(pWatershed);
            attachSegmentsSignalToGraph(watershedSignal);
            updateStepEnablement();
            deactivateSignalsByIndices(seedOutputSignalIndices);
        },
        std::move(then));
}


void WatershedControl::createRefinementAsync(std::function<void()> then) {
    const dataType::SegmentsImageType::Pointer selectedOutput = selectedFinalOutputInput();
    taskRunner->runWithLabel(
        outputMode == OutputMode::Segments
            ? QStringLiteral("Exporting segments...")
            : QStringLiteral("Creating refinement..."),
        [this, selectedOutput]() {
            if (!useROI || outputMode == OutputMode::Segments) {
                return selectedOutput;
            }

            dataType::SegmentsImageType::Pointer paddedWorkingSegmentsImage = dataType::SegmentsImageType::New();
            paddedWorkingSegmentsImage->SetRegions(
                linkedSignalControl->graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
            paddedWorkingSegmentsImage->SetSpacing(linkedSignalControl->graphBase->pWorkingSegmentsImage->GetSpacing());
            paddedWorkingSegmentsImage->SetOrigin(linkedSignalControl->graphBase->pWorkingSegmentsImage->GetOrigin());
            paddedWorkingSegmentsImage->Allocate(true);

            using PasteImageFilterType = itk::PasteImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
            PasteImageFilterType::Pointer pasteImageFilter = PasteImageFilterType::New();
            pasteImageFilter->SetSourceImage(selectedOutput);
            pasteImageFilter->SetSourceRegion(selectedOutput->GetLargestPossibleRegion());
            pasteImageFilter->SetDestinationImage(paddedWorkingSegmentsImage);

            dataType::SegmentsImageType::IndexType destinationIndex;
            destinationIndex.at(0) = fx;
            destinationIndex.at(1) = fy;
            destinationIndex.at(2) = fz;
            pasteImageFilter->SetDestinationIndex(destinationIndex);

            paddedWorkingSegmentsImage = pasteImageFilter->GetOutput();
            pasteImageFilter->Update();
            return paddedWorkingSegmentsImage;
        },
        [this](dataType::SegmentsImageType::Pointer createdOutput) {
            if (linkedSignalControl == nullptr) {
                throw std::logic_error("WatershedControl has no linked SignalControl.");
            }

            if (outputMode == OutputMode::Segments) {
                linkedSignalControl->importGeneratedSegments(createdOutput);
            } else {
                linkedSignalControl->receiveNewRefinement(createdOutput);
            }
            emit sendClosingSignal();
        },
        std::move(then));
}


size_t WatershedControl::registerSignal(std::unique_ptr<itkSignalBase> sig, SignalStage stage,
                                        const QString &name, bool categorical, bool transparentZero, bool active) {
    size_t idx = allSignalList.size();
    itkSignalBase *raw = sig.get();
    ownedSignals.push_back(std::move(sig));
    allSignalList.push_back(raw);
    raw->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    raw->setupTreeWidget(signalTreeWidget, idx);
    if (categorical) raw->setLUTCategorical(); else raw->setLUTContinuous();
    if (transparentZero) raw->setLUTValueToTransparent(0);
    orthoViewer->addSignal(raw);

    if (!active) {
        setSignalActive(idx, false);
    }

    switch (stage) {
        case SignalStage::Threshold:
            thresholdOutputSignalIndices.push_back(idx);
            break;
        case SignalStage::DistanceMap:
            distanceMapOutputSignalIndices.push_back(idx);
            break;
        case SignalStage::Seeds:
            seedOutputSignalIndices.push_back(idx);
            break;
        case SignalStage::Watershed:
            watershedOutputSignalIndices.push_back(idx);
            break;
        case SignalStage::None:
            break;
    }

    refreshInputSelectors();
    updateStepEnablement();
    return idx;
}


void WatershedControl::addImage(QString fileName) {
    std::cout << "Adding file: " << fileName.toStdString() << std::endl;
    if (!fileName.isEmpty()) {
        itk::ImageIOBase::IOComponentType dataType;
        size_t signalIndexGlobal;
        bool loadSuccessFull = loadImage(fileName, dataType, signalIndexGlobal, false);
        if (loadSuccessFull) {
            allSignalList[signalIndexGlobal]->setLUTContinuous();
            allSignalList[signalIndexGlobal]->setName(
                signal_name_utils::makeUniqueSignalName(allSignalList, QFileInfo(fileName).baseName()));
            allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
            orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        }
    }
}

WatershedControl::WatershedControl(std::shared_ptr<GraphBase> graphBaseIn,
                                   OrthoViewer *orthoViewerIn,
                                   TaskRunner *taskRunnerIn,
                                   OutputMode outputModeIn,
                                   QWidget *parent,
                                   bool verboseIn) {
    setParent(parent);
    graphBase = graphBaseIn;
    orthoViewer = orthoViewerIn;
    taskRunner = taskRunnerIn;
    verbose = verboseIn;
    outputMode = outputModeIn;
    allSignalList.reserve(10);
    itkSignalSegmentsGraph = nullptr;
    pThresholdedMembrane = nullptr;
    pWatershed = nullptr;
    pThresholdPreviewSignal = nullptr;
    thresholdAlgorithmComboBox = nullptr;
    thresholdValueSlider = nullptr;
    thresholdValueSpinBox = nullptr;
    distanceMapAlgorithmComboBox = nullptr;
    seedAlgorithmComboBox = nullptr;
    watershedAlgorithmComboBox = nullptr;
    boundaryInputComboBox = nullptr;
    thresholdInputComboBox = nullptr;
    distanceMapInputComboBox = nullptr;
    watershedDistanceMapInputComboBox = nullptr;
    watershedSeedsInputComboBox = nullptr;
    watershedThresholdInputComboBox = nullptr;
    finalOutputInputComboBox = nullptr;
    workerThreadCount = defaultWatershedThreadCount();
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Expanding);

    useROI = false;
    fx = 0;
    fy = 0;
    fz = 0;
    tx = 0;
    ty = 0;
    tz = 0;

    setupWorkflowUi();
    setupAlgorithmComboBoxes();
    refreshInputSelectors();
    updateStepEnablement();

    connect(taskRunner, &TaskRunner::busyChanged, this, &WatershedControl::setGuiBusy);
}

void WatershedControl::setupWorkflowUi() {
    signalControlLayout = new QVBoxLayout(this);
    signalControlLayout->setContentsMargins(0, 0, 0, 0);
    signalControlLayout->setSpacing(0);

    workflowScrollArea = new QScrollArea(this);
    workflowScrollArea->setWidgetResizable(true);
    workflowScrollArea->setFrameShape(QFrame::NoFrame);

    workflowContentWidget = new QWidget(workflowScrollArea);
    workflowLayout = new QVBoxLayout(workflowContentWidget);
    workflowLayout->setContentsMargins(8, 8, 8, 8);
    workflowLayout->setSpacing(8);

    setupSignalTreeWidget();

    setupThresholdWidget();
    setupDistanceMapWidget();
    setupSeedWidget();
    setupWatershedWidget();
    setupFinalizeWidget();

    workflowLayout->addStretch(1);
    workflowScrollArea->setWidget(workflowContentWidget);
    signalControlLayout->addWidget(workflowScrollArea);
}

QGroupBox *WatershedControl::createStepGroup(const QString &title) const {
    auto *groupBox = new QGroupBox(title, workflowContentWidget);
    auto *layout = new QVBoxLayout(groupBox);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    return groupBox;
}

QWidget *WatershedControl::createLabeledInputRow(const QString &labelText, QComboBox *comboBox) const {
    return createLabeledInputRow(labelText, static_cast<QWidget *>(comboBox));
}

QWidget *WatershedControl::createLabeledInputRow(const QString &labelText, QWidget *widget) const {
    auto *rowWidget = new QWidget(workflowContentWidget);
    auto *rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    auto *label = new QLabel(labelText, rowWidget);
    label->setMinimumWidth(80);
    rowLayout->addWidget(label);
    rowLayout->addWidget(widget, 1);
    return rowWidget;
}

QWidget *WatershedControl::createSliderWithSpinBox(QSlider *&slider, QSpinBox *&spinBox) {
    auto *rowWidget = new QWidget(workflowContentWidget);
    auto *rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    slider = new QSlider(Qt::Horizontal, rowWidget);
    spinBox = new QSpinBox(rowWidget);
    spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spinBox->setFixedWidth(50);
    spinBox->setAlignment(Qt::AlignRight);

    rowLayout->addWidget(slider, 1);
    rowLayout->addWidget(spinBox, 0);

    connect(slider, &QSlider::valueChanged, spinBox, &QSpinBox::setValue);
    connect(spinBox, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);

    return rowWidget;
}

void WatershedControl::addStepSection(QGroupBox *groupBox, QWidget *controlsWidget, QWidget *actionWidget) {
    auto *layout = qobject_cast<QVBoxLayout *>(groupBox->layout());
    if (controlsWidget != nullptr) {
        layout->addWidget(controlsWidget);
    }
    if (actionWidget != nullptr) {
        layout->addWidget(actionWidget);
    }
    workflowLayout->addWidget(groupBox, 0);
}

void WatershedControl::setupTreeWidget(QTreeWidget *treeWidget) {
    treeWidget->setFocusPolicy(Qt::NoFocus);
    treeWidget->setColumnCount(2);
    treeWidget->setHeaderLabels({"Name", "Properties"});
    treeWidget->header()->setStretchLastSection(false);
    treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    treeWidget->setUniformRowHeights(true);
    setTreeVisibleRows(treeWidget, 2);
    connect(treeWidget, &QTreeWidget::itemDoubleClicked, this, &WatershedControl::treeDoubleClicked);
    connect(treeWidget, &QTreeWidget::itemClicked, this, &WatershedControl::treeClicked);
}

void WatershedControl::setupSignalTreeWidget() {
    signalTreeWidget = new QTreeWidget(this);
    setupTreeWidget(signalTreeWidget);
    setTreeVisibleRows(signalTreeWidget, 8);
    auto *groupBox = createStepGroup("Available Signals");
    auto *layout = qobject_cast<QVBoxLayout *>(groupBox->layout());
    layout->addWidget(signalTreeWidget);
    workflowLayout->addWidget(groupBox, 0);
}
void WatershedControl::forwardValueChangedSignal(int value) {
    std::cout << value << "\n";
    if (pThresholdPreviewSignal) {
        pThresholdPreviewSignal->thresholdValue = value;
    }
    refreshViewers();
}

void WatershedControl::setupThresholdWidget() {
    thresholdBoundariesButton = new QPushButton("Threshold Boundaries", this);
    boundaryInputComboBox = new QComboBox(this);
    thresholdAlgorithmComboBox = new QComboBox(this);
    auto *thresholdControl = createSliderWithSpinBox(thresholdValueSlider, thresholdValueSpinBox);
    thresholdValueSlider->setEnabled(false);
    thresholdValueSpinBox->setEnabled(false);
    configureInputCombo(boundaryInputComboBox);

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Boundary", boundaryInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Algorithm", thresholdAlgorithmComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Threshold", thresholdControl));

    auto *groupBox = createStepGroup("1. Threshold");
    addStepSection(groupBox, controlsWidget, thresholdBoundariesButton);

    connect(thresholdBoundariesButton, &QPushButton::clicked, this, &WatershedControl::thresholdBoundariesPressed);
    connect(thresholdValueSlider, &QSlider::valueChanged, this, &WatershedControl::forwardValueChangedSignal);
    connect(boundaryInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
}


void WatershedControl::setupDistanceMapWidget() {
    calculateDistanceMapButton = new QPushButton("Calculate Distancemap", this);
    thresholdInputComboBox = new QComboBox(this);
    distanceMapAlgorithmComboBox = new QComboBox(this);
    togglePaintBoundaryModeButton = new QPushButton(this);
    configureInputCombo(thresholdInputComboBox);
    updatePaintBoundaryModeButtonText();

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Thresholded Boundaries", thresholdInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Algorithm", distanceMapAlgorithmComboBox));

    auto *actionRow = new QWidget(this);
    auto *actionLayout = new QHBoxLayout(actionRow);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(6);
    actionLayout->addWidget(togglePaintBoundaryModeButton);
    actionLayout->addStretch(1);
    actionLayout->addWidget(calculateDistanceMapButton);
    controlsLayout->addWidget(actionRow);

    auto *groupBox = createStepGroup("2. Distance Map");
    addStepSection(groupBox, controlsWidget);

    connect(calculateDistanceMapButton, &QPushButton::clicked, this, &WatershedControl::calculateDistanceMapPressed);
    connect(togglePaintBoundaryModeButton, &QPushButton::clicked, this, &WatershedControl::togglePaintBoundaryMode);
    connect(thresholdInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
}

void WatershedControl::setupSeedWidget() {
    calculateSeedsButton = new QPushButton("Extract Seeds", this);
    distanceMapInputComboBox = new QComboBox(this);
    seedAlgorithmComboBox = new QComboBox(this);
    configureInputCombo(distanceMapInputComboBox);

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Distance Map", distanceMapInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Algorithm", seedAlgorithmComboBox));

    auto *groupBox = createStepGroup("3. Seeds");
    addStepSection(groupBox, controlsWidget, calculateSeedsButton);

    connect(calculateSeedsButton, &QPushButton::clicked, this, &WatershedControl::extractSeedsPressed);
    connect(distanceMapInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
}


void WatershedControl::setupWatershedWidget() {
    runWatershedButton = new QPushButton("Run Watershed", this);
    watershedDistanceMapInputComboBox = new QComboBox(this);
    watershedSeedsInputComboBox = new QComboBox(this);
    watershedThresholdInputComboBox = new QComboBox(this);
    watershedAlgorithmComboBox = new QComboBox(this);
    configureInputCombo(watershedDistanceMapInputComboBox);
    configureInputCombo(watershedSeedsInputComboBox);
    configureInputCombo(watershedThresholdInputComboBox);
    checkBoxFiltering = new QCheckBox("Activate Segment Size Filtering", this);
    checkBoxFiltering->setChecked(false);
    sizeFilteringInput = new QSpinBox(this);
    sizeFilteringInput->setMinimum(100);
    sizeFilteringInput->setMaximum(10000000);
    sizeFilteringInput->setValue(5000);
    sizeFilteringInput->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);

    auto *filterRow = new QWidget(this);
    auto *filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);
    filterLayout->addWidget(checkBoxFiltering);
    filterLayout->addWidget(sizeFilteringInput);
    filterLayout->addStretch(1);

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Distance Map", watershedDistanceMapInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Seeds", watershedSeedsInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Thresholded Boundaries", watershedThresholdInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Algorithm", watershedAlgorithmComboBox));
    controlsLayout->addWidget(filterRow);

    auto *groupBox = createStepGroup("4. Watershed");
    addStepSection(groupBox, controlsWidget, runWatershedButton);

    connect(runWatershedButton, &QPushButton::clicked, this, &WatershedControl::watershedPressed);
    connect(watershedDistanceMapInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
    connect(watershedSeedsInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
    connect(watershedThresholdInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
}


void WatershedControl::setupFinalizeWidget() {
    createRefinementButton = new QPushButton(
        outputMode == OutputMode::Segments ? "Export Segments" : "Create Refinement",
        this);
    finalOutputInputComboBox = new QComboBox(this);
    configureInputCombo(finalOutputInputComboBox);

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Watershed", finalOutputInputComboBox));

    auto *groupBox = createStepGroup(
        outputMode == OutputMode::Segments ? "5. Export Segments" : "5. Create Refinement");
    auto *layout = qobject_cast<QVBoxLayout *>(groupBox->layout());
    layout->addWidget(controlsWidget);
    layout->addWidget(createRefinementButton);
    workflowLayout->addWidget(groupBox, 0);

    connect(createRefinementButton, &QPushButton::clicked, this, &WatershedControl::finalizeOutputPressed);
    connect(finalOutputInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
}

void WatershedControl::setupAlgorithmComboBoxes() {
    configureAlgorithmCombo(thresholdAlgorithmComboBox);
    thresholdAlgorithmComboBox->addItem(
        thresholdAlgorithmLabel(ThresholdAlgorithm::BinaryThreshold),
        static_cast<int>(ThresholdAlgorithm::BinaryThreshold));
    thresholdAlgorithmComboBox->setCurrentIndex(0);

    configureAlgorithmCombo(distanceMapAlgorithmComboBox);
    distanceMapAlgorithmComboBox->addItem(
        distanceMapAlgorithmLabel(DistanceMapAlgorithm::Maurer),
        static_cast<int>(DistanceMapAlgorithm::Maurer));
    distanceMapAlgorithmComboBox->addItem(
        distanceMapAlgorithmLabel(DistanceMapAlgorithm::FH),
        static_cast<int>(DistanceMapAlgorithm::FH));
    distanceMapAlgorithmComboBox->setCurrentIndex(0);

    configureAlgorithmCombo(seedAlgorithmComboBox);
    seedAlgorithmComboBox->addItem(
        seedAlgorithmLabel(distance_map_benchmark::SeedExtractorKind::LocalMaxima),
        static_cast<int>(distance_map_benchmark::SeedExtractorKind::LocalMaxima));
    seedAlgorithmComboBox->addItem(
        seedAlgorithmLabel(distance_map_benchmark::SeedExtractorKind::HConvex),
        static_cast<int>(distance_map_benchmark::SeedExtractorKind::HConvex));
    seedAlgorithmComboBox->setCurrentIndex(0);

    configureAlgorithmCombo(watershedAlgorithmComboBox);
    watershedAlgorithmComboBox->addItem(
        watershedAlgorithmLabel(WatershedAlgorithm::MorphologicalWatershedFromMarkers),
        static_cast<int>(WatershedAlgorithm::MorphologicalWatershedFromMarkers));
    watershedAlgorithmComboBox->addItem(
        watershedAlgorithmLabel(WatershedAlgorithm::FastMarkerWatershed),
        static_cast<int>(WatershedAlgorithm::FastMarkerWatershed));
    watershedAlgorithmComboBox->setCurrentIndex(1);
}

void WatershedControl::configureInputCombo(QComboBox *comboBox) const {
    comboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    comboBox->setMaxVisibleItems(8);
    comboBox->setStyleSheet(
        "QComboBox { combobox-popup: 0; }"
        "QComboBox QAbstractItemView { padding: 0px; margin: 0px; outline: 0; }"
        "QComboBox QAbstractItemView::item { margin: 0px; padding: 0px; min-height: 0px; }");

    auto *view = new QListView(comboBox);
    view->setUniformItemSizes(true);
    view->setSpacing(0);
    comboBox->setView(view);
}

void WatershedControl::configureAlgorithmCombo(QComboBox *comboBox) const {
    configureInputCombo(comboBox);
}

void WatershedControl::setTreeVisibleRows(QTreeWidget *treeWidget, int rows) const {
    if (treeWidget == nullptr) {
        return;
    }

    const int rowHeight = treeWidget->fontMetrics().height() + 6;
    const int headerHeight = treeWidget->header() != nullptr ? treeWidget->header()->sizeHint().height() : 0;
    const int frameHeight = treeWidget->frameWidth() * 2;
    const int targetHeight = headerHeight + frameHeight + (std::max(0, rows) * rowHeight);
    treeWidget->setMinimumHeight(targetHeight);
    treeWidget->setMaximumHeight(targetHeight);
}

void WatershedControl::refreshComboSelection(QComboBox *comboBox, const std::vector<size_t> &signalIndices) {
    QVariant currentData = comboBox->currentData();
    const QSignalBlocker blocker(comboBox);
    comboBox->clear();
    for (size_t signalIndex : signalIndices) {
        if (signalIndex < allSignalList.size() && allSignalList[signalIndex] != nullptr) {
            comboBox->addItem(allSignalList[signalIndex]->name, static_cast<int>(signalIndex));
        }
    }

    if (comboBox->count() == 0) {
        return;
    }

    const int preservedIndex = comboBox->findData(currentData);
    comboBox->setCurrentIndex(preservedIndex >= 0 ? preservedIndex : comboBox->count() - 1);
}

void WatershedControl::refreshInputSelectors() {
    if (boundaryInputComboBox != nullptr) {
        const QSignalBlocker blocker(boundaryInputComboBox);
        const QVariant currentData = boundaryInputComboBox->currentData();
        boundaryInputComboBox->clear();
        if (boundarySignalIndex >= 0 && boundarySignalIndex < static_cast<int>(allSignalList.size())) {
            boundaryInputComboBox->addItem(allSignalList[boundarySignalIndex]->name, boundarySignalIndex);
        }
        if (boundaryInputComboBox->count() > 0) {
            const int preservedIndex = boundaryInputComboBox->findData(currentData);
            boundaryInputComboBox->setCurrentIndex(preservedIndex >= 0 ? preservedIndex : 0);
        }
    }

    refreshComboSelection(thresholdInputComboBox, thresholdOutputSignalIndices);
    refreshComboSelection(distanceMapInputComboBox, distanceMapOutputSignalIndices);
    refreshComboSelection(watershedThresholdInputComboBox, thresholdOutputSignalIndices);
    refreshComboSelection(watershedDistanceMapInputComboBox, distanceMapOutputSignalIndices);
    refreshComboSelection(watershedSeedsInputComboBox, seedOutputSignalIndices);
    refreshComboSelection(finalOutputInputComboBox, watershedOutputSignalIndices);
}

bool WatershedControl::comboHasValidSelection(const QComboBox *comboBox) const {
    return comboBox != nullptr && comboBox->currentIndex() >= 0;
}

size_t WatershedControl::selectedSignalIndex(const QComboBox *comboBox) const {
    if (!comboHasValidSelection(comboBox)) {
        throw std::logic_error("Selected input is not available.");
    }
    return static_cast<size_t>(comboBox->currentData().toInt());
}

dataType::BoundaryImageType::Pointer WatershedControl::selectedBoundaryInput() const {
    if (!comboHasValidSelection(boundaryInputComboBox) || pBoundaries.IsNull()) {
        return nullptr;
    }
    return pBoundaries;
}

itk::Image<unsigned char, 3>::Pointer WatershedControl::selectedThresholdInput() const {
    if (!comboHasValidSelection(thresholdInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<unsigned char> *>(allSignalList[selectedSignalIndex(thresholdInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}

itk::Image<float, 3>::Pointer WatershedControl::selectedDistanceMapInput() const {
    if (!comboHasValidSelection(distanceMapInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<float> *>(allSignalList[selectedSignalIndex(distanceMapInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}

itk::Image<unsigned int, 3>::Pointer WatershedControl::selectedSeedsInput() const {
    if (!comboHasValidSelection(watershedSeedsInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<unsigned int> *>(allSignalList[selectedSignalIndex(watershedSeedsInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}

itk::Image<float, 3>::Pointer WatershedControl::selectedWatershedDistanceMapInput() const {
    if (!comboHasValidSelection(watershedDistanceMapInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<float> *>(allSignalList[selectedSignalIndex(watershedDistanceMapInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}

itk::Image<unsigned int, 3>::Pointer WatershedControl::selectedWatershedSeedsInput() const {
    if (!comboHasValidSelection(watershedSeedsInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<unsigned int> *>(allSignalList[selectedSignalIndex(watershedSeedsInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}

itk::Image<unsigned char, 3>::Pointer WatershedControl::selectedWatershedThresholdInput() const {
    if (!comboHasValidSelection(watershedThresholdInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<unsigned char> *>(allSignalList[selectedSignalIndex(watershedThresholdInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}

dataType::SegmentsImageType::Pointer WatershedControl::selectedFinalOutputInput() const {
    if (!comboHasValidSelection(finalOutputInputComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<GraphSegmentType> *>(allSignalList[selectedSignalIndex(finalOutputInputComboBox)]);
    return signal ? signal->pImage : nullptr;
}


void WatershedControl::updateStepEnablement() {
    const bool hasBoundary = selectedBoundaryInput().IsNotNull();
    const bool hasThreshold = selectedThresholdInput().IsNotNull();
    const bool hasDistanceMap = selectedDistanceMapInput().IsNotNull();
    const bool hasWatershedDistanceMap = selectedWatershedDistanceMapInput().IsNotNull();
    const bool hasWatershedSeeds = selectedWatershedSeedsInput().IsNotNull();
    const bool hasWatershedThreshold = selectedWatershedThresholdInput().IsNotNull();
    const bool hasFinalOutput = selectedFinalOutputInput().IsNotNull();

    thresholdBoundariesButton->setEnabled(hasBoundary);
    thresholdValueSlider->setEnabled(hasBoundary);
    calculateDistanceMapButton->setEnabled(hasThreshold);
    calculateSeedsButton->setEnabled(hasDistanceMap);
    runWatershedButton->setEnabled(hasWatershedDistanceMap && hasWatershedSeeds && hasWatershedThreshold);
    createRefinementButton->setEnabled(hasFinalOutput);
}

WatershedControl::ThresholdAlgorithm WatershedControl::selectedThresholdAlgorithm() const {
    return static_cast<ThresholdAlgorithm>(thresholdAlgorithmComboBox->currentData().toInt());
}

DistanceMapAlgorithm WatershedControl::selectedDistanceMapAlgorithm() const {
    return static_cast<DistanceMapAlgorithm>(distanceMapAlgorithmComboBox->currentData().toInt());
}

distance_map_benchmark::SeedExtractorKind WatershedControl::selectedSeedAlgorithm() const {
    return static_cast<distance_map_benchmark::SeedExtractorKind>(seedAlgorithmComboBox->currentData().toInt());
}

WatershedAlgorithm WatershedControl::selectedWatershedAlgorithm() const {
    return static_cast<WatershedAlgorithm>(watershedAlgorithmComboBox->currentData().toInt());
}

QString WatershedControl::thresholdAlgorithmLabel(ThresholdAlgorithm algorithm) const {
    switch (algorithm) {
        case ThresholdAlgorithm::BinaryThreshold:
            return "Binary Threshold";
    }
    return "Binary Threshold";
}

QString WatershedControl::distanceMapAlgorithmLabel(DistanceMapAlgorithm algorithm) const {
    switch (algorithm) {
        case DistanceMapAlgorithm::Maurer:
            return "Maurer";
        case DistanceMapAlgorithm::FH:
            return "FH";
    }
    return "Maurer";
}

QString WatershedControl::seedAlgorithmLabel(distance_map_benchmark::SeedExtractorKind algorithm) const {
    switch (algorithm) {
        case distance_map_benchmark::SeedExtractorKind::LocalMaxima:
            return "Local Maxima";
        case distance_map_benchmark::SeedExtractorKind::HConvex:
            return "H-Convex";
        case distance_map_benchmark::SeedExtractorKind::All:
            return "All";
    }
    return "Local Maxima";
}

QString WatershedControl::watershedAlgorithmLabel(WatershedAlgorithm algorithm) const {
    switch (algorithm) {
        case WatershedAlgorithm::MorphologicalWatershedFromMarkers:
            return "Morphological Watershed";
        case WatershedAlgorithm::FastMarkerWatershed:
            return "Fast Marker Watershed";
    }
    return "Morphological Watershed";
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
    const size_t signalIndex = signalIndexForItem(item);
    if (verbose) {
        std::cout << "Setting Color for signal " << signalIndex << std::endl;
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

void WatershedControl::setDescription(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);
    if (verbose) {
        std::cout << "Setting Name for signal " << signalIndex << std::endl;
    }

    bool inputSuccessful;
    QString newName =
            QInputDialog::getText(this, "Set New Description", "Enter new Descriptor:", QLineEdit::Normal, QString(),
                                  &inputSuccessful);

    if (inputSuccessful) {
        if (isSegmentsItem(item)) { // TODO: put some unique identifier besides name/descriptor for segments
            std::cout << "TODO: Fix segment descriptor change!\n";
        } else {
            item->setText(0, newName);
            allSignalList[signalIndex]->setName(newName);
        }
    }
}

void WatershedControl::togglePaintBoundaryMode() {
    paintBoundaryModeActive = !paintBoundaryModeActive;
    updatePaintBoundaryModeButtonText();

    orthoViewer->xy->togglePaintBoundaryMode();
    orthoViewer->zy->togglePaintBoundaryMode();
    orthoViewer->xz->togglePaintBoundaryMode();
}

void WatershedControl::updatePaintBoundaryModeButtonText() {
    if (togglePaintBoundaryModeButton == nullptr) {
        return;
    }

    togglePaintBoundaryModeButton->setText(paintBoundaryModeActive ? "Disable Paint Mode"
                                                                   : "Enable Paint Mode");
}



void WatershedControl::setSignalActive(size_t signalIdx, bool active) {
    for (int i = 0; i < signalTreeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem *treeItem = signalTreeWidget->topLevelItem(i);
        if (treeItem->data(0, Qt::UserRole).toULongLong() == static_cast<qulonglong>(signalIdx)) {
            if ((treeItem->checkState(0) == Qt::Checked) != active) {
                treeItem->setCheckState(0, active ? Qt::Checked : Qt::Unchecked);
                setIsActive(treeItem, active);
            }
            break;
        }
    }
}

void WatershedControl::deactivateSignalsByIndices(const std::vector<size_t> &signalIndices) {
    for (const size_t signalIdx : signalIndices) {
        setSignalActive(signalIdx, false);
    }
}

void WatershedControl::setIsActive(QTreeWidgetItem *item, bool isActiveIn) {
    if (verbose) { std::cout << "Setting item isActive to: " << isActiveIn << std::endl; }
    const size_t signalIndex = signalIndexForItem(item);

    if (isActiveIn) {
        item->setText(1, "active");
    } else {
        item->setText(1, "inactive");
    }

    allSignalList[signalIndex]->setIsActive(isActiveIn);

    for (auto *viewer : orthoViewer->viewerList) {
        viewer->setSliceIndex(viewer->getSliceIndex()); // update slice indices of newly activated signals
        viewer->recalculateQImages();
    }

}

void WatershedControl::setUserNorm(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);

//        std::string test item->get;
    std::cout << "index: " << signalIndex << std::endl;
    int normLower = QInputDialog::getInt(this, "Min Normalization", "Min Normalization", 0);
    int normUpper = QInputDialog::getInt(this, "Max Normalization", "Max Normalization", 255, normLower);

    QString normString = QString("%1").arg(normLower) + " " + QString("%1").arg(normUpper);
    item->setText(1, normString);

    allSignalList[signalIndex]->setNorm(normLower, normUpper);

    refreshViewers();
}

void WatershedControl::setUserAlpha(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);

    unsigned char alpha = QInputDialog::getInt(this, "Alpha", "Alpha", 255, 0, 255);

    std::string alphaString = std::to_string(alpha);
    item->setText(1, QString::fromStdString(alphaString));

    allSignalList[signalIndex]->setAlpha(alpha);

    refreshViewers();
}


void WatershedControl::transferWatershedToGraph() {
    auto pSignal2 = std::make_unique<itkSignal<unsigned int>>(pWatershed);
    auto *watershedSignal = pSignal2.get();
    registerSignal(std::move(pSignal2), SignalStage::Watershed, "Watershed", /*categorical=*/true);

    rebuildGraphFromSegmentsImage(watershedSignal->pImage);
    //TODO: Add feature calculation
//    graphBase->pGraph->calculateNodeFeatures();
//    if(FeatureList::GroundTruthLabelComputed) graphBase->pGraph->propagateMergeFlagToEdges();
//    graphBase->pGraph->calculateEdgeFeatures();
//    graphBase->pGraph->calculateUnionFeatures();
    attachSegmentsSignalToGraph(watershedSignal);
}

void WatershedControl::removeRegisteredEdgeSignal() {
    if (registeredEdgeSignalIndex < 0) {
        return;
    }
    itkSignalBase *oldEdge = allSignalList[static_cast<size_t>(registeredEdgeSignalIndex)];
    orthoViewer->removeSignal(oldEdge);
    allSignalList[static_cast<size_t>(registeredEdgeSignalIndex)] = nullptr;
    for (int i = 0; i < signalTreeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem *treeItem = signalTreeWidget->topLevelItem(i);
        if (treeItem->data(0, Qt::UserRole).toULongLong() ==
                static_cast<qulonglong>(registeredEdgeSignalIndex)) {
            delete signalTreeWidget->takeTopLevelItem(i);
            break;
        }
    }
    registeredEdgeSignalIndex = -1;
}

void WatershedControl::rebuildGraphFromSegmentsImage(dataType::SegmentsImageType::Pointer segmentsImage) {
    graphBase->ignoredSegmentLabels.clear();
    graphBase->edgeStatus.clear();
    graphBase->colorLookUpEdgesStatus.clear();
    graphBase->pWorkingSegmentsImage = segmentsImage;
    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    double t = utils::tic("Build graph from volume");
    graphBase->pGraph->constructFromVolume(segmentsImage);
    utils::toc(t, "Build graph from volume done:");
}

void WatershedControl::attachSegmentsSignalToGraph(itkSignal<GraphSegmentType> *segmentsSignal) {
    if (segmentsSignal == nullptr) {
        throw std::logic_error("Segments signal is null.");
    }

    itkSignalSegmentsGraph = segmentsSignal;
    graphBase->pWorkingSegments = itkSignalSegmentsGraph;
    graphBase->pWorkingSegmentsImage = itkSignalSegmentsGraph->pImage;
    if (!graphBase->ignoredSegmentLabels.empty()) {
        segmentsSignal->setLUTValueToBlack(graphBase->ignoredSegmentLabels.front());
    }

    graphBase->pEdgesInitialSegmentsITKSignal->setName(
        signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Edges")));
    graphBase->pEdgesInitialSegmentsITKSignal->setupTreeWidget(signalTreeWidget, allSignalList.size());
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    graphBase->pEdgesInitialSegmentsITKSignal->setIsActive(false);
    int lastItemIndex = signalTreeWidget->topLevelItemCount() - 1;
    signalTreeWidget->topLevelItem(lastItemIndex)->setCheckState(0, Qt::Unchecked);
    signalTreeWidget->topLevelItem(lastItemIndex)->setText(1, "inactive");
    registeredEdgeSignalIndex = static_cast<int>(allSignalList.size());
    allSignalList.push_back(graphBase->pEdgesInitialSegmentsITKSignal);
    orthoViewer->addSignal(graphBase->pEdgesInitialSegmentsITKSignal);
    orthoViewer->setViewToMiddleOfStack();
}



bool WatershedControl::loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                              size_t &signalIndexGlobalOut, bool forceShapeOfSegments,
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
                                                    tr("Load Image"));
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

    addBoundaries(pBoundariesROIImage);
}

void WatershedControl::addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn) {
    pBoundaries = pBoundariesIn;

    // 1. Raw Boundaries Signal
    auto rawSignal = std::make_unique<itkSignal<dataType::BoundaryVoxelType>>(pBoundaries);
    rawSignal->setMainColor(0, 255, 0); // Green
    const int rawIdx = static_cast<int>(registerSignal(
        std::move(rawSignal), SignalStage::None, "Boundaries", false, false, false));
    boundarySignalIndex = rawIdx;

    // 2. Live Threshold Preview Signal
    auto previewSignal = std::make_unique<itkSignalThresholdPreview<dataType::BoundaryVoxelType>>(pBoundaries);
    pThresholdPreviewSignal = previewSignal.get();
    pThresholdPreviewSignal->setMainColor(0, 255, 255); // Cyan
    const int previewIdx = static_cast<int>(registerSignal(
        std::move(previewSignal), SignalStage::None, "Threshold Preview", false, true));

    int minValue = pThresholdPreviewSignal->getMinimumValue();
    int maxValue = pThresholdPreviewSignal->getMaximumValue();

    thresholdValueSlider->setRange(minValue, maxValue);
    thresholdValueSpinBox->setRange(minValue, maxValue);
    thresholdValueSlider->setValue(static_cast<int>((minValue + maxValue)/2));
    
    refreshInputSelectors();
    updateStepEnablement();
}


void WatershedControl::thresholdBoundariesPressed() {
    thresholdBoundariesAsync();
}

void WatershedControl::extractSeedsPressed() {
    if (selectedDistanceMapInput() != nullptr) {
        extractSeedsAsync();
    } else {
        QMessageBox msgBox;
        msgBox.setText("Please generate a distance map first.");
        msgBox.exec();
    }
}

void WatershedControl::watershedPressed() {
    if (selectedWatershedDistanceMapInput() != nullptr &&
        selectedWatershedSeedsInput() != nullptr &&
        selectedWatershedThresholdInput() != nullptr) {
        watershedAsync();
    } else {
        QMessageBox msgBox;
        msgBox.setText("Please generate seeds first.");
        msgBox.exec();
    }
}


void WatershedControl::finalizeOutputPressed() {
    if (selectedFinalOutputInput() != nullptr) {
        createRefinementAsync();
    } else {
        QMessageBox msgBox;
        msgBox.setText(outputMode == OutputMode::Segments
                           ? "Please generate a watershed before exporting segments."
                           : "Please generate a watershed first.");
        msgBox.exec();
    }
}
}


void WatershedControl::calculateDistanceMapPressed() {
    if (selectedThresholdInput() != nullptr) {
        calculateDistanceMapAsync();
    } else {
        QMessageBox msgBox;
        msgBox.setText("Please threshold the boundaries first.");
        msgBox.exec();
    }
}


QTreeWidgetItem *WatershedControl::topLevelItem(QTreeWidgetItem *item) const {
    return item != nullptr && item->parent() != nullptr ? item->parent() : item;
}

size_t WatershedControl::signalIndexForItem(QTreeWidgetItem *item) const {
    QTreeWidgetItem *baseItem = topLevelItem(item);
    if (baseItem == nullptr) {
        throw std::logic_error("signal item not found!");
    }

    const qulonglong signalIndex = baseItem->data(0, Qt::UserRole).toULongLong();
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        throw std::logic_error("signal index not found!");
    }

    return static_cast<size_t>(signalIndex);
}

itkSignalBase *WatershedControl::signalForItem(QTreeWidgetItem *item) const {
    return allSignalList[signalIndexForItem(item)];
}

bool WatershedControl::isSegmentsItem(QTreeWidgetItem *item) const {
    return itkSignalSegmentsGraph != nullptr && signalForItem(item) == itkSignalSegmentsGraph;
}
