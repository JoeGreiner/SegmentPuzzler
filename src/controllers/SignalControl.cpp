#include "SignalControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"
#include "MainWindowWatershedControl.h"
#include "src/viewers/OrthoViewer.h"
#include "src/qtUtils/SignalLayerWidget.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SignalTreeWidgetUtils.h"
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include <QFileDialog>
#include <QColorDialog>
#include <QFont>
#include <QActionGroup>
#include <QInputDialog>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QWidgetAction>
#include <src/qtUtils/QImageSelectionRadioButtons.h>
#include <QSettings>
#include <QThread>
#include <QApplication>
#include <QTimer>
#include <clocale>
#include <algorithm>
#include <itkImageDuplicator.h>
#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>
#include <limits>
#include <cmath>

#include "src/utils/SignalNameUtils.h"
#include "src/utils/AppLogger.h"

namespace {

constexpr int kSectionSpacing = 4;
// Default visible-row targets for the initial layout. Trees scroll once more
// items are loaded.
constexpr int kLayersVisibleRows = 4;
constexpr int kOtherSectionVisibleRows = 2;
constexpr int kApproximateTreeRowPadding = 8;

bool debugLayerLayoutEnabled() {
    static const bool enabled = !qgetenv("SEGMENTPUZZLER_DEBUG_LAYER_LAYOUT").isEmpty();
    return enabled;
}

QLabel *createSectionLabel(const QString &title, QWidget *parent) {
    auto *label = new QLabel(title, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

dataType::SegmentsImageType::Pointer duplicateSegmentsImage(
    const dataType::SegmentsImageType::Pointer &sourceImage) {
    using DuplicatorType = itk::ImageDuplicator<dataType::SegmentsImageType>;
    auto duplicator = DuplicatorType::New();
    duplicator->SetInputImage(sourceImage);
    duplicator->Update();
    return duplicator->GetOutput();
}

template<typename T>
dataType::BoundaryImageType::Pointer convertFloatBoundaryImage(
    const typename itk::Image<T, 3>::Pointer &sourceImage,
    SignalControl::FloatBoundaryConversionMode conversionMode) {
    auto convertedImage = dataType::BoundaryImageType::New();
    convertedImage->SetRegions(sourceImage->GetLargestPossibleRegion());
    convertedImage->SetSpacing(sourceImage->GetSpacing());
    convertedImage->SetOrigin(sourceImage->GetOrigin());
    convertedImage->SetDirection(sourceImage->GetDirection());
    convertedImage->Allocate();

    const double targetMax = static_cast<double>(std::numeric_limits<dataType::BoundaryVoxelType>::max());
    double minValue = 0.0;
    double maxValue = 0.0;

    if (conversionMode == SignalControl::FloatBoundaryConversionMode::ScaleMinMax) {
        itk::ImageRegionConstIterator<itk::Image<T, 3>> inputIt(sourceImage, sourceImage->GetLargestPossibleRegion());
        bool firstFiniteValue = true;
        for (inputIt.GoToBegin(); !inputIt.IsAtEnd(); ++inputIt) {
            const double value = static_cast<double>(inputIt.Get());
            if (!std::isfinite(value)) {
                continue;
            }
            if (firstFiniteValue) {
                minValue = value;
                maxValue = value;
                firstFiniteValue = false;
            } else {
                minValue = std::min(minValue, value);
                maxValue = std::max(maxValue, value);
            }
        }
        if (firstFiniteValue) {
            minValue = 0.0;
            maxValue = 0.0;
        }
    }

    itk::ImageRegionConstIterator<itk::Image<T, 3>> inputIt(sourceImage, sourceImage->GetLargestPossibleRegion());
    itk::ImageRegionIterator<dataType::BoundaryImageType> outputIt(convertedImage, convertedImage->GetLargestPossibleRegion());
    for (inputIt.GoToBegin(), outputIt.GoToBegin(); !inputIt.IsAtEnd(); ++inputIt, ++outputIt) {
        const double rawValue = static_cast<double>(inputIt.Get());
        double convertedValue = 0.0;

        if (std::isfinite(rawValue)) {
            switch (conversionMode) {
                case SignalControl::FloatBoundaryConversionMode::CastValues:
                    convertedValue = rawValue;
                    break;
                case SignalControl::FloatBoundaryConversionMode::ScaleMinMax:
                    if (maxValue > minValue) {
                        convertedValue = ((rawValue - minValue) / (maxValue - minValue)) * targetMax;
                    }
                    break;
                case SignalControl::FloatBoundaryConversionMode::ScaleZeroToOne:
                    convertedValue = std::clamp(rawValue, 0.0, 1.0) * targetMax;
                    break;
            }
        }

        convertedValue = std::clamp(convertedValue, 0.0, targetMax);
        outputIt.Set(static_cast<dataType::BoundaryVoxelType>(std::llround(convertedValue)));
    }

    return convertedImage;
}

void bindButtonToAction(QPushButton *button, QAction *action, const QString &buttonText = QString()) {
    if (button == nullptr || action == nullptr) {
        return;
    }

    auto syncButton = [button, action, buttonText]() {
        button->setText(buttonText.isEmpty() ? action->text() : buttonText);
        button->setEnabled(action->isEnabled());
    };

    syncButton();
    QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
    QObject::connect(action, &QAction::changed, button, syncButton);
}

void setTreeMinimumRows(QTreeWidget *tree, int rows) {
    if (tree == nullptr) {
        return;
    }

    const int rowHeight = tree->fontMetrics().height() + kApproximateTreeRowPadding;
    const int headerHeight = tree->header() != nullptr ? tree->header()->sizeHint().height() : 0;
    const int frameHeight = tree->frameWidth() * 2;
    tree->setMinimumHeight(headerHeight + frameHeight + (std::max(0, rows) * rowHeight));
}

void configureSelectionLabel(QLabel *label) {
    if (label == nullptr) {
        return;
    }

    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
}

SignalLayerWidget *layerWidgetForItem(QTreeWidget *tree, QTreeWidgetItem *item) {
    if (tree == nullptr || item == nullptr) {
        return nullptr;
    }
    return qobject_cast<SignalLayerWidget *>(tree->itemWidget(item, 0));
}

QString debugTreeName(const QTreeWidget *tree) {
    if (tree == nullptr) {
        return QStringLiteral("<null>");
    }
    if (!tree->objectName().isEmpty()) {
        return tree->objectName();
    }
    return QStringLiteral("QTreeWidget@0x%1").arg(reinterpret_cast<quintptr>(tree), 0, 16);
}

int layerTreeLeftGutterWidth(QTreeWidget *tree) {
    if (tree == nullptr) {
        return 0;
    }

    int gutterWidth = 0;
    for (int itemIndex = 0; itemIndex < tree->topLevelItemCount(); ++itemIndex) {
        QTreeWidgetItem *item = tree->topLevelItem(itemIndex);
        if (item == nullptr) {
            continue;
        }

        const QModelIndex modelIndex = tree->model()->index(itemIndex, 0);
        const QRect itemRect = modelIndex.isValid() ? tree->visualRect(modelIndex) : tree->visualItemRect(item);
        if (itemRect.isValid()) {
            gutterWidth = std::max(gutterWidth, itemRect.x());
        }
    }

    return gutterWidth;
}

int layerTreePreferredWidth(QTreeWidget *tree) {
    if (tree == nullptr) {
        return 0;
    }

    int widestLayerWidth = 0;
    for (int itemIndex = 0; itemIndex < tree->topLevelItemCount(); ++itemIndex) {
        QTreeWidgetItem *item = tree->topLevelItem(itemIndex);
        if (auto *layerWidget = layerWidgetForItem(tree, item)) {
            widestLayerWidth = std::max(widestLayerWidth, layerWidget->minimumSizeHint().width());
        }
    }

    if (widestLayerWidth <= 0) {
        return 0;
    }

    const int gutterWidth = layerTreeLeftGutterWidth(tree);
    const int scrollbarWidth = tree->verticalScrollBar() != nullptr ? tree->verticalScrollBar()->sizeHint().width() : 0;
    const int frameWidth = tree->frameWidth() * 2;
    return gutterWidth + widestLayerWidth + scrollbarWidth + frameWidth + 8;
}

QTreeWidgetItem *findSignalTreeItem(QTreeWidget *tree, size_t signalIndex) {
    if (tree == nullptr) {
        return nullptr;
    }

    for (int itemIndex = 0; itemIndex < tree->topLevelItemCount(); ++itemIndex) {
        QTreeWidgetItem *item = tree->topLevelItem(itemIndex);
        if (item != nullptr && signal_tree::signalIndex(item) == signalIndex) {
            return item;
        }
    }

    return nullptr;
}

QString contrastChipText(itkSignalBase *signal) {
    if (signal == nullptr) {
        return QString();
    }
    return QString("%1..%2")
        .arg(static_cast<int>(std::lround(signal->getNormLower())))
        .arg(static_cast<int>(std::lround(signal->getNormUpper())));
}

QString opacityChipText(itkSignalBase *signal) {
    if (signal == nullptr) {
        return QString();
    }
    const int opacityPercent = static_cast<int>(std::lround((static_cast<double>(signal->getAlpha()) / 255.0) * 100.0));
    return QString("%1%").arg(opacityPercent);
}

int alphaToPercent(unsigned int alpha) {
    return static_cast<int>(std::lround((static_cast<double>(alpha) / 255.0) * 100.0));
}

unsigned char percentToAlpha(int percent) {
    const int clampedPercent = std::clamp(percent, 0, 100);
    return static_cast<unsigned char>(std::lround((static_cast<double>(clampedPercent) / 100.0) * 255.0));
}

QString layerToolTipText(itkSignalBase *signal) {
    if (signal == nullptr) {
        return QString();
    }

    QString toolTip = QString("Type: %1").arg(signal->getDisplayDataTypeName());
    if (signal->usesEdgeStatusColors()) {
        toolTip += "\nEdge colors are fixed: white, red, green.";
    } else if (signal->usesCategoricalLUT()) {
        toolTip += "\nClick the heart to randomize categorical colors.";
    } else {
        toolTip += "\nClick the heart to change the display color.";
    }
    if (signal->supportsNormControl()) {
        toolTip += "\nClick contrast to adjust the display range.";
    }
    toolTip += "\nClick opacity to adjust the overlay strength.";
    return toolTip;
}

int clampPopupValue(double value, int minimum, int maximum) {
    if (maximum < minimum) {
        return minimum;
    }
    if (!std::isfinite(value)) {
        return minimum;
    }
    const double clamped = std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum));
    return static_cast<int>(std::lround(clamped));
}

QPoint popupPositionForAnchor(QWidget *anchor) {
    if (anchor == nullptr) {
        return {};
    }
    return anchor->mapToGlobal(QPoint(0, anchor->height() + 2));
}

template<typename ImagePointer>
slice_geometry::Dimensions3D imageDimensions(const ImagePointer &image) {
    const auto size = image->GetLargestPossibleRegion().GetSize();
    return slice_geometry::makeDimensions(size[0], size[1], size[2]);
}

template<typename ImagePointer>
bool imageMatchesDimensions(const ImagePointer &image, const slice_geometry::Dimensions3D &expectedDimensions) {
    const auto dimensions = imageDimensions(image);
    return dimensions.x == expectedDimensions.x &&
           dimensions.y == expectedDimensions.y &&
           dimensions.z == expectedDimensions.z;
}

bool loadChoiceRequiresWorkingSegments(ImageLoadChoice loadChoice) {
    switch (loadChoice) {
        case ImageLoadChoice::Supervoxels:
            return false;
        case ImageLoadChoice::Image:
            return false;
        case ImageLoadChoice::Boundaries:
            return false;
        case ImageLoadChoice::Refinement:
            return true;
        case ImageLoadChoice::Segmentation:
            return false;
    }

    return false;
}

}

SignalControl::~SignalControl() {

}

void SignalControl::prepareWorkingSegmentsGraph(const GraphSegmentImageType::Pointer &workingSegmentsImage) {
    graphBase->ignoredSegmentLabels.clear();
    graphBase->edgeStatus.clear();
    graphBase->colorLookUpEdgesStatus.clear();
    graphBase->pGraph->setBackgroundIdStrategy("backgroundIsLowestId");
    graphBase->pWorkingSegmentsImage = workingSegmentsImage;
    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graphBase->pGraph->constructFromVolume(workingSegmentsImage);
}

void SignalControl::showInfoMessage(const QString &message) const {
    QMessageBox msgBox;
    msgBox.setText(message);
    msgBox.exec();
}

std::optional<SignalControl::FloatBoundaryConversionMode> SignalControl::askForFloatBoundaryConversionMode(
    const QString &fileName) const {
    return boundary_conversion_dialog::askForBoundaryConversionMode(
        const_cast<SignalControl *>(this),
        tr("Float Boundary Detected"),
        tr("The loaded boundary is float-valued. How should it be converted to the boundary type?"),
        QFileInfo(fileName).fileName());
}

bool SignalControl::hasWorkingSegments() const {
    return segmentsGraph != nullptr && graphBase->pWorkingSegmentsImage != nullptr;
}

bool SignalControl::hasSelectedSegmentation() const {
    return graphBase->pSelectedSegmentation != nullptr;
}

bool SignalControl::hasSelectedRefinement() const {
    return graphBase->pSelectedRefinement != nullptr;
}

bool SignalControl::hasSelectedBoundary() const {
    return graphBase->pSelectedBoundary != nullptr;
}

SignalControl::ConnectedComponentSplitTarget SignalControl::selectedConnectedComponentSplitTarget() const {
    if (connectedComponentSplitTargetSegmentationAction != nullptr &&
        connectedComponentSplitTargetSegmentationAction->isChecked()) {
        return ConnectedComponentSplitTarget::SelectedSegmentation;
    }
    return ConnectedComponentSplitTarget::InitialSegments;
}

segment_puzzler::connected_components::ConnectivityStencil SignalControl::selectedConnectedComponentConnectivity() const {
    if (connectedComponentSplitConnectivitySixAction != nullptr &&
        connectedComponentSplitConnectivitySixAction->isChecked()) {
        return segment_puzzler::connected_components::ConnectivityStencil::SixConnected;
    }
    return segment_puzzler::connected_components::ConnectivityStencil::Full;
}

bool SignalControl::connectedComponentSplitTargetAvailable(ConnectedComponentSplitTarget target) const {
    switch (target) {
        case ConnectedComponentSplitTarget::InitialSegments:
            return hasWorkingSegments();
        case ConnectedComponentSplitTarget::SelectedSegmentation:
            return hasSelectedSegmentation();
    }
    return false;
}

void SignalControl::ensureConnectedComponentSplitTargetAvailable() {
    if (connectedComponentSplitTargetInitialAction == nullptr ||
        connectedComponentSplitTargetSegmentationAction == nullptr) {
        return;
    }

    if (connectedComponentSplitTargetAvailable(selectedConnectedComponentSplitTarget())) {
        return;
    }
    if (hasWorkingSegments()) {
        connectedComponentSplitTargetInitialAction->setChecked(true);
    } else if (hasSelectedSegmentation()) {
        connectedComponentSplitTargetSegmentationAction->setChecked(true);
    }
}

void SignalControl::updateModeActionTexts() {
    if (toggleROISelectionAction != nullptr) {
        toggleROISelectionAction->setText(roiSelectionActive ? tr("Disable ROI Selection")
                                                             : tr("Enable ROI Selection"));
    }
    if (togglePaintModeAction != nullptr) {
        togglePaintModeAction->setText(paintModeActive ? tr("Disable Paint Mode")
                                                       : tr("Enable Paint Mode"));
    }
}

void SignalControl::setROISelectionActive(bool active) {
    if (roiSelectionActive == active) {
        return;
    }

    roiSelectionActive = active;
    updateModeActionTexts();

    if (roiSelectionActive) {
        orthoViewer->xy->turnROISelectonModeActive();
        orthoViewer->xz->turnROISelectonModeActive();
        orthoViewer->zy->turnROISelectonModeActive();
    } else {
        orthoViewer->xy->turnROISelectonModeInactive();
        orthoViewer->xz->turnROISelectonModeInactive();
        orthoViewer->zy->turnROISelectonModeInactive();
    }
}

void SignalControl::setPaintModeActive(bool active) {
    if (paintModeActive == active) {
        return;
    }

    paintModeActive = active;
    updateModeActionTexts();
    orthoViewer->xy->togglePaintMode();
    orthoViewer->zy->togglePaintMode();
    orthoViewer->xz->togglePaintMode();
}

void SignalControl::setAnnotationToolMode(SliceViewer::ToolMode toolMode) {
    if (orthoViewer == nullptr) {
        return;
    }
    orthoViewer->setAnnotationToolMode(toolMode);
}

void SignalControl::refreshUiState() {
    const bool enabled = !guiBusy;
    ensureConnectedComponentSplitTargetAvailable();

    signalTreeWidget->setEnabled(enabled);
    probabilityTreeWidget->setEnabled(enabled);
    segmentationTreeWidget->setEnabled(enabled);
    refinementTreeWidget->setEnabled(enabled);

    updateLayerSelectionState(signalTreeWidget);
    updateLayerSelectionState(probabilityTreeWidget);
    updateLayerSelectionState(segmentationTreeWidget);
    updateLayerSelectionState(refinementTreeWidget);

    updateSelectionLabel(probabilityTreeWidget, selectedBoundaryLabel);
    updateSelectionLabel(refinementTreeWidget, selectedRefinementLabel);
    updateSelectionLabel(segmentationTreeWidget, selectedSegmentationLabel);

    addImageAction->setEnabled(enabled);
    addSegmentsAction->setEnabled(enabled && !hasWorkingSegments());
    addBoundariesAction->setEnabled(enabled);
    loadRefinementAction->setEnabled(enabled && hasWorkingSegments());
    createEmptySegmentationAction->setEnabled(enabled && hasWorkingSegments());
    loadSegmentationAction->setEnabled(enabled);
    exportSegmentationAction->setEnabled(enabled && hasSelectedSegmentation());
    runWatershedAction->setEnabled(enabled && hasSelectedBoundary());
    mergeWithRefinementAction->setEnabled(enabled && hasSelectedRefinement());
    setIdTransparentAction->setEnabled(enabled && hasSelectedRefinement());
    toggleROISelectionAction->setEnabled(enabled && hasWorkingSegments());
    togglePaintModeAction->setEnabled(enabled && hasSelectedSegmentation());
    setPaintIdAction->setEnabled(enabled && hasSelectedSegmentation());
    dilateSegmentationAction->setEnabled(enabled && hasSelectedSegmentation());
    erodeSegmentationAction->setEnabled(enabled && hasSelectedSegmentation());
    connectedComponentSplitTargetInitialAction->setEnabled(enabled && hasWorkingSegments());
    connectedComponentSplitTargetSegmentationAction->setEnabled(enabled && hasSelectedSegmentation());
    connectedComponentSplitConnectivityFullAction->setEnabled(enabled);
    connectedComponentSplitConnectivitySixAction->setEnabled(enabled);
    connectedComponentSplitAction->setEnabled(
        enabled && connectedComponentSplitTargetAvailable(selectedConnectedComponentSplitTarget()));
    transferWithVolumeAction->setEnabled(enabled && hasSelectedSegmentation());
    transferWithRefinementAction->setEnabled(enabled && hasSelectedSegmentation() && hasSelectedRefinement());
    transferAllAction->setEnabled(enabled && hasSelectedSegmentation());
    schedulePreferredSidebarWidthChanged();
}

int SignalControl::preferredSidebarWidthHint() const {
    int preferredWidth = QWidget::minimumSizeHint().width();
    for (QTreeWidget *tree : {signalTreeWidget, probabilityTreeWidget, segmentationTreeWidget, refinementTreeWidget}) {
        preferredWidth = std::max(preferredWidth, layerTreePreferredWidth(tree));
    }
    return preferredWidth;
}

void SignalControl::schedulePreferredSidebarWidthChanged() {
    if (preferredSidebarWidthChangePending) {
        return;
    }

    preferredSidebarWidthChangePending = true;
    QPointer<SignalControl> guardedThis(this);
    QTimer::singleShot(0, this, [guardedThis]() {
        if (guardedThis == nullptr) {
            return;
        }

        guardedThis->preferredSidebarWidthChangePending = false;
        guardedThis->updateGeometry();
        const int preferredWidth = guardedThis->preferredSidebarWidthHint();
        if (debugLayerLayoutEnabled()) {
            std::cout << QStringLiteral(
                             "[LayerSidebarWidthSource] signalControlWidth=%1 splitterWidth=%2 source=preferredSidebarWidthHint value=%3")
                             .arg(guardedThis->width())
                             .arg(guardedThis->sectionSplitter != nullptr ? guardedThis->sectionSplitter->width() : -1)
                             .arg(preferredWidth)
                             .toStdString()
                      << std::endl;
        }
        if (preferredWidth == guardedThis->lastEmittedPreferredSidebarWidth) {
            return;
        }

        guardedThis->lastEmittedPreferredSidebarWidth = preferredWidth;
        emit guardedThis->preferredSidebarWidthChanged();
    });
}

void SignalControl::updateSelectionLabel(QTreeWidget *tree, QLabel *label) {
    if (tree == nullptr || label == nullptr) {
        return;
    }

    if (tree->topLevelItemCount() == 0) {
        label->setText("Selected: none");
        return;
    }

    // The label is a read-only view of the tree selection. Load/click paths are
    // responsible for keeping the current item set.
    QTreeWidgetItem *currentItem = signal_tree::topLevelSignalItem(tree->currentItem());
    if (currentItem == nullptr) {
        currentItem = tree->topLevelItem(tree->topLevelItemCount() - 1);
    }

    if (itkSignalBase *signal = signalForItem(currentItem)) {
        label->setText(QString("Selected: %1").arg(signal->name));
    } else {
        label->setText("Selected: none");
    }
}

void SignalControl::updateLayerSelectionState(QTreeWidget *tree) {
    if (tree == nullptr) {
        return;
    }

    for (int itemIndex = 0; itemIndex < tree->topLevelItemCount(); ++itemIndex) {
        refreshLayerWidget(tree, tree->topLevelItem(itemIndex));
    }
}

void SignalControl::attachLayerWidgetToItem(QTreeWidget *tree, QTreeWidgetItem *item) {
    if (tree == nullptr || item == nullptr) {
        return;
    }

    auto *layerWidget = new SignalLayerWidget(tree);
    item->setFirstColumnSpanned(true);
    tree->setItemWidget(item, 0, layerWidget);
    SignalLayerWidget::requestHostTreeLayoutSync(tree);
    schedulePreferredSidebarWidthChanged();

    connect(layerWidget, &SignalLayerWidget::sizeHintChanged, this, [this, tree]() {
        SignalLayerWidget::requestHostTreeLayoutSync(tree);
        schedulePreferredSidebarWidthChanged();
    });

    connect(layerWidget, &SignalLayerWidget::activated, this, [this, tree, item]() {
        tree->setCurrentItem(item);
        if (tree == probabilityTreeWidget) {
            boundaryClicked(item, 0);
        } else if (tree == refinementTreeWidget) {
            refinementClicked(item, 0);
        } else if (tree == segmentationTreeWidget) {
            segmentationClicked(item, 0);
        } else {
            updateLayerSelectionState(tree);
        }
    });

    connect(layerWidget, &SignalLayerWidget::renameRequested, this, [this, tree, item]() {
        tree->setCurrentItem(item);
        setDescription(item);
    });

    connect(layerWidget, &SignalLayerWidget::visibilityToggled, this, [this, tree, item](bool visible) {
        setIsActive(item, visible);
        refreshLayerWidget(tree, item);
    });

    connect(layerWidget, &SignalLayerWidget::colorRequested, this, [this, tree, item]() {
        setUserColor(item);
        refreshLayerWidget(tree, item);
    });

    connect(layerWidget, &SignalLayerWidget::contrastRequested, this, [this, item](QWidget *anchor) {
        openNormPopup(item, anchor);
    });

    connect(layerWidget, &SignalLayerWidget::opacityRequested, this, [this, item](QWidget *anchor) {
        openOpacityPopup(item, anchor);
    });

    refreshLayerWidget(tree, item);
    schedulePreferredSidebarWidthChanged();
    if (debugLayerLayoutEnabled()) {
        const int viewportWidth = tree->viewport() != nullptr ? tree->viewport()->width() : -1;
        const bool scrollbarVisible = tree->verticalScrollBar() != nullptr && tree->verticalScrollBar()->isVisible();
        std::cout << QStringLiteral(
                         "[LayerTreeAttach] tree=%1 signalControlWidth=%2 splitterWidth=%3 treeWidth=%4 viewport=%5 "
                         "scrollbarVisible=%6 widthSource=preferredSidebarWidthHint sourceValue=%7")
                         .arg(debugTreeName(tree))
                         .arg(width())
                         .arg(sectionSplitter != nullptr ? sectionSplitter->width() : -1)
                         .arg(tree->width())
                         .arg(viewportWidth)
                         .arg(scrollbarVisible)
                         .arg(preferredSidebarWidthHint())
                         .toStdString()
                  << std::endl;
    }
}

void SignalControl::attachLayerWidgetToLastItem(QTreeWidget *tree) {
    if (tree == nullptr || tree->topLevelItemCount() <= 0) {
        return;
    }
    attachLayerWidgetToItem(tree, tree->topLevelItem(tree->topLevelItemCount() - 1));
}

void SignalControl::refreshLayerWidget(QTreeWidget *tree, QTreeWidgetItem *item) {
    if (tree == nullptr || item == nullptr) {
        return;
    }

    auto *layerWidget = layerWidgetForItem(tree, item);
    if (layerWidget == nullptr) {
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }

    itkSignalBase *signal = allSignalList[signalIndex];
    const QString name = signal->name;
    const bool active = signal->getIsActive();

    item->setText(0, QString());
    item->setText(1, QString());

    SignalLayerWidget::Presentation presentation;
    presentation.layerName = name;
    presentation.layerColor = QColor::fromRgba(signal->getColor());
    presentation.usesCategoricalPalette = signal->usesCategoricalLUT();
    presentation.usesEdgeStatusColors = signal->usesEdgeStatusColors();
    presentation.layerVisible = active;
    presentation.selected = item == signal_tree::topLevelSignalItem(tree->currentItem());
    presentation.contrastText = contrastChipText(signal);
    presentation.opacityText = opacityChipText(signal);
    presentation.toolTip = layerToolTipText(signal);
    presentation.contrastAvailable = signal->supportsNormControl();
    layerWidget->applyPresentation(presentation);

    if (debugLayerLayoutEnabled()) {
        const int viewportWidth = tree->viewport() != nullptr ? tree->viewport()->width() : -1;
        const bool scrollbarVisible = tree->verticalScrollBar() != nullptr && tree->verticalScrollBar()->isVisible();
        std::cout << QStringLiteral(
                         "[LayerTreeRefresh] tree=%1 name=\"%2\" signalControlWidth=%3 splitterWidth=%4 treeWidth=%5 "
                         "viewport=%6 scrollbarVisible=%7 widthSource=preferredSidebarWidthHint sourceValue=%8")
                         .arg(debugTreeName(tree))
                         .arg(name)
                         .arg(width())
                         .arg(sectionSplitter != nullptr ? sectionSplitter->width() : -1)
                         .arg(tree->width())
                         .arg(viewportWidth)
                         .arg(scrollbarVisible)
                         .arg(preferredSidebarWidthHint())
                         .toStdString()
                  << std::endl;
    }
}

void SignalControl::openNormPopup(QTreeWidgetItem *item, QWidget *anchor) {
    if (item == nullptr || anchor == nullptr) {
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }

    itkSignalBase *signal = allSignalList[signalIndex];
    if (!signal->supportsNormControl()) {
        return;
    }

    auto *tree = item->treeWidget();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto *action = new QWidgetAction(menu);
    auto *container = new QWidget(menu);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *lowerSlider = new QSlider(Qt::Horizontal, container);
    auto *upperSlider = new QSlider(Qt::Horizontal, container);
    auto *lowerSpinBox = new QSpinBox(container);
    auto *upperSpinBox = new QSpinBox(container);
    auto *autoButton = new QPushButton("Auto", container);
    auto *resetButton = new QPushButton("Reset", container);

    const int minimumBound = clampPopupValue(signal->getMinimumValueAsDouble(),
                                             std::numeric_limits<int>::min(),
                                             std::numeric_limits<int>::max());
    const int maximumBound = clampPopupValue(signal->getMaximumValueAsDouble(),
                                             std::numeric_limits<int>::min(),
                                             std::numeric_limits<int>::max());

    const int popupMinimum = std::min(minimumBound, maximumBound);
    const int popupMaximum = std::max(minimumBound, maximumBound);

    lowerSlider->setRange(popupMinimum, popupMaximum);
    upperSlider->setRange(popupMinimum, popupMaximum);
    lowerSpinBox->setRange(popupMinimum, popupMaximum);
    upperSpinBox->setRange(popupMinimum, popupMaximum);

    auto *lowerRow = new QWidget(container);
    auto *lowerLayout = new QHBoxLayout(lowerRow);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    lowerLayout->setSpacing(6);
    lowerLayout->addWidget(new QLabel("Min", lowerRow));
    lowerLayout->addWidget(lowerSlider, 1);
    lowerLayout->addWidget(lowerSpinBox);

    auto *upperRow = new QWidget(container);
    auto *upperLayout = new QHBoxLayout(upperRow);
    upperLayout->setContentsMargins(0, 0, 0, 0);
    upperLayout->setSpacing(6);
    upperLayout->addWidget(new QLabel("Max", upperRow));
    upperLayout->addWidget(upperSlider, 1);
    upperLayout->addWidget(upperSpinBox);

    layout->addWidget(lowerRow);
    layout->addWidget(upperRow);

    auto *buttonRow = new QWidget(container);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(6);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(autoButton);
    buttonLayout->addWidget(resetButton);
    layout->addWidget(buttonRow);

    const int initialLower = clampPopupValue(signal->getNormLower(), popupMinimum, popupMaximum);
    const int initialUpper = clampPopupValue(signal->getNormUpper(), popupMinimum, popupMaximum);

    auto applyNorm = [this, tree, item, signal, lowerSlider, upperSlider, lowerSpinBox, upperSpinBox](int lower, int upper) {
        lower = std::min(lower, upper);
        upper = std::max(lower, upper);

        const QSignalBlocker lowerSliderBlocker(lowerSlider);
        const QSignalBlocker upperSliderBlocker(upperSlider);
        const QSignalBlocker lowerSpinBlocker(lowerSpinBox);
        const QSignalBlocker upperSpinBlocker(upperSpinBox);

        lowerSlider->setValue(lower);
        upperSlider->setValue(upper);
        lowerSpinBox->setValue(lower);
        upperSpinBox->setValue(upper);

        signal->setNorm(lower, upper);
        refreshLayerWidget(tree, item);
        refreshViewers();
    };

    {
        const int clampedLower = std::min(initialLower, initialUpper);
        const int clampedUpper = std::max(initialLower, initialUpper);
        const QSignalBlocker lowerSliderBlocker(lowerSlider);
        const QSignalBlocker upperSliderBlocker(upperSlider);
        const QSignalBlocker lowerSpinBlocker(lowerSpinBox);
        const QSignalBlocker upperSpinBlocker(upperSpinBox);
        lowerSlider->setValue(clampedLower);
        upperSlider->setValue(clampedUpper);
        lowerSpinBox->setValue(clampedLower);
        upperSpinBox->setValue(clampedUpper);
    }

    connect(lowerSlider, &QSlider::valueChanged, this, [applyNorm, upperSpinBox](int value) {
        applyNorm(std::min(value, upperSpinBox->value()), upperSpinBox->value());
    });
    connect(upperSlider, &QSlider::valueChanged, this, [applyNorm, lowerSpinBox](int value) {
        applyNorm(lowerSpinBox->value(), std::max(value, lowerSpinBox->value()));
    });
    connect(lowerSpinBox, qOverload<int>(&QSpinBox::valueChanged), this, [applyNorm, upperSpinBox](int value) {
        applyNorm(std::min(value, upperSpinBox->value()), upperSpinBox->value());
    });
    connect(upperSpinBox, qOverload<int>(&QSpinBox::valueChanged), this, [applyNorm, lowerSpinBox](int value) {
        applyNorm(lowerSpinBox->value(), std::max(value, lowerSpinBox->value()));
    });
    connect(autoButton, &QPushButton::clicked, this, [applyNorm, signal, popupMinimum, popupMaximum]() {
        double autoLower = static_cast<double>(popupMinimum);
        double autoUpper = static_cast<double>(popupMaximum);
        if (!signal->computeNextAutoContrastRange(autoLower, autoUpper)) {
            autoLower = static_cast<double>(popupMinimum);
            autoUpper = static_cast<double>(popupMaximum);
        }

        applyNorm(clampPopupValue(autoLower, popupMinimum, popupMaximum),
                  clampPopupValue(autoUpper, popupMinimum, popupMaximum));
    });
    connect(resetButton, &QPushButton::clicked, this, [applyNorm, signal, popupMinimum, popupMaximum]() {
        applyNorm(popupMinimum, popupMaximum);
        signal->resetAutoContrastState();
    });

    action->setDefaultWidget(container);
    menu->addAction(action);
    menu->popup(popupPositionForAnchor(anchor));
}

void SignalControl::openOpacityPopup(QTreeWidgetItem *item, QWidget *anchor) {
    if (item == nullptr || anchor == nullptr) {
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }

    itkSignalBase *signal = allSignalList[signalIndex];
    auto *tree = item->treeWidget();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto *action = new QWidgetAction(menu);
    auto *container = new QWidget(menu);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *slider = new QSlider(Qt::Horizontal, container);
    auto *spinBox = new QSpinBox(container);
    slider->setRange(0, 100);
    spinBox->setRange(0, 100);
    spinBox->setSuffix("%");

    auto *row = new QWidget(container);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    rowLayout->addWidget(new QLabel("Opacity", row));
    rowLayout->addWidget(slider, 1);
    rowLayout->addWidget(spinBox);
    layout->addWidget(row);

    auto applyOpacity = [this, tree, item, signal, slider, spinBox](int value) {
        const int clampedValue = std::clamp(value, 0, 100);
        const QSignalBlocker sliderBlocker(slider);
        const QSignalBlocker spinBlocker(spinBox);
        slider->setValue(clampedValue);
        spinBox->setValue(clampedValue);
        signal->setAlpha(percentToAlpha(clampedValue));
        refreshLayerWidget(tree, item);
        refreshViewers();
    };

    {
        const int initialOpacityPercent = alphaToPercent(signal->getAlpha());
        const QSignalBlocker sliderBlocker(slider);
        const QSignalBlocker spinBlocker(spinBox);
        slider->setValue(initialOpacityPercent);
        spinBox->setValue(initialOpacityPercent);
    }

    connect(slider, &QSlider::valueChanged, this, applyOpacity);
    connect(spinBox, qOverload<int>(&QSpinBox::valueChanged), this, applyOpacity);

    action->setDefaultWidget(container);
    menu->addAction(action);
    menu->popup(popupPositionForAnchor(anchor));
}

void SignalControl::selectLoadedItemIfAppropriate(QTreeWidget *tree,
                                                  QTreeWidgetItem *newItem,
                                                  QTreeWidgetItem *&lastAutoSelectedItem) {
    if (tree == nullptr || newItem == nullptr) {
        return;
    }

    // Each section auto-follows the latest loaded item until the user clicks a
    // different current item. The trees are append-only in the current UI, so
    // tracking the last auto-selected item is enough here.
    QTreeWidgetItem *currentItem = signal_tree::topLevelSignalItem(tree->currentItem());
    if (currentItem == nullptr || currentItem == lastAutoSelectedItem) {
        tree->setCurrentItem(newItem);
        lastAutoSelectedItem = newItem;
    }
}

void SignalControl::selectBoundaryItem(QTreeWidgetItem *item) {
    if (signal_tree::topLevelSignalItem(item) == nullptr) {
        graphBase->pSelectedBoundary = nullptr;
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    graphBase->pSelectedBoundary = dynamic_cast<dataType::BoundaryImageType *>(
        allSignalList[signalIndex]->getImageBase().GetPointer());
}

void SignalControl::selectRefinementItem(QTreeWidgetItem *item) {
    if (signal_tree::topLevelSignalItem(item) == nullptr) {
        graphBase->pSelectedRefinement = nullptr;
        graphBase->pSelectedRefinementSignal = nullptr;
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    graphBase->pSelectedRefinement = dynamic_cast<GraphSegmentImageType *>(
        allSignalList[signalIndex]->getImageBase().GetPointer());
    graphBase->pSelectedRefinementSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(
        allSignalList[signalIndex]);
}

void SignalControl::selectSegmentationItem(QTreeWidgetItem *item) {
    if (signal_tree::topLevelSignalItem(item) == nullptr) {
        graphBase->pSelectedSegmentation = nullptr;
        graphBase->pSelectedSegmentationSignal = nullptr;
        graphBase->selectedSegmentationMaxSegmentId = 0;
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    graphBase->pSelectedSegmentation = dynamic_cast<GraphSegmentImageType *>(
        allSignalList[signalIndex]->getImageBase().GetPointer());
    graphBase->pSelectedSegmentationSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(
        allSignalList[signalIndex]);
    graphBase->selectedSegmentationMaxSegmentId =
        graphBase->pGraph->getLargestIdInSegmentVolume(graphBase->pSelectedSegmentation);
}

void SignalControl::setGuiBusy(bool busy) {
    guiBusy = busy;
    refreshUiState();
}

void SignalControl::refreshViewers() {
    orthoViewer->refreshViewers();
}

QString SignalControl::resolvedDisplayName(const QString &fileName, const QString &displayedName) const {
    const QString requestedName = !displayedName.isEmpty() ? displayedName : QFileInfo(fileName).baseName();
    return signal_name_utils::makeUniqueSignalName(allSignalList, requestedName);
}

std::optional<slice_geometry::Dimensions3D> SignalControl::expectedDimensionsForNewSignal(
    bool forceShapeOfSegments) const {
    itkSignalBase *referenceSignal = nullptr;
    if (segmentsGraph != nullptr && forceShapeOfSegments) {
        referenceSignal = segmentsGraph;
    } else if (segmentsGraph == nullptr && !allSignalList.empty()) {
        referenceSignal = allSignalList.front();
    }

    if (referenceSignal == nullptr) {
        return std::nullopt;
    }

    return slice_geometry::makeDimensions(
        referenceSignal->getDimX(),
        referenceSignal->getDimY(),
        referenceSignal->getDimZ());
}

bool SignalControl::dimensionsMatchExpectedDimensions(unsigned long dimX,
                                                      unsigned long dimY,
                                                      unsigned long dimZ,
                                                      bool forceShapeOfSegments) const {
    const auto expectedDimensions = expectedDimensionsForNewSignal(forceShapeOfSegments);
    if (!expectedDimensions) {
        return true;
    }

    return dimX == expectedDimensions->x &&
           dimY == expectedDimensions->y &&
           dimZ == expectedDimensions->z;
}

void SignalControl::reportDimensionMismatch(unsigned long dimX,
                                            unsigned long dimY,
                                            unsigned long dimZ,
                                            bool forceShapeOfSegments) const {
    const auto expectedDimensions = expectedDimensionsForNewSignal(forceShapeOfSegments);
    if (!expectedDimensions) {
        return;
    }

    const QString referenceName =
        (segmentsGraph != nullptr && forceShapeOfSegments)
            ? tr("loaded supervoxels")
            : tr("the first loaded layer");

    SP_LOG_WARNING(
        "io",
        QStringLiteral("Dimension mismatch while adding volume. reference=[%1 %2 %3] loaded=[%4 %5 %6]")
            .arg(expectedDimensions->x)
            .arg(expectedDimensions->y)
            .arg(expectedDimensions->z)
            .arg(dimX)
            .arg(dimY)
            .arg(dimZ));

    showInfoMessage(
        tr("The loaded volume dimensions do not match %1.\n\nExpected: %2 x %3 x %4\nLoaded: %5 x %6 x %7")
            .arg(referenceName)
            .arg(expectedDimensions->x)
            .arg(expectedDimensions->y)
            .arg(expectedDimensions->z)
            .arg(dimX)
            .arg(dimY)
            .arg(dimZ));
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
    SP_LOG_INFO("io", QStringLiteral("Reading image %1").arg(fileName));
    getDimensionAndDataTypeOfFile(fileName, dimension, loadedImage.dataType);
    SP_LOG_INFO("io", QStringLiteral("Detected image dimension=%1 for %2").arg(dimension).arg(fileName));

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
    const bool centerFirstStandaloneLayer = !hasWorkingSegments() && allSignalList.size() == 1;
    allSignalList[signalIndexGlobal]->setLUTContinuous();
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    attachLayerWidgetToLastItem(signalTreeWidget);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    if (centerFirstStandaloneLayer) {
        orthoViewer->setViewToMiddleOfStack();
    }
    refreshUiState();
}

void SignalControl::registerSegmentationSignal(size_t signalIndexGlobal, const QString &name) {
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setIsActive(true);
    attachLayerWidgetToLastItem(segmentationTreeWidget);
    QTreeWidgetItem *newItem = segmentationTreeWidget->topLevelItem(segmentationTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(segmentationTreeWidget, newItem, lastAutoSelectedSegmentationItem);

    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectSegmentationItem(segmentationTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::registerBoundarySignal(size_t signalIndexGlobal, const QString &name) {
    allSignalList[signalIndexGlobal]->setLUTContinuous();
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setupTreeWidget(probabilityTreeWidget, signalIndexGlobal);
    attachLayerWidgetToLastItem(probabilityTreeWidget);
    QTreeWidgetItem *newItem = probabilityTreeWidget->topLevelItem(probabilityTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(probabilityTreeWidget, newItem, lastAutoSelectedBoundaryItem);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectBoundaryItem(probabilityTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::registerRefinementSignal(size_t signalIndexGlobal, const QString &name) {
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setupTreeWidget(refinementTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setIsActive(false);
    attachLayerWidgetToLastItem(refinementTreeWidget);
    QTreeWidgetItem *newItem = refinementTreeWidget->topLevelItem(refinementTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(refinementTreeWidget, newItem, lastAutoSelectedRefinementItem);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectRefinementItem(refinementTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::registerSegmentsGraphSignal(size_t signalIndexGlobal, bool createSegmentationVolume) {
    segmentsGraph = allSignalList[signalIndexGlobal];
    auto *typedSegmentsSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(segmentsGraph);
    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setName(
        signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Supervoxels")));
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    attachLayerWidgetToLastItem(signalTreeWidget);

    graphBase->pWorkingSegments = typedSegmentsSignal;
    graphBase->pWorkingSegmentsImage = typedSegmentsSignal->pImage;

    allSignalList[signalIndexGlobal]->setLUTValueToBlack(graphBase->ignoredSegmentLabels.front());
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);

    const size_t edgeSignalIndex = allSignalList.size();
    graphBase->pEdgesInitialSegmentsITKSignal->setName(
        signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Edges")));
    allSignalList.push_back(graphBase->pEdgesInitialSegmentsITKSignal);
    graphBase->pEdgesInitialSegmentsITKSignal->setupTreeWidget(signalTreeWidget, edgeSignalIndex);
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    graphBase->pEdgesInitialSegmentsITKSignal->setIsActive(false);
    attachLayerWidgetToLastItem(signalTreeWidget);
    orthoViewer->addSignal(graphBase->pEdgesInitialSegmentsITKSignal);

    orthoViewer->setViewToMiddleOfStack();
    if (createSegmentationVolume) {
        createEmptySegmentation();
    }
    refreshUiState();
}

void SignalControl::addImageAsync(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Loading image..."),
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

SignalControl::GraphSegmentImageType::Pointer SignalControl::duplicateSegmentationAndBuildWorkingSegments(
    const GraphSegmentImageType::Pointer &segmentationImage) {
    auto workingSegmentsImage = duplicateSegmentsImage(segmentationImage);
    prepareWorkingSegmentsGraph(workingSegmentsImage);
    return workingSegmentsImage;
}

void SignalControl::loadSegmentationVolume(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    bool createWorkingSegments = false;
    if (!hasWorkingSegments()) {
        askForBackgroundStrategy();
        QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            tr("No Supervoxels Loaded"),
            tr("No supervoxels are loaded. Do you want to duplicate the loaded segmentation as supervoxels too?"),
            QMessageBox::Yes | QMessageBox::No);
        createWorkingSegments = reply == QMessageBox::Yes;
    }

    loadSegmentationVolumeAsync(fileName, displayedName, createWorkingSegments, std::move(then));
}

void SignalControl::loadSegmentationVolumeAsync(QString fileName,
                                                QString displayedName,
                                                bool createWorkingSegments,
                                                LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const bool hadWorkingSegments = hasWorkingSegments();
    const auto expectedDimensions = expectedDimensionsForNewSignal(hadWorkingSegments);
    taskRunner->runWithLabel(
        QStringLiteral("Loading segmentation..."),
        [this, fileName, createWorkingSegments, hadWorkingSegments, expectedDimensions]() mutable {
            SegmentationLoadResultData result;
            result.segmentationImage = ITKImageLoader<GraphSegmentType>(fileName);
            if (expectedDimensions && !imageMatchesDimensions(result.segmentationImage, *expectedDimensions)) {
                result.dimensionMismatch = true;
                return result;
            }
            if (!hadWorkingSegments) {
                graphBase->pGraph->updateBackgroundIdFromVolume(result.segmentationImage);
            }
            if (createWorkingSegments) {
                result.workingSegmentsImage = duplicateSegmentationAndBuildWorkingSegments(result.segmentationImage);
            }
            return result;
        },
        [this, fileName, displayedName, hadWorkingSegments, then = std::move(then)](SegmentationLoadResultData result) mutable {
            if (result.dimensionMismatch && result.segmentationImage != nullptr) {
                const auto dimensions = imageDimensions(result.segmentationImage);
                reportDimensionMismatch(dimensions.x, dimensions.y, dimensions.z, hadWorkingSegments);
                invokeLoadCallbackLater(std::move(then), std::nullopt);
                return;
            }

            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(result.segmentationImage, signalIndexGlobal, hadWorkingSegments);
            if (ok) {
                if (result.workingSegmentsImage != nullptr) {
                    size_t segmentIndexGlobal = 0;
                    const bool insertedSegments = insertImageSegmenttype(result.workingSegmentsImage, segmentIndexGlobal, false);
                    if (insertedSegments) {
                        registerSegmentsGraphSignal(segmentIndexGlobal, false);
                    }
                }
                registerSegmentationSignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName));
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::loadRefinementAsync(QString fileName, QString displayedName, LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Loading refinement..."),
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

    struct SegmentsGraphLoadResult {
        GraphSegmentImageType::Pointer image;
        bool dimensionMismatch = false;
    };

    const auto expectedDimensions = expectedDimensionsForNewSignal(false);
    taskRunner->runWithLabel(
        QStringLiteral("Loading supervoxels and building graph..."),
        [this, fileName, expectedDimensions]() mutable {
            SegmentsGraphLoadResult result;
            // Modifies graphBase on the worker thread. Safe only because
            // one task runs at a time and the owning window is blocked.
            auto pImage = ITKImageLoader<GraphSegmentType>(fileName);
            result.image = pImage;
            if (expectedDimensions && !imageMatchesDimensions(pImage, *expectedDimensions)) {
                result.dimensionMismatch = true;
                return result;
            }
            graphBase->ignoredSegmentLabels.clear();
            graphBase->edgeStatus.clear();
            graphBase->colorLookUpEdgesStatus.clear();
            graphBase->pWorkingSegmentsImage = pImage;
            graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
            graphBase->pGraph->constructFromVolume(pImage);
            return result;
        },
        [this, then = std::move(then)](SegmentsGraphLoadResult result) mutable {
            if (result.dimensionMismatch && result.image != nullptr) {
                const auto dimensions = imageDimensions(result.image);
                reportDimensionMismatch(dimensions.x, dimensions.y, dimensions.z, false);
                invokeLoadCallbackLater(std::move(then), std::nullopt);
                return;
            }

            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(result.image, signalIndexGlobal, false);
            if (ok) {
                registerSegmentsGraphSignal(signalIndexGlobal);
            } else {
                QMessageBox::critical(this, tr("Error"), tr("Failed to load the supervoxels."));
            }
            invokeLoadCallbackLater(std::move(then), ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        });
}

void SignalControl::loadMembraneProbabilityAsync(QString fileName,
                                                 QString displayedName,
                                                 BoundaryLoadMode loadMode,
                                                 FloatBoundaryConversionMode floatConversionMode,
                                                 LoadCallback then) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const bool createEmptySegments = loadMode == BoundaryLoadMode::CreateEmptySegments;
    const auto expectedDimensions = expectedDimensionsForNewSignal(false);
    taskRunner->runWithLabel(
        QStringLiteral("Loading boundaries..."),
        [this, fileName, createEmptySegments, floatConversionMode, expectedDimensions]() mutable {
            BoundaryLoadResult result;
            unsigned int dimension = 0;
            itk::ImageIOBase::IOComponentType boundaryDataType = itk::ImageIOBase::UNKNOWNCOMPONENTTYPE;
            getDimensionAndDataTypeOfFile(fileName, dimension, boundaryDataType);

            switch (boundaryDataType) {
                case itk::ImageIOBase::FLOAT:
                    result.boundaryImage = convertFloatBoundaryImage<float>(
                        ITKImageLoader<float>(fileName),
                        floatConversionMode);
                    break;
                case itk::ImageIOBase::DOUBLE:
                    result.boundaryImage = convertFloatBoundaryImage<double>(
                        ITKImageLoader<double>(fileName),
                        floatConversionMode);
                    break;
                default:
                    result.boundaryImage = ITKImageLoader<dataType::BoundaryVoxelType>(fileName);
                    break;
            }
            if (createEmptySegments) {
                if (expectedDimensions && !imageMatchesDimensions(result.boundaryImage, *expectedDimensions)) {
                    return result;
                }

                result.emptySegmentsImage = dataType::SegmentsImageType::New();
                result.emptySegmentsImage->SetRegions(result.boundaryImage->GetLargestPossibleRegion());
                result.emptySegmentsImage->SetSpacing(result.boundaryImage->GetSpacing());
                result.emptySegmentsImage->SetOrigin(result.boundaryImage->GetOrigin());
                result.emptySegmentsImage->Allocate(true);

                // Modifies graphBase on the worker thread. Safe only because
                // one task runs at a time and the owning window is blocked.
                prepareWorkingSegmentsGraph(result.emptySegmentsImage);
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
    SP_LOG_INFO("io", QStringLiteral("Adding image file %1").arg(fileName));
    addImageAsync(fileName, displayedName);
}

void SignalControl::populateAddDataMenu(QMenu *menu, QAction *loadSampleDataAction) {
    if (menu == nullptr) {
        return;
    }

    if (loadSampleDataAction != nullptr) {
        menu->addAction(loadSampleDataAction);
        menu->addSeparator();
    }
    menu->addAction(addSegmentsAction);
    menu->addAction(addImageAction);
    menu->addAction(addBoundariesAction);
    menu->addAction(loadRefinementAction);
    menu->addAction(loadSegmentationAction);
    menu->addSeparator();
    menu->addAction(createEmptySegmentationAction);
}

void SignalControl::populateBoundariesMenu(QMenu *menu) {
    if (menu == nullptr) {
        return;
    }

    menu->addAction(addBoundariesAction);
    menu->addSeparator();
    menu->addAction(toggleROISelectionAction);
    menu->addAction(runWatershedAction);
}

void SignalControl::populateRefinementsMenu(QMenu *menu) {
    if (menu == nullptr) {
        return;
    }

    menu->addAction(loadRefinementAction);
    menu->addSeparator();
    menu->addAction(mergeWithRefinementAction);
    menu->addAction(setIdTransparentAction);
}

void SignalControl::populateSegmentationsMenu(QMenu *menu) {
    if (menu == nullptr) {
        return;
    }

    menu->addAction(loadSegmentationAction);
    menu->addAction(createEmptySegmentationAction);
    menu->addAction(exportSegmentationAction);
    menu->addSeparator();
    menu->addAction(togglePaintModeAction);
    menu->addAction(setPaintIdAction);
    menu->addAction(dilateSegmentationAction);
    menu->addAction(erodeSegmentationAction);
    menu->addSeparator();
    QMenu *connectedComponentMenu = menu->addMenu(tr("Connected Component Split"));
    connectedComponentMenu->addAction(connectedComponentSplitAction);
    connectedComponentMenu->addSeparator();
    connectedComponentMenu->addAction(connectedComponentSplitTargetInitialAction);
    connectedComponentMenu->addAction(connectedComponentSplitTargetSegmentationAction);
    connectedComponentMenu->addSeparator();
    connectedComponentMenu->addAction(connectedComponentSplitConnectivityFullAction);
    connectedComponentMenu->addAction(connectedComponentSplitConnectivitySixAction);
    menu->addSeparator();
    menu->addAction(transferWithVolumeAction);
    menu->addAction(transferAllAction);
    menu->addAction(transferWithRefinementAction);
}

void SignalControl::createMenuActions() {
    auto createAction = [this](QAction *&action, const QString &text, auto slot) {
        action = new QAction(text, this);
        connect(action, &QAction::triggered, this, slot);
    };

    createAction(addImageAction, tr("Load Image"), &SignalControl::addImagePressed);
    createAction(addSegmentsAction, tr("Load Supervoxels"), &SignalControl::addSegmentsPressed);
    createAction(addBoundariesAction, tr("Load Boundaries"), &SignalControl::loadMembraneProbabilityPressed);
    createAction(loadRefinementAction, tr("Load Refinement"), &SignalControl::loadRefinementPressed);
    createAction(createEmptySegmentationAction, tr("Create Empty Segmentation"), &SignalControl::createEmptySegmentation);
    createAction(loadSegmentationAction, tr("Load Segmentation"), &SignalControl::loadSegmentationVolumePressed);
    createAction(exportSegmentationAction, tr("Export Selected Segmentation"), &SignalControl::exportSelectedSegmentation);
    createAction(runWatershedAction, tr("Run Watershed"), &SignalControl::runWatershed);
    createAction(mergeWithRefinementAction, tr("Merge Supervoxels that Share a Refinement Label"), &SignalControl::mergeSupervoxelsByRefinementLabel);
    createAction(setIdTransparentAction, tr("Set Transparent Label ID in Refinement"), &SignalControl::setTransparentLabelIdInRefinement);
    createAction(toggleROISelectionAction, QString(), &SignalControl::toggleROISelection);
    createAction(togglePaintModeAction, QString(), &SignalControl::togglePaintMode);
    createAction(setPaintIdAction, tr("Set Paint Label ID"), &SignalControl::setPaintId);
    createAction(dilateSegmentationAction, tr("Dilate Label One Step"), &SignalControl::activateDilateTool);
    createAction(erodeSegmentationAction, tr("Erode Label One Step"), &SignalControl::activateErodeTool);
    createAction(connectedComponentSplitAction, tr("Run Connected Component Split"), &SignalControl::runConnectedComponentSplit);
    connectedComponentSplitAction->setShortcut(Qt::Key_F7);
    createAction(transferWithVolumeAction, tr("Transfer Supervoxels by Volume"), &SignalControl::transferSegmentsWithVolume);
    createAction(transferAllAction, tr("Transfer All Supervoxels"), &SignalControl::transferAllSegments);
    createAction(transferWithRefinementAction, tr("Transfer Supervoxels by Refinement Overlap"), &SignalControl::transferSupervoxelsByRefinementOverlap);

    connectedComponentSplitTargetGroup = new QActionGroup(this);
    connectedComponentSplitTargetInitialAction = new QAction(tr("Target: Initial Segments"), this);
    connectedComponentSplitTargetSegmentationAction = new QAction(tr("Target: Selected Segmentation"), this);
    connectedComponentSplitTargetInitialAction->setCheckable(true);
    connectedComponentSplitTargetSegmentationAction->setCheckable(true);
    connectedComponentSplitTargetGroup->addAction(connectedComponentSplitTargetInitialAction);
    connectedComponentSplitTargetGroup->addAction(connectedComponentSplitTargetSegmentationAction);
    connectedComponentSplitTargetInitialAction->setChecked(true);
    connect(connectedComponentSplitTargetInitialAction, &QAction::triggered, this, &SignalControl::refreshUiState);
    connect(connectedComponentSplitTargetSegmentationAction, &QAction::triggered, this, &SignalControl::refreshUiState);

    connectedComponentSplitConnectivityGroup = new QActionGroup(this);
    connectedComponentSplitConnectivityFullAction = new QAction(tr("Connectivity: Full"), this);
    connectedComponentSplitConnectivitySixAction = new QAction(tr("Connectivity: 6-Connected"), this);
    connectedComponentSplitConnectivityFullAction->setCheckable(true);
    connectedComponentSplitConnectivitySixAction->setCheckable(true);
    connectedComponentSplitConnectivityGroup->addAction(connectedComponentSplitConnectivityFullAction);
    connectedComponentSplitConnectivityGroup->addAction(connectedComponentSplitConnectivitySixAction);
    connectedComponentSplitConnectivityFullAction->setChecked(true);
    updateModeActionTexts();
}

QVBoxLayout *SignalControl::createSectionLayout(const QString &title) {
    auto *sectionWidget = new QWidget(sectionSplitter);
    sectionWidget->setMinimumHeight(0);
    sectionWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *sectionLayout = new QVBoxLayout(sectionWidget);
    sectionLayout->setContentsMargins(4, 4, 4, 4);
    sectionLayout->setSpacing(kSectionSpacing);
    sectionLayout->addWidget(createSectionLabel(title, sectionWidget));
    sectionSplitter->addWidget(sectionWidget);
    return sectionLayout;
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
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    sectionSplitter = new QSplitter(Qt::Vertical, this);
    sectionSplitter->setChildrenCollapsible(false);
    mainLayout->addWidget(sectionSplitter);

    createMenuActions();

    setupSignalTreeWidget();
    setupProbabilityTreeWidget();
    setupRefinementTreeWidget();
    setupSegmentationTreeWidget();

    sectionSplitter->setStretchFactor(0, kLayersVisibleRows);
    sectionSplitter->setStretchFactor(1, kOtherSectionVisibleRows);
    sectionSplitter->setStretchFactor(2, kOtherSectionVisibleRows);
    sectionSplitter->setStretchFactor(3, kOtherSectionVisibleRows);

    connect(taskRunner, &TaskRunner::busyChanged, this, &SignalControl::setGuiBusy);
    setGuiBusy(taskRunner->isBusy());
}

void SignalControl::setupSignalTreeWidget() {
    auto *signalWidgetLayout = createSectionLayout(tr("Layers"));

    signalTreeWidget = new QTreeWidget();
    signalTreeWidget->setObjectName(QStringLiteral("layersTree"));
    signalTreeWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    signalTreeWidget->setUniformRowHeights(true);
    signalTreeWidget->setFocusPolicy(Qt::NoFocus);
    signalTreeWidget->setColumnCount(2);
    signalTreeWidget->setHeaderHidden(true);
    signalTreeWidget->setRootIsDecorated(false);
    signalTreeWidget->setIndentation(0);
    SignalLayerWidget::configureHostTree(signalTreeWidget);
    setTreeMinimumRows(signalTreeWidget, kLayersVisibleRows);
    // Extra vertical space should go into the tree, not the section title.
    signalWidgetLayout->addWidget(signalTreeWidget, 1);

    connect(signalTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(signalTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::treeClicked);
    connect(signalTreeWidget, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        updateLayerSelectionState(signalTreeWidget);
    });
}


void SignalControl::setupProbabilityTreeWidget() {
    auto *probabilityWidgetLayout = createSectionLayout(tr("Boundaries"));

    probabilityTreeWidget = new QTreeWidget();
    probabilityTreeWidget->setObjectName(QStringLiteral("boundariesTree"));
    probabilityTreeWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    probabilityTreeWidget->setUniformRowHeights(true);
    probabilityTreeWidget->setFocusPolicy(Qt::NoFocus);
    probabilityTreeWidget->setColumnCount(2);
    probabilityTreeWidget->setHeaderHidden(true);
    probabilityTreeWidget->setRootIsDecorated(false);
    probabilityTreeWidget->setIndentation(0);
    SignalLayerWidget::configureHostTree(probabilityTreeWidget);
    setTreeMinimumRows(probabilityTreeWidget, kOtherSectionVisibleRows);
    probabilityWidgetLayout->addWidget(probabilityTreeWidget, 1);

    selectedBoundaryLabel = new QLabel("Selected: none");
    selectedBoundaryLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    configureSelectionLabel(selectedBoundaryLabel);
    probabilityWidgetLayout->addWidget(selectedBoundaryLabel);

    auto *boundaryButtonRow = new QHBoxLayout();
    boundaryButtonRow->setContentsMargins(0, 0, 0, 0);
    boundaryButtonRow->setSpacing(kSectionSpacing);

    toggleROISelectionButton = new QPushButton();
    runWatershedButton = new QPushButton();
    toggleROISelectionButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    runWatershedButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    boundaryButtonRow->addWidget(toggleROISelectionButton);
    boundaryButtonRow->addWidget(runWatershedButton);
    boundaryButtonRow->addStretch();
    probabilityWidgetLayout->addLayout(boundaryButtonRow);

    bindButtonToAction(toggleROISelectionButton, toggleROISelectionAction);
    bindButtonToAction(runWatershedButton, runWatershedAction);


    connect(probabilityTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(probabilityTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::boundaryClicked);
    connect(probabilityTreeWidget, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        updateLayerSelectionState(probabilityTreeWidget);
    });
}


void SignalControl::setupRefinementTreeWidget() {
    auto *refinementWidgetLayout = createSectionLayout(tr("Refinements"));

    refinementTreeWidget = new QTreeWidget();
    refinementTreeWidget->setObjectName(QStringLiteral("refinementsTree"));
    refinementTreeWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    refinementTreeWidget->setUniformRowHeights(true);
    refinementTreeWidget->setFocusPolicy(Qt::NoFocus);
    refinementTreeWidget->setColumnCount(2);
    refinementTreeWidget->setHeaderHidden(true);
    refinementTreeWidget->setRootIsDecorated(false);
    refinementTreeWidget->setIndentation(0);
    SignalLayerWidget::configureHostTree(refinementTreeWidget);
    setTreeMinimumRows(refinementTreeWidget, kOtherSectionVisibleRows);
    refinementWidgetLayout->addWidget(refinementTreeWidget, 1);
    selectedRefinementLabel = new QLabel("Selected: none");
    selectedRefinementLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    configureSelectionLabel(selectedRefinementLabel);
    refinementWidgetLayout->addWidget(selectedRefinementLabel);

    connect(refinementTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(refinementTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::refinementClicked);
    connect(refinementTreeWidget, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        updateLayerSelectionState(refinementTreeWidget);
    });
}

void SignalControl::handleDroppedFile(QString fileName) {
    SP_LOG_INFO("io", QStringLiteral("Handling dropped file %1").arg(fileName));
    QImageSelectionRadioButtons chooser(this);
    if (chooser.exec() == QDialog::Accepted) {
        loadDroppedFileAs(fileName, chooser.selectedChoice());
    }
}

void SignalControl::loadDroppedFileAs(QString fileName, ImageLoadChoice loadChoice) {
    if (loadChoiceRequiresWorkingSegments(loadChoice) && !hasWorkingSegments()) {
        showInfoMessage("Please load supervoxels first.");
        return;
    }

    switch (loadChoice) {
        case ImageLoadChoice::Supervoxels:
            askForBackgroundStrategy();
            addSegmentsGraph(fileName);
            break;
        case ImageLoadChoice::Image:
            addImage(fileName, QString(""));
            break;
        case ImageLoadChoice::Boundaries:
            if (!hasWorkingSegments()) {
                loadMembraneProbability(fileName);
            } else {
                loadMembraneProbability(fileName, QString(""));
            }
            break;
        case ImageLoadChoice::Refinement:
            loadRefinement(fileName, QString(""));
            break;
        case ImageLoadChoice::Segmentation:
            loadSegmentationVolume(fileName, QString(""));
            break;
    }
}


void SignalControl::toggleROISelection() {
    setROISelectionActive(!roiSelectionActive);
}

void SignalControl::transferSegmentsWithVolume() {
    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }

    bool ok;
    double volumeThreshold = QInputDialog::getDouble(this, tr("Transfer Supervoxels by Volume"),
                                                     tr("Transfer all supervoxels with a volume greater than:"),
                                                     50000, 0, 1000000, 0, &ok);
    if (!ok) {
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Transferring supervoxels..."),
        [this, volumeThreshold]() { graphBase->pGraph->transferSegmentsWithVolumeCriterion(volumeThreshold); },
        [this]() { refreshViewers(); });
}

void SignalControl::transferAllSegments() {
    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Transferring supervoxels..."),
        [this]() { graphBase->pGraph->transferSegmentsWithVolumeCriterion(1); },
        [this]() { refreshViewers(); });
}


void SignalControl::transferSupervoxelsByRefinementOverlap() {
    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }
    if (!hasSelectedRefinement()) {
        showInfoMessage("Please load a refinement first.");
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Transferring supervoxels by refinement overlap..."),
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

    auto *segmentationWidgetLayout = createSectionLayout(tr("Segmentations"));

    segmentationTreeWidget = new QTreeWidget();
    segmentationTreeWidget->setObjectName(QStringLiteral("segmentationsTree"));
    segmentationTreeWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    segmentationTreeWidget->setUniformRowHeights(true);
    segmentationTreeWidget->setFocusPolicy(Qt::NoFocus);
    segmentationTreeWidget->setColumnCount(2);
    segmentationTreeWidget->setHeaderHidden(true);
    segmentationTreeWidget->setRootIsDecorated(false);
    segmentationTreeWidget->setIndentation(0);
    SignalLayerWidget::configureHostTree(segmentationTreeWidget);
    setTreeMinimumRows(segmentationTreeWidget, kOtherSectionVisibleRows);
    segmentationWidgetLayout->addWidget(segmentationTreeWidget, 1);

    auto *selectionRow = new QHBoxLayout();
    selectionRow->setContentsMargins(0, 0, 0, 0);
    selectionRow->setSpacing(kSectionSpacing);

    selectedSegmentationLabel = new QLabel("Selected: none");
    selectedSegmentationLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    configureSelectionLabel(selectedSegmentationLabel);
    exportSegmentationButton = new QPushButton();
    exportSegmentationButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    selectionRow->addWidget(selectedSegmentationLabel, 1);
    selectionRow->addWidget(exportSegmentationButton);
    segmentationWidgetLayout->addLayout(selectionRow);

    auto *segmentationButtonRow = new QHBoxLayout();
    segmentationButtonRow->setContentsMargins(0, 0, 0, 0);
    segmentationButtonRow->setSpacing(kSectionSpacing);

    togglePaintBrushButton = new QPushButton();
    setPaintIdButton = new QPushButton();
    connectedComponentSplitButton = new QPushButton();
    togglePaintBrushButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setPaintIdButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connectedComponentSplitButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    segmentationButtonRow->addWidget(togglePaintBrushButton);
    segmentationButtonRow->addWidget(setPaintIdButton);
    segmentationButtonRow->addWidget(connectedComponentSplitButton);
    segmentationButtonRow->addStretch();

    segmentationWidgetLayout->addLayout(segmentationButtonRow);

    // Keep the panel button short for space; the shared action keeps the full
    // wording used in menus and dialogs.
    bindButtonToAction(exportSegmentationButton, exportSegmentationAction, tr("Export Selected"));
    bindButtonToAction(togglePaintBrushButton, togglePaintModeAction);
    bindButtonToAction(setPaintIdButton, setPaintIdAction);
    bindButtonToAction(connectedComponentSplitButton, connectedComponentSplitAction, tr("CC Split"));

    connect(segmentationTreeWidget, &QTreeWidget::itemDoubleClicked, this, &SignalControl::treeDoubleClicked);
    connect(segmentationTreeWidget, &QTreeWidget::itemClicked, this, &SignalControl::segmentationClicked);
    connect(segmentationTreeWidget, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        updateLayerSelectionState(segmentationTreeWidget);
    });
}


void SignalControl::treeDoubleClicked(QTreeWidgetItem *item, int) {
    switch (signal_tree::rowKind(item)) {
        case signal_tree::RowKind::Color:
            setUserColor(item);
            break;
        case signal_tree::RowKind::Norm:
            setUserNorm(item);
            break;
        case signal_tree::RowKind::Alpha:
            setUserAlpha(item);
            break;
        case signal_tree::RowKind::Root:
            setDescription(item);
            break;
        case signal_tree::RowKind::DataType:
            break;
    }
}


void SignalControl::treeClicked(QTreeWidgetItem *item, int) {
    if (signal_tree::rowKind(item) != signal_tree::RowKind::Root) {
        return;
    }
}

void SignalControl::refinementClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);
    selectRefinementItem(item);
    refreshUiState();
}

void SignalControl::boundaryClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);
    selectBoundaryItem(item);
    refreshUiState();
}


void SignalControl::setTransparentLabelIdInRefinement() {
    if (!hasSelectedRefinement()) {
        showInfoMessage("Please load a refinement first.");
        return;
    }

    int inputVal = QInputDialog::getInt(this,
                                        tr("Set Transparent Label ID in Refinement"),
                                        tr("Set Transparent Label ID in Refinement"),
                                        0,
                                        0);
    SP_LOG_INFO("segmentation", QStringLiteral("Setting refinement label %1 to transparent").arg(inputVal));
    graphBase->pSelectedRefinementSignal->setLUTValueToTransparent(inputVal);
    refreshViewers();
}

void SignalControl::segmentationClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);
    selectSegmentationItem(item);
    SP_LOG_DEBUG("segmentation", QStringLiteral("Selected segmentation maxId=%1").arg(graphBase->selectedSegmentationMaxSegmentId));
    refreshUiState();
}

void SignalControl::setUserColor(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Opening color picker for signal index=%1").arg(signalIndex));
    }

    if (allSignalList[signalIndex]->usesCategoricalLUT()) {
        allSignalList[signalIndex]->randomizeCategoricalLUT();
        refreshLayerWidget(item->treeWidget(), item);
        refreshViewers();
        return;
    }

    if (allSignalList[signalIndex]->usesEdgeStatusColors()) {
        return;
    }

    QColor newColor = QColorDialog::getColor();
    if (!newColor.isValid()) {
        return;
    }

    allSignalList[signalIndex]->setMainColor(newColor);

    refreshLayerWidget(item->treeWidget(), item);
    refreshViewers();
}

void SignalControl::setDescription(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Renaming signal index=%1").arg(signalIndex));
    }

    bool inputSuccessful;
    QString newName =
            QInputDialog::getText(this, "Set New Description", "Enter new Descriptor:", QLineEdit::Normal, QString(),
                                  &inputSuccessful);

    if (inputSuccessful) {
        if (isSegmentsItem(item)) { // TODO: put some unique identifier besides name/descriptor for segments
            SP_LOG_WARNING("segmentation", QStringLiteral("Segment descriptor renaming is still not implemented"));
        } else {
            allSignalList[signalIndex]->setName(newName);
            refreshLayerWidget(item->treeWidget(), item);
            refreshUiState();
        }
    }
}


void SignalControl::exportSelectedSegmentation() {
    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }

    // TODO: decide computer-wide vs application instance wide usage of settings
    QSettings MySettings;
    const QString DEFAULT_SAVE_DIR_KEY("default_save_dir");
    //    QString default_save_dir = MySettings.value(DEFAULT_SAVE_DIR_KEY).toString();
    QString path =
            QFileDialog::getSaveFileName(this, "Export Selected Segmentation", DEFAULT_SAVE_DIR,
                                         "Same Type as Watershed!!! (*.nrrd *.shlat *.uilat)");
    SP_LOG_INFO("io", QStringLiteral("Export selected segmentation dialog returned path=%1").arg(path));
    if (!path.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_SAVE_DIR_KEY, CurrentDir.absoluteFilePath(path));
        try {
            {
                auto spacing = graphBase->pSelectedSegmentation->GetSpacing();
                auto origin = graphBase->pSelectedSegmentation->GetOrigin();
                auto direction = graphBase->pSelectedSegmentation->GetDirection();
                SP_LOG_INFO("io",
                            QStringLiteral("Exporting selected segmentation to %1 spacing=[%2,%3,%4] origin=[%5,%6,%7] direction=[[ %8,%9,%10 ],[ %11,%12,%13 ],[ %14,%15,%16 ]] locale=%17")
                                .arg(path)
                                .arg(spacing[0]).arg(spacing[1]).arg(spacing[2])
                                .arg(origin[0]).arg(origin[1]).arg(origin[2])
                                .arg(direction[0][0]).arg(direction[0][1]).arg(direction[0][2])
                                .arg(direction[1][0]).arg(direction[1][1]).arg(direction[1][2])
                                .arg(direction[2][0]).arg(direction[2][1]).arg(direction[2][2])
                                .arg(QString::fromUtf8(std::setlocale(LC_NUMERIC, nullptr))));
            }
            graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(graphBase->pSelectedSegmentation,
                                                                           path.toStdString());
        } catch (itk::ExceptionObject &err) {
            SP_LOG_ERROR("io", QStringLiteral("Failed to export selected segmentation to %1: %2").arg(path, QString::fromStdString(err.GetDescription())));
        }

    }
}

void SignalControl::setPaintId(){
    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }
    auto maxId = std::numeric_limits<dataType::SegmentIdType>::max();
    //    cast to int max
    if (maxId > std::numeric_limits<int>::max()) {
        maxId = std::numeric_limits<int>::max();
    }
    bool ok;
    int paintId = QInputDialog::getInt(this, "Set Paint Label ID", "Set Paint Label ID",
                                       graphBase->pGraph->getNextFreeId(graphBase->pSelectedSegmentation),
                                       0, static_cast<int>(maxId), 1, &ok);
    if (!ok) {
        return;
    }
    SP_LOG_INFO("segmentation", QStringLiteral("Setting paint id to %1").arg(paintId));
    orthoViewer->xy->labelOfClickedSegmentation = paintId;
    orthoViewer->zy->labelOfClickedSegmentation = paintId;
    orthoViewer->xz->labelOfClickedSegmentation = paintId;
    orthoViewer->xy->setPaintId(paintId);
    orthoViewer->zy->setPaintId(paintId);
    orthoViewer->xz->setPaintId(paintId);
}

void SignalControl::togglePaintMode() {
    setPaintModeActive(!paintModeActive);
}

void SignalControl::activateDilateTool() {
    setAnnotationToolMode(SliceViewer::ToolMode::Dilate);
}

void SignalControl::activateErodeTool() {
    setAnnotationToolMode(SliceViewer::ToolMode::Erode);
}

void SignalControl::runConnectedComponentSplit() {
    using segment_puzzler::connected_components::ConnectedComponentSplitOptions;
    using segment_puzzler::connected_components::ConnectedComponentSplitStats;
    using segment_puzzler::connected_components::connectivityStencilName;
    using segment_puzzler::connected_components::splitDisconnectedLabelComponentsInPlace;

    if (orthoViewer != nullptr) {
        orthoViewer->flashShortcutLegendKey("f7");
    }

    ensureConnectedComponentSplitTargetAvailable();
    const auto target = selectedConnectedComponentSplitTarget();
    const auto connectivity = selectedConnectedComponentConnectivity();
    const QString connectivityName = QString::fromLatin1(connectivityStencilName(connectivity));

    if (target == ConnectedComponentSplitTarget::InitialSegments) {
        if (!hasWorkingSegments()) {
            showInfoMessage("Please load supervoxels first.");
            return;
        }

        taskRunner->runWithLabel(
            tr("Splitting disconnected initial segments..."),
            [this, connectivity]() {
                return graphBase->pGraph->splitDisconnectedInitialSegments(connectivity);
            },
            [this, connectivityName](ConnectedComponentSplitStats stats) {
                if (graphBase->pWorkingSegments != nullptr) {
                    graphBase->pWorkingSegments->checkAndResizeLUT(stats.maxLabel);
                }
                if (graphBase->pEdgesInitialSegmentsITKSignal != nullptr) {
                    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
                }
                SP_LOG_INFO("segmentation",
                            QStringLiteral("Connected component split on initial segments used %1 connectivity, split %2 labels into %3 new components")
                                .arg(connectivityName)
                                .arg(stats.labelsSplit)
                                .arg(stats.componentsCreated));
                refreshViewers();
                refreshUiState();
            });
        return;
    }

    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }

    std::unordered_set<dataType::SegmentIdType> ignoredLabels;
    ignoredLabels.insert(0);
    if (graphBase->pGraph != nullptr) {
        ignoredLabels.insert(graphBase->pGraph->backgroundId);
    }
    ignoredLabels.insert(graphBase->ignoredSegmentLabels.begin(), graphBase->ignoredSegmentLabels.end());

    const auto selectedSegmentation = graphBase->pSelectedSegmentation;
    const dataType::SegmentIdType nextFreeLabel =
        graphBase->pGraph != nullptr
            ? graphBase->pGraph->getNextFreeId(selectedSegmentation)
            : static_cast<dataType::SegmentIdType>(graphBase->selectedSegmentationMaxSegmentId + 1);

    taskRunner->runWithLabel(
        tr("Splitting disconnected segmentation labels..."),
        [selectedSegmentation, ignoredLabels = std::move(ignoredLabels), connectivity, nextFreeLabel]() {
            ConnectedComponentSplitOptions options;
            options.connectivity = connectivity;
            options.ignoredLabels = ignoredLabels;
            options.nextFreeLabel = nextFreeLabel;
            return splitDisconnectedLabelComponentsInPlace(selectedSegmentation, options);
        },
        [this, connectivityName](ConnectedComponentSplitStats stats) {
            graphBase->selectedSegmentationMaxSegmentId =
                graphBase->pGraph->getLargestIdInSegmentVolume(graphBase->pSelectedSegmentation);
            if (graphBase->pSelectedSegmentationSignal != nullptr) {
                graphBase->pSelectedSegmentationSignal->checkAndResizeLUT(graphBase->selectedSegmentationMaxSegmentId);
            }
            SP_LOG_INFO("segmentation",
                        QStringLiteral("Connected component split on selected segmentation used %1 connectivity, split %2 labels into %3 new components")
                            .arg(connectivityName)
                            .arg(stats.labelsSplit)
                            .arg(stats.componentsCreated));
            refreshViewers();
            refreshUiState();
        });
}


void SignalControl::setIsActive(QTreeWidgetItem *item, bool isActiveIn) {
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Setting signal active state to %1").arg(isActiveIn));
    }
    const size_t signalIndex = signalIndexForItem(item);

    allSignalList[signalIndex]->setIsActive(isActiveIn);

    for (auto *viewer: orthoViewer->viewerList) {
        viewer->setSliceIndex(viewer->getSliceIndex()); // update slice indices of newly activated signals
        viewer->recalculateQImages();
    }

    refreshLayerWidget(item->treeWidget(), item);
}

void SignalControl::setUserNorm(QTreeWidgetItem *item) {
    openNormPopup(item, this);
}

void SignalControl::setUserAlpha(QTreeWidgetItem *item) {
    openOpacityPopup(item, this);
}

void SignalControl::setSignalNormAndRefresh(size_t signalIndex, double lower, double upper) {
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }

    allSignalList[signalIndex]->setNorm(lower, upper);

    for (QTreeWidget *tree : {signalTreeWidget, probabilityTreeWidget, segmentationTreeWidget, refinementTreeWidget}) {
        if (QTreeWidgetItem *treeItem = findSignalTreeItem(tree, signalIndex)) {
            refreshLayerWidget(tree, treeItem);
            break;
        }
    }

    refreshViewers();
}


void SignalControl::addSegmentsGraph(QString fileName) {
    addSegmentsGraphAsync(fileName);
}

void SignalControl::addEmptySegmentsFromBoundary() {
    SP_LOG_INFO("segmentation", QStringLiteral("Creating empty segments volume from the selected boundary image"));
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
        SP_LOG_DEBUG("segmentation",
                     QStringLiteral("Empty segments spacing source=[%1,%2,%3] destination=[%4,%5,%6]")
                         .arg(srcSpacing[0]).arg(srcSpacing[1]).arg(srcSpacing[2])
                         .arg(dstSpacing[0]).arg(dstSpacing[1]).arg(dstSpacing[2]));
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

    SP_LOG_INFO("segmentation", QStringLiteral("Empty segments volume initialized and registered"));
}

void SignalControl::initializeGraph(size_t signalIndexGlobal) {
    registerSegmentsGraphSignal(signalIndexGlobal);
}

void SignalControl::runWatershed() {
    SP_LOG_INFO("watershed", QStringLiteral("Opening the watershed workflow window"));
    if (!hasSelectedBoundary()) {
        showInfoMessage("Please add boundaries first.");
        return;
    }

    auto *myMainWindow = new MainWindowWatershedControl(
        hasWorkingSegments() ? WatershedControl::OutputMode::Refinement
                             : WatershedControl::OutputMode::Segments);
    myMainWindow->setLinkedSignalControl(this);
    if (graphBase->ROI_set && hasWorkingSegments()) {
        myMainWindow->myWatershedControl->addBoundaries(graphBase->pSelectedBoundary,
                                                        graphBase->ROI_fx, graphBase->ROI_tx,
                                                        graphBase->ROI_fy, graphBase->ROI_ty,
                                                        graphBase->ROI_fz, graphBase->ROI_tz);
        myMainWindow->show();
    } else {
        myMainWindow->myWatershedControl->addBoundaries(graphBase->pSelectedBoundary);
        myMainWindow->show();

    }

    setROISelectionActive(false);
}

void SignalControl::receiveNewRefinement(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage) {
    size_t signalIndexGlobal;
    bool loadSuccessFull = insertImageSegmenttype(pImage, signalIndexGlobal);
    if (loadSuccessFull) {
        auto *typedSignal = dynamic_cast<itkSignal<GraphSegmentType>*>(allSignalList[signalIndexGlobal]);
        allSignalList[signalIndexGlobal]->setLUTCategorical();
        allSignalList[signalIndexGlobal]->setName(
            signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Refinement")));
        allSignalList[signalIndexGlobal]->setupTreeWidget(refinementTreeWidget, signalIndexGlobal);
        allSignalList[signalIndexGlobal]->setIsActive(false);
        allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
        attachLayerWidgetToLastItem(refinementTreeWidget);
        QTreeWidgetItem *newItem = refinementTreeWidget->topLevelItem(refinementTreeWidget->topLevelItemCount() - 1);
        setIsActive(newItem, false);
        selectLoadedItemIfAppropriate(refinementTreeWidget, newItem, lastAutoSelectedRefinementItem);
        orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
        selectRefinementItem(refinementTreeWidget->currentItem());

        // Copy the ROI that was active in the watershed workflow result so
        // refinement clicks stay inside that ROI.
        if (graphBase->ROI_set) {
            typedSignal->ROI_set = true;
            typedSignal->ROI_fx = graphBase->ROI_fx;
            typedSignal->ROI_fy = graphBase->ROI_fy;
            typedSignal->ROI_fz = graphBase->ROI_fz;
            typedSignal->ROI_tx = graphBase->ROI_tx;
            typedSignal->ROI_ty = graphBase->ROI_ty;
            typedSignal->ROI_tz = graphBase->ROI_tz;
        }
        refreshUiState();
    }
}

void SignalControl::importGeneratedSegments(GraphSegmentImageType::Pointer pImage, const QString &name) {
    if (pImage == nullptr) {
        showInfoMessage("No watershed segments available for import.");
        return;
    }

    size_t signalIndexGlobal = 0;
    if (!insertImageSegmenttype(pImage, signalIndexGlobal, false)) {
        showInfoMessage("Failed to import generated segments.");
        return;
    }

    prepareWorkingSegmentsGraph(pImage);
    registerSegmentsGraphSignal(signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    if (QTreeWidgetItem *item = findSignalTreeItem(signalTreeWidget, signalIndexGlobal)) {
        refreshLayerWidget(signalTreeWidget, item);
    }
}


void SignalControl::loadMembraneProbability(QString fileName, QString displayedName) {
    SP_LOG_INFO("io", QStringLiteral("Loading boundaries from %1").arg(fileName));
    unsigned int dimension = 0;
    itk::ImageIOBase::IOComponentType boundaryDataType = itk::ImageIOBase::UNKNOWNCOMPONENTTYPE;
    getDimensionAndDataTypeOfFile(fileName, dimension, boundaryDataType);

    FloatBoundaryConversionMode floatConversionMode = FloatBoundaryConversionMode::CastValues;
    if (boundaryDataType == itk::ImageIOBase::FLOAT || boundaryDataType == itk::ImageIOBase::DOUBLE) {
        auto selectedMode = askForFloatBoundaryConversionMode(fileName);
        if (!selectedMode) {
            return;
        }
        floatConversionMode = *selectedMode;
    }

    if (hasWorkingSegments()) {
        loadMembraneProbabilityAsync(fileName, displayedName, BoundaryLoadMode::BoundaryOnly, floatConversionMode);
        return;
    }

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("No Supervoxels Loaded"));
    msgBox.setText(tr("No supervoxels are loaded. Do you want to create empty segments or run watershed?"));
    QPushButton *emptyButton = msgBox.addButton(tr("Create Empty Segments"), QMessageBox::AcceptRole);
    QPushButton *watershedButton = msgBox.addButton(tr("Run Watershed"), QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Cancel);
    msgBox.exec();

    if (msgBox.clickedButton() == emptyButton) {
        loadMembraneProbabilityAsync(
            fileName,
            displayedName,
            BoundaryLoadMode::CreateEmptySegments,
            floatConversionMode);
        return;
    }

    if (msgBox.clickedButton() == watershedButton) {
        loadMembraneProbabilityAsync(
            fileName,
            displayedName,
            BoundaryLoadMode::BoundaryOnly,
            floatConversionMode,
            [this](LoadResult boundaryIndex) {
                if (!boundaryIndex) {
                    return;
                }
                runWatershed();
            });
    }
}


void SignalControl::createEmptySegmentation() {
    SP_LOG_INFO("segmentation", QStringLiteral("Creating an empty segmentation volume"));
    if (!hasWorkingSegments()) {
        showInfoMessage("Please load supervoxels first.");
        return;
    }

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
        SP_LOG_DEBUG("segmentation",
                     QStringLiteral("Empty segmentation source spacing=[%1,%2,%3] origin=[%4,%5,%6] destination spacing=[%7,%8,%9] origin=[%10,%11,%12]")
                         .arg(srcSpacing[0]).arg(srcSpacing[1]).arg(srcSpacing[2])
                         .arg(srcOrigin[0]).arg(srcOrigin[1]).arg(srcOrigin[2])
                         .arg(dstSpacing[0]).arg(dstSpacing[1]).arg(dstSpacing[2])
                         .arg(dstOrigin[0]).arg(dstOrigin[1]).arg(dstOrigin[2]));
    }

    std::unique_ptr<itkSignal<GraphSegmentType>> pSignal2(new itkSignal<GraphSegmentType>(pImage));
    auto *typedSignal = pSignal2.get();
    size_t signalIndexGlobal = allSignalList.size();
    allSignalList.push_back(typedSignal);
    ownedSignals.push_back(std::move(pSignal2));

    allSignalList[signalIndexGlobal]->setLUTCategorical();
    allSignalList[signalIndexGlobal]->setLUTValueToTransparent(0);
    allSignalList[signalIndexGlobal]->setName(
        signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Segmentation")));
    allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);
    attachLayerWidgetToLastItem(segmentationTreeWidget);
    QTreeWidgetItem *newItem = segmentationTreeWidget->topLevelItem(segmentationTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(segmentationTreeWidget, newItem, lastAutoSelectedSegmentationItem);

    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectSegmentationItem(segmentationTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::loadSegmentationVolumePressed() {
    SP_LOG_INFO("segmentation", QStringLiteral("Opening the load segmentation dialog"));
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();

    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Load Segmentation"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        loadSegmentationVolume(fileName); //TODO: add Progressbar?
    }
}

void SignalControl::addSegmentsPressed() {
    if (hasWorkingSegments()) {
        showInfoMessage("Supervoxels are already loaded, please restart SegmentPuzzler for a new project.");
        return;
    }
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Load Supervoxels"), default_load_dir);
    if (!fileName.isEmpty()) {
        askForBackgroundStrategy();

        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        addSegmentsGraph(fileName);
    }
}


void SignalControl::loadRefinementPressed() {
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_save_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Load Refinement"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        loadRefinement(fileName);
    }
}

void SignalControl::mergeSupervoxelsByRefinementLabel() {
    if (!hasSelectedRefinement()) {
        showInfoMessage("Please load a refinement first.");
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Merging supervoxels that share a refinement label..."),
        [this]() { graphBase->pGraph->mergeSegmentsWithRefinement(); },
        [this]() { refreshViewers(); });
}

void SignalControl::loadRefinement(QString fileName) {
    loadRefinement(fileName, "");
}

void SignalControl::loadRefinement(QString fileName, QString displayedName) {
    if (!hasWorkingSegments()) {
        showInfoMessage("Please load supervoxels first.");
        return;
    }
    loadRefinementAsync(fileName, displayedName);
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
        SP_LOG_INFO("io", QStringLiteral("Reading image %1").arg(fileName));
        getDimensionAndDataTypeOfFile(fileName, dimension, dataTypeOut);
        SP_LOG_INFO("io", QStringLiteral("Detected image dimension=%1 for %2").arg(dimension).arg(fileName));

        QSettings MySettings;
        QDir CurrentDir;
        const QString DEFAULT_SAVE_DIR_KEY("default_save_dir");
        MySettings.setValue(DEFAULT_SAVE_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        DEFAULT_SAVE_DIR = CurrentDir.absoluteFilePath(fileName);
        SP_LOG_DEBUG("io", QStringLiteral("Updated default save dir from %1").arg(fileName));

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
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_load_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Load Image"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        addImage(fileName);
    }
}


void SignalControl::loadMembraneProbabilityPressed() {
    QSettings MySettings;
    const QString DEFAULT_LOAD_DIR_KEY("default_save_dir");
    QString default_load_dir = MySettings.value(DEFAULT_LOAD_DIR_KEY).toString();
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Load Boundaries"), default_load_dir);
    if (!fileName.isEmpty()) {
        QDir CurrentDir;
        MySettings.setValue(DEFAULT_LOAD_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
        loadMembraneProbability(fileName);
    }
}

size_t SignalControl::signalIndexForItem(QTreeWidgetItem *item) const {
    const size_t signalIndex = signal_tree::signalIndex(item);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        throw std::logic_error("signal index not found!");
    }
    return signalIndex;
}

itkSignalBase *SignalControl::signalForItem(QTreeWidgetItem *item) const {
    return allSignalList[signalIndexForItem(item)];
}

bool SignalControl::isSegmentsItem(QTreeWidgetItem *item) const {
    if (signal_tree::topLevelSignalItem(item) == nullptr || segmentsGraph == nullptr) {
        return false;
    }
    return signalForItem(item) == segmentsGraph;
}
