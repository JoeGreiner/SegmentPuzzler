#include "watershedControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"
#include <itkImage.h>
#include "itkRegionOfInterestImageFilter.h"
#include "itkPasteImageFilter.h"


#include <src/viewers/itkSignal.h>
#include <QFileDialog>
#include <QColorDialog>
#include <QDateTime>
#include <src/segment_handling/Graph.h>
#include <src/viewers/OrthoViewer.h>
#include "src/qtUtils/SignalLayerWidget.h"
#include <QInputDialog>
#include <QHeaderView>
#include <QAbstractItemView>
#include <src/viewers/fileIO.h>


#include <QTreeWidget>
#include <src/itkImageFilters/itkWatershedHelpers.h>
#include <itkMultiThreaderBase.h>
#include <QThread>
#include <src/viewers/itkSignalThresholdPreview.h>
#include <QtWidgets/QMessageBox>
#include <QCheckBox>
#include <QMenu>
#include <QSlider>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QShortcut>
#include <QWidgetAction>
#include <itkCastImageFilter.h>
#include "src/qtUtils/SegmentTableDialog.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SignalTreeWidgetUtils.h"
#include "src/utils/AppLogger.h"
#include "src/utils/SignalNameUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr size_t kInvalidSignalIndex = static_cast<size_t>(-1);

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

QLabel *createHelpBadgeLabel(const QString &tooltipText, QWidget *parent) {
    if (tooltipText.isEmpty()) {
        return nullptr;
    }

    auto *helpLabel = new QLabel("?", parent);
    helpLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    helpLabel->setStyleSheet(
        "QLabel { color: white; background-color: #666; border-radius: 8px; "
        "font-weight: bold; font-size: 11px; min-width: 16px; min-height: 16px; "
        "max-width: 16px; max-height: 16px; padding: 0px; qproperty-alignment: AlignCenter; }");
    helpLabel->setToolTip(tooltipText);
    return helpLabel;
}

WatershedRunOptions makeBoundaryRepairOptions(WatershedAlgorithm algorithm) {
    WatershedRunOptions options;
    options.algorithm = algorithm;
    options.showWatershedLines = false;
    options.fullyConnected = false;
    return options;
}

dataType::SegmentsImageType::Pointer projectClusterLabelsOntoReference(
    dataType::SegmentsImageType::Pointer referenceLabels,
    dataType::SegmentsImageType::Pointer clusteredLabels) {
    if (referenceLabels.IsNull() || clusteredLabels.IsNull()) {
        return nullptr;
    }

    auto projectedLabels = dataType::SegmentsImageType::New();
    projectedLabels->SetRegions(referenceLabels->GetLargestPossibleRegion());
    projectedLabels->SetSpacing(referenceLabels->GetSpacing());
    projectedLabels->SetOrigin(referenceLabels->GetOrigin());
    projectedLabels->SetDirection(referenceLabels->GetDirection());
    projectedLabels->Allocate();

    const size_t voxelCount = referenceLabels->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *referenceBuffer = referenceLabels->GetBufferPointer();
    const auto *clusteredBuffer = clusteredLabels->GetBufferPointer();
    auto *projectedBuffer = projectedLabels->GetBufferPointer();

    std::unordered_map<dataType::SegmentIdType, dataType::SegmentIdType> labelToCluster;
    labelToCluster.reserve(1024);
    dataType::SegmentIdType nextFallbackLabel = 1;
    for (size_t index = 0; index < voxelCount; ++index) {
        nextFallbackLabel = std::max(nextFallbackLabel, static_cast<dataType::SegmentIdType>(clusteredBuffer[index] + 1));
        const dataType::SegmentIdType referenceLabel = referenceBuffer[index];
        const dataType::SegmentIdType clusterLabel = clusteredBuffer[index];
        if (referenceLabel == 0 || clusterLabel == 0) {
            continue;
        }

        const auto insertResult = labelToCluster.emplace(referenceLabel, clusterLabel);
        if (!insertResult.second && insertResult.first->second != clusterLabel) {
            throw std::logic_error("Agglomeration cluster projection produced conflicting labels for one reference segment.");
        }
    }

    for (size_t index = 0; index < voxelCount; ++index) {
        const dataType::SegmentIdType referenceLabel = referenceBuffer[index];
        if (referenceLabel == 0) {
            projectedBuffer[index] = 0;
            continue;
        }

        auto it = labelToCluster.find(referenceLabel);
        if (it == labelToCluster.end()) {
            const auto insertResult = labelToCluster.emplace(referenceLabel, nextFallbackLabel++);
            it = insertResult.first;
        }
        projectedBuffer[index] = it->second;
    }

    return projectedLabels;
}

QString makeUniqueSignalNameExcludingIndex(const std::vector<itkSignalBase *> &signalList,
                                           size_t excludedSignalIndex,
                                           const QString &requestedName) {
    auto otherSignals = signalList;
    if (excludedSignalIndex < otherSignals.size()) {
        otherSignals[excludedSignalIndex] = nullptr;
    }
    return signal_name_utils::makeUniqueSignalName(otherSignals, requestedName);
}

QTreeWidgetItem *findSignalTreeItem(QTreeWidget *treeWidget, size_t signalIndex) {
    if (treeWidget == nullptr) {
        return nullptr;
    }

    for (int itemIndex = 0; itemIndex < treeWidget->topLevelItemCount(); ++itemIndex) {
        QTreeWidgetItem *treeItem = treeWidget->topLevelItem(itemIndex);
        if (treeItem != nullptr && signal_tree::signalIndex(treeItem) == signalIndex) {
            return treeItem;
        }
    }
    return nullptr;
}

SignalLayerWidget *layerWidgetForItem(QTreeWidget *treeWidget, QTreeWidgetItem *item) {
    if (treeWidget == nullptr || item == nullptr) {
        return nullptr;
    }
    return qobject_cast<SignalLayerWidget *>(treeWidget->itemWidget(item, 0));
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

} // namespace

WatershedControl::~WatershedControl() {
    clearAgglomertionPreview();
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
    inspectSegmentsButton->setEnabled(!busy);
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
    agglomertionInputComboBox->setEnabled(!busy);
    agglomertionThresholdMaskComboBox->setEnabled(!busy);
    agglomertionStrategyComboBox->setEnabled(!busy);
    agglomertionLinkageComboBox->setEnabled(!busy);
    agglomertionBoundaryModeComboBox->setEnabled(!busy);
    agglomertionBiasSlider->setEnabled(!busy);
    agglomertionBiasSpinBox->setEnabled(!busy);
    agglomertionPreviewCheckBox->setEnabled(!busy);
    agglomertionApproximatePreviewCheckBox->setEnabled(!busy);
    agglomertionPreviewBoundariesCheckBox->setEnabled(!busy);
    agglomertionReplaceCheckBox->setEnabled(!busy);
    agglomertionSizeBiasCheckBox->setEnabled(!busy);
    agglomertionSizeBiasMaskCheckBox->setEnabled(!busy);
    {
        const bool sizeBiasOn = !busy && agglomertionSizeBiasCheckBox->isChecked();
        agglomertionSizeBiasStrategyComboBox->setEnabled(sizeBiasOn);
        agglomertionSizeBiasThresholdSlider->setEnabled(sizeBiasOn);
        agglomertionSizeBiasThresholdSpinBox->setEnabled(sizeBiasOn);
        agglomertionSizeBiasStrengthSlider->setEnabled(sizeBiasOn);
        agglomertionSizeBiasStrengthSpinBox->setEnabled(sizeBiasOn);
        agglomertionSizeBiasProtectionSlider->setEnabled(sizeBiasOn);
        agglomertionSizeBiasProtectionSpinBox->setEnabled(sizeBiasOn);
    }
    runAgglomertionButton->setEnabled(!busy);
    finalOutputInputComboBox->setEnabled(!busy);
    if (!busy) {
        updateStepEnablement();
        agglomertionPreviewSettingsChanged();
    }
}

void WatershedControl::updateLayerSelectionState(QTreeWidget *treeWidget) {
    if (treeWidget == nullptr) {
        return;
    }

    for (int itemIndex = 0; itemIndex < treeWidget->topLevelItemCount(); ++itemIndex) {
        refreshLayerWidget(treeWidget, treeWidget->topLevelItem(itemIndex));
    }
}

void WatershedControl::attachLayerWidgetToItem(QTreeWidget *treeWidget, QTreeWidgetItem *item) {
    if (treeWidget == nullptr || item == nullptr) {
        return;
    }

    auto *layerWidget = new SignalLayerWidget(treeWidget);
    item->setFirstColumnSpanned(true);
    treeWidget->setItemWidget(item, 0, layerWidget);
    SignalLayerWidget::requestHostTreeLayoutSync(treeWidget);

    connect(layerWidget, &SignalLayerWidget::sizeHintChanged, this, [treeWidget]() {
        SignalLayerWidget::requestHostTreeLayoutSync(treeWidget);
    });

    connect(layerWidget, &SignalLayerWidget::activated, this, [this, treeWidget, item]() {
        treeWidget->setCurrentItem(item);
        updateLayerSelectionState(treeWidget);
    });
    connect(layerWidget, &SignalLayerWidget::renameRequested, this, [this, treeWidget, item]() {
        treeWidget->setCurrentItem(item);
        setDescription(item);
    });
    connect(layerWidget, &SignalLayerWidget::visibilityToggled, this, [this, treeWidget, item](bool visible) {
        setIsActive(item, visible);
        refreshLayerWidget(treeWidget, item);
    });
    connect(layerWidget, &SignalLayerWidget::colorRequested, this, [this, treeWidget, item]() {
        setUserColor(item);
        refreshLayerWidget(treeWidget, item);
    });
    connect(layerWidget, &SignalLayerWidget::contrastRequested, this, [this, item](QWidget *anchor) {
        openNormPopup(item, anchor);
    });
    connect(layerWidget, &SignalLayerWidget::opacityRequested, this, [this, item](QWidget *anchor) {
        openOpacityPopup(item, anchor);
    });

    refreshLayerWidget(treeWidget, item);
}

void WatershedControl::attachLayerWidgetToLastItem(QTreeWidget *treeWidget) {
    if (treeWidget == nullptr || treeWidget->topLevelItemCount() <= 0) {
        return;
    }
    attachLayerWidgetToItem(treeWidget, treeWidget->topLevelItem(treeWidget->topLevelItemCount() - 1));
}

void WatershedControl::refreshLayerWidget(QTreeWidget *treeWidget, QTreeWidgetItem *item) {
    if (treeWidget == nullptr || item == nullptr) {
        return;
    }

    auto *layerWidget = layerWidgetForItem(treeWidget, item);
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
    presentation.selected = item == signal_tree::topLevelSignalItem(treeWidget->currentItem());
    presentation.contrastText = contrastChipText(signal);
    presentation.opacityText = opacityChipText(signal);
    presentation.toolTip = layerToolTipText(signal);
    presentation.contrastAvailable = signal->supportsNormControl();
    layerWidget->applyPresentation(presentation);
}

void WatershedControl::openNormPopup(QTreeWidgetItem *item, QWidget *anchor) {
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

    auto *treeWidget = item->treeWidget();
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

    auto applyNorm = [this, treeWidget, item, signal, lowerSlider, upperSlider, lowerSpinBox, upperSpinBox](int lower,
                                                                                                             int upper) {
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
        refreshLayerWidget(treeWidget, item);
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

void WatershedControl::openOpacityPopup(QTreeWidgetItem *item, QWidget *anchor) {
    if (item == nullptr || anchor == nullptr) {
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }

    itkSignalBase *signal = allSignalList[signalIndex];
    auto *treeWidget = item->treeWidget();
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

    auto applyOpacity = [this, treeWidget, item, signal, slider, spinBox](int value) {
        const int clampedValue = std::clamp(value, 0, 100);
        const QSignalBlocker sliderBlocker(slider);
        const QSignalBlocker spinBlocker(spinBox);
        slider->setValue(clampedValue);
        spinBox->setValue(clampedValue);
        signal->setAlpha(percentToAlpha(clampedValue));
        refreshLayerWidget(treeWidget, item);
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

void WatershedControl::refreshViewers() {
    orthoViewer->refreshViewers();
}

bool WatershedControl::shouldShowAgglomertionPreview() const {
    if (agglomertionPreviewCheckBox == nullptr || !agglomertionPreviewCheckBox->isChecked()) {
        return false;
    }
    if (taskRunner->isBusy()) {
        return false;
    }
    if (selectedAgglomertionInput().IsNull() || selectedBoundaryInput().IsNull()) {
        return false;
    }
    return !agglomertionNeedsThresholdMask() || selectedAgglomertionThresholdMask().IsNotNull();
}

void WatershedControl::restoreHiddenAgglomertionPreviewSource() {
    if (hiddenAgglomertionPreviewSourceSignalIndex == kInvalidSignalIndex) {
        return;
    }

    const size_t signalIndex = hiddenAgglomertionPreviewSourceSignalIndex;
    hiddenAgglomertionPreviewSourceSignalIndex = kInvalidSignalIndex;
    if (signalIndex < allSignalList.size() && allSignalList[signalIndex] != nullptr) {
        setSignalActive(signalIndex, true);
    }
}

void WatershedControl::syncAgglomertionPreviewSourceVisibility(bool showPreview) {
    restoreHiddenAgglomertionPreviewSource();
    if (!showPreview || !comboHasValidSelection(agglomertionInputComboBox)) {
        return;
    }

    const size_t signalIndex = selectedSignalIndex(agglomertionInputComboBox);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }
    if (!allSignalList[signalIndex]->getIsActive()) {
        return;
    }

    hiddenAgglomertionPreviewSourceSignalIndex = signalIndex;
    setSignalActive(signalIndex, false);
}

void WatershedControl::clearAgglomertionPreview() {
    if (agglomertionPreviewTimer != nullptr) {
        agglomertionPreviewTimer->stop();
    }
    if (pAgglomertionPreviewSignal != nullptr) {
        for (size_t idx = 0; idx < allSignalList.size(); ++idx) {
            if (allSignalList[idx] == pAgglomertionPreviewSignal) {
                setSignalActive(idx, false);
                break;
            }
        }
    }
    restoreHiddenAgglomertionPreviewSource();
}

void WatershedControl::scheduleAgglomertionPreviewRefresh() {
    if (agglomertionPreviewTimer == nullptr || taskRunner->isBusy()) {
        return;
    }
    agglomertionPreviewTimer->start();
}

void WatershedControl::agglomertionPreviewSettingsChanged() {
    if (agglomertionBiasValueLabel != nullptr) {
        agglomertionBiasValueLabel->setText(agglomertionBiasLabelText());
    }

    const bool showPreview = shouldShowAgglomertionPreview();
    syncAgglomertionPreviewSourceVisibility(showPreview);

    if (!showPreview) {
        clearAgglomertionPreview();
        updateStepEnablement();
        return;
    }
    scheduleAgglomertionPreviewRefresh();
}

void WatershedControl::agglomertionPreviewSliceChanged(int, int) {
    if (agglomertionPreviewCheckBox != nullptr && agglomertionPreviewCheckBox->isChecked()) {
        const bool approximate = agglomertionApproximatePreviewCheckBox != nullptr && agglomertionApproximatePreviewCheckBox->isChecked();
        if (approximate) {
            scheduleAgglomertionPreviewRefresh();
        }
    }
}

void WatershedControl::connectAgglomertionPreviewSignals() {
    if (orthoViewer == nullptr || orthoViewer->xy == nullptr || orthoViewer->xz == nullptr || orthoViewer->zy == nullptr) {
        return;
    }
    connect(orthoViewer->xy, &SliceViewer::sliceIndexChanged, this, &WatershedControl::agglomertionPreviewSliceChanged);
    connect(orthoViewer->xz, &SliceViewer::sliceIndexChanged, this, &WatershedControl::agglomertionPreviewSliceChanged);
    connect(orthoViewer->zy, &SliceViewer::sliceIndexChanged, this, &WatershedControl::agglomertionPreviewSliceChanged);
}

void WatershedControl::refreshAgglomertionPreview() {
    const bool showPreview = shouldShowAgglomertionPreview();
    const bool approximate = agglomertionApproximatePreviewCheckBox != nullptr && agglomertionApproximatePreviewCheckBox->isChecked();

    if (!showPreview) {
        clearAgglomertionPreview();
        updateStepEnablement();
        return;
    }

    const dataType::SegmentsImageType::Pointer watershedInput = selectedAgglomertionInput();
    const dataType::BoundaryImageType::Pointer boundaryInput = selectedBoundaryInput();
    const itk::Image<unsigned char, 3>::Pointer thresholdMask = selectedAgglomertionThresholdMask();
    if (watershedInput.IsNull() || boundaryInput.IsNull()) {
        clearAgglomertionPreview();
        updateStepEnablement();
        return;
    }
    if (agglomertionNeedsThresholdMask() && thresholdMask.IsNull()) {
        clearAgglomertionPreview();
        updateStepEnablement();
        return;
    }

    using CastFilterType = itk::CastImageFilter<dataType::BoundaryImageType, segment_puzzler::BoundaryFloatImageType>;
    auto castFilter = CastFilterType::New();
    castFilter->SetInput(boundaryInput);
    castFilter->Update();

    const segment_puzzler::WatershedRagAgglomerationOptions options = currentAgglomertionOptions();

    itk::Image<unsigned int, 3>::Pointer previewLabels;

    if (!approximate) {
        auto result = segment_puzzler::runWatershedRagAgglomeration(
            watershedInput,
            castFilter->GetOutput(),
            thresholdMask,
            options);
        previewLabels = projectClusterLabelsOntoReference(watershedInput, result.agglomeratedLabels);
    } else {
        segment_puzzler::OrthoPlanePreviewSelection previewSelection;
        previewSelection.sliceIndices = {{
            orthoViewer->zy->getSliceIndex(),
            orthoViewer->xz->getSliceIndex(),
            orthoViewer->xy->getSliceIndex()
        }};
        auto result = segment_puzzler::runWatershedRagAgglomerationPreview(
            watershedInput,
            castFilter->GetOutput(),
            previewSelection,
            thresholdMask,
            options);
        previewLabels = result.agglomeratedLabels;
    }

    if (agglomertionPreviewBoundariesCheckBox != nullptr &&
        agglomertionPreviewBoundariesCheckBox->isChecked() &&
        pThresholdedMembrane.IsNotNull()) {
        auto previewPartition = deriveBoundaryConsistentPartition(
            previewLabels,
            pThresholdedMembrane,
            makeBoundaryRepairOptions(selectedWatershedAlgorithm()),
            /*repairCanonicalLabels=*/false,
            selectedDistanceMapAlgorithm(),
            workerThreadCount);
        previewLabels = previewPartition.displayLabels;
    }

    if (pAgglomertionPreviewSignal == nullptr) {
        auto previewSignal = std::make_unique<itkSignal<unsigned int>>(previewLabels);
        pAgglomertionPreviewSignal = previewSignal.get();
        pAgglomertionPreviewSignal->setMainColor(255, 0, 255); // Magenta
        registerSignal(std::move(previewSignal), SignalStage::None, "Agglomertion Preview", true, true);
    } else {
        pAgglomertionPreviewSignal->updateImage(previewLabels);
        for (size_t idx = 0; idx < allSignalList.size(); ++idx) {
            if (allSignalList[idx] == pAgglomertionPreviewSignal) {
                setSignalActive(idx, true);
                break;
            }
        }
    }
    refreshViewers();
    updateStepEnablement();

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
            const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
            switch (thresholdAlgorithm) {
                case ThresholdAlgorithm::BinaryThreshold:
                    binaryThresholdImageFilterFloat(thresholdInputCopy, thresholded, thresholdValue);
                    break;
            }
            SP_LOG_INFO("watershed",
                        QStringLiteral("Threshold boundaries finished in %1 ms")
                            .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
            return thresholded;
        },
        [this, signalName](itk::Image<unsigned char, 3>::Pointer thresholded) {
            pThresholdedMembrane = thresholded;
            auto pThresholdedMembraneSignal = std::make_unique<itkSignal<unsigned char>>(pThresholdedMembrane);
            pThresholdedMembraneSignal->setMainColor(0, 255, 255); // Cyan
            itkSignalBase *pSignal = pThresholdedMembraneSignal.get();
            registerSignal(std::move(pThresholdedMembraneSignal),
                           SignalStage::Threshold,
                           signalName,
                           /*categorical=*/false,
                           /*transparentZero=*/true);
            orthoViewer->xy->pThresholdedBoundaries = pThresholdedMembrane;
            orthoViewer->xz->pThresholdedBoundaries = pThresholdedMembrane;
            orthoViewer->zy->pThresholdedBoundaries = pThresholdedMembrane;
            orthoViewer->xy->pThresholdedBoundariesSignal = pSignal;
            orthoViewer->xz->pThresholdedBoundariesSignal = pSignal;
            orthoViewer->zy->pThresholdedBoundariesSignal = pSignal;
            orthoViewer->setViewToMiddleOfStack();
            updateStepEnablement();
            std::vector<size_t> signalsToDeactivate;
            if (boundarySignalIndex >= 0) {
                signalsToDeactivate.push_back(static_cast<size_t>(boundarySignalIndex));
            }
            if (thresholdPreviewSignalIndex >= 0) {
                signalsToDeactivate.push_back(static_cast<size_t>(thresholdPreviewSignalIndex));
            }
            deactivateSignalsByIndices(signalsToDeactivate);
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
            const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
            generateDistanceMap(thresholdInputCopy, distanceMap, 0, distanceMapAlgorithm, threadCount);
            SP_LOG_INFO("watershed",
                        QStringLiteral("Distance map generation finished in %1 ms")
                            .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
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
            const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
            extractMinimaFromDistanceMap(distanceMapInputCopy, seeds, 1, seedAlgorithm);
            SP_LOG_INFO("watershed",
                        QStringLiteral("Seed extraction finished in %1 ms")
                            .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
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
    const DistanceMapAlgorithm distanceMapAlgorithm = selectedDistanceMapAlgorithm();
    const itk::Image<float, 3>::Pointer distanceMapInput = selectedWatershedDistanceMapInput();
    const itk::Image<unsigned int, 3>::Pointer seedsInput = selectedWatershedSeedsInput();
    const itk::Image<unsigned char, 3>::Pointer thresholdInput = selectedWatershedThresholdInput();
    const QString signalName =
        QString("Watershed [%1]").arg(watershedAlgorithmLabel(watershedAlgorithm));

    removeRegisteredEdgeSignal();

    taskRunner->runWithLabel(
        QStringLiteral("Running watershed..."),
        [filterEnabled, minSegmentSize, watershedAlgorithm, distanceMapAlgorithm, distanceMapInput, seedsInput, thresholdInput,
         threadCount = workerThreadCount]() {
            itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);
            auto distanceMapInputCopy = distanceMapInput;
            WatershedRunOptions watershedOptions;
            watershedOptions.algorithm = watershedAlgorithm;
            itk::Image<float, 3>::Pointer invertedDistanceMap = itk::Image<float, 3>::New();
            qint64 stepStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            invertDistanceMap(distanceMapInputCopy, invertedDistanceMap);
            SP_LOG_DEBUG("watershed",
                         QStringLiteral("Distance map inversion finished in %1 ms")
                             .arg(QDateTime::currentMSecsSinceEpoch() - stepStartedAtMs));

            itk::Image<unsigned int, 3>::Pointer watershedFragments;
            itk::Image<unsigned int, 3>::Pointer seedsForWatershed = seedsInput;
            stepStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            runWatershed(invertedDistanceMap, seedsForWatershed, watershedFragments, watershedOptions);
            SP_LOG_INFO("watershed",
                        QStringLiteral("Watershed run finished in %1 ms")
                            .arg(QDateTime::currentMSecsSinceEpoch() - stepStartedAtMs));

            if (filterEnabled) {
                stepStartedAtMs = QDateTime::currentMSecsSinceEpoch();
                filterSmallSegmentSeeds(watershedFragments, seedsForWatershed, minSegmentSize);
                SP_LOG_DEBUG("watershed",
                             QStringLiteral("Small seed filtering finished in %1 ms")
                                 .arg(QDateTime::currentMSecsSinceEpoch() - stepStartedAtMs));
                stepStartedAtMs = QDateTime::currentMSecsSinceEpoch();
                runWatershed(invertedDistanceMap, seedsForWatershed, watershedFragments, watershedOptions);
                SP_LOG_INFO("watershed",
                            QStringLiteral("Filtered watershed rerun finished in %1 ms")
                                .arg(QDateTime::currentMSecsSinceEpoch() - stepStartedAtMs));
            }

            stepStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            auto derivedPartition = deriveBoundaryConsistentPartition(
                watershedFragments,
                thresholdInput,
                makeBoundaryRepairOptions(watershedAlgorithm),
                /*repairCanonicalLabels=*/true,
                distanceMapAlgorithm,
                threadCount);
            SP_LOG_INFO("watershed",
                        QStringLiteral("Boundary-consistent watershed labels finished in %1 ms")
                            .arg(QDateTime::currentMSecsSinceEpoch() - stepStartedAtMs));

            return derivedPartition;
        },
        [this, signalName](const BoundaryConsistentPartitionResult &watershedOutputs) {
            pWatershedFragments = watershedOutputs.canonicalLabels;
            pWatershed = watershedOutputs.displayLabels;
            auto pSignal2 = std::make_unique<itkSignal<unsigned int>>(pWatershed);
            auto *watershedSignal = pSignal2.get();
            const size_t signalIndex = registerSignal(std::move(pSignal2), SignalStage::Watershed, signalName, /*categorical=*/true);
            generatedStageOutputs[signalIndex] = GeneratedStageOutput{
                SignalStage::Watershed,
                watershedOutputs.canonicalLabels,
                watershedOutputs.displayLabels,
                watershedOutputs.splitComponentIds};
            rebuildGraphFromSegmentsImage(pWatershed);
            attachSegmentsSignalToGraph(watershedSignal);
            updateStepEnablement();
            deactivateSignalsByIndices(seedOutputSignalIndices);
        },
        std::move(then));
}

void WatershedControl::agglomertionAsync(std::function<void()> then) {
    const dataType::SegmentsImageType::Pointer watershedInput = selectedAgglomertionInput();
    const dataType::BoundaryImageType::Pointer boundaryInput = selectedBoundaryInput();
    const itk::Image<unsigned char, 3>::Pointer thresholdInput = selectedWatershedThresholdInput();
    const itk::Image<unsigned char, 3>::Pointer thresholdMask = selectedAgglomertionThresholdMask();
    const WatershedAlgorithm watershedAlgorithm = selectedWatershedAlgorithm();
    const DistanceMapAlgorithm distanceMapAlgorithm = selectedDistanceMapAlgorithm();
    const segment_puzzler::RagLinkage linkage = selectedAgglomertionLinkage();
    const segment_puzzler::BoundaryNormalizationMode boundaryMode = selectedAgglomertionBoundaryNormalization();
    const segment_puzzler::BoundaryEvidenceStrategy boundaryStrategy = selectedAgglomertionBoundaryEvidenceStrategy();
    const double tau = selectedAgglomertionTau();
    const bool sizeBiasEnabled = agglomertionSizeBiasCheckBox != nullptr && agglomertionSizeBiasCheckBox->isChecked();
    const segment_puzzler::SizeBiasStrategy sizeBiasStrategy = sizeBiasEnabled ? selectedSizeBiasStrategy() : segment_puzzler::SizeBiasStrategy::Off;
    const uint64_t sizeBiasThreshold = sizeBiasEnabled ? selectedSizeBiasThreshold() : 5000;
    const double sizeBiasStrength = sizeBiasEnabled ? selectedSizeBiasStrength() : 0.3;
    const double sizeBiasProtection = sizeBiasEnabled ? selectedSizeBiasProtection() : 0.3;
    const bool sizeBiasRespectMask = !sizeBiasEnabled || agglomertionSizeBiasMaskCheckBox->isChecked();
    const size_t replaceTargetSignalIndex =
        agglomertionReplaceCheckBox != nullptr
        && agglomertionReplaceCheckBox->isChecked()
        && selectedFinalAgglomertionStageOutput() != nullptr
        && comboHasValidSelection(finalOutputInputComboBox)
            ? selectedSignalIndex(finalOutputInputComboBox)
            : kInvalidSignalIndex;

    const QString signalName =
        QString("Agglomertion [%1 | %2 | %3 | tau=%4]")
            .arg(agglomertionBoundaryEvidenceStrategyLabel(boundaryStrategy))
            .arg(agglomertionLinkageLabel(linkage))
            .arg(agglomertionBoundaryModeLabel(boundaryMode))
            .arg(tau, 0, 'f', 2);

    clearAgglomertionPreview();
    removeRegisteredEdgeSignal();

    taskRunner->runWithLabel(
        QStringLiteral("Running agglomertion..."),
        [watershedInput, boundaryInput, thresholdInput, thresholdMask, watershedAlgorithm, distanceMapAlgorithm,
         linkage, boundaryMode, boundaryStrategy, tau, sizeBiasStrategy, sizeBiasThreshold, sizeBiasStrength,
         sizeBiasProtection, sizeBiasRespectMask, this]() {
            using CastFilterType = itk::CastImageFilter<dataType::BoundaryImageType, segment_puzzler::BoundaryFloatImageType>;
            auto castFilter = CastFilterType::New();
            castFilter->SetInput(boundaryInput);
            castFilter->Update();

            segment_puzzler::WatershedRagAgglomerationOptions options;
            options.linkage = linkage;
            options.boundaryNormalization = boundaryMode;
            options.boundaryEvidenceStrategy = boundaryStrategy;
            options.tau = tau;
            options.threadCount = workerThreadCount;
            options.sizeBiasStrategy = sizeBiasStrategy;
            options.sizeBiasThreshold = sizeBiasThreshold;
            options.sizeBiasStrength = sizeBiasStrength;
            options.sizeBiasProtection = sizeBiasProtection;
            options.sizeBiasRespectMask = sizeBiasRespectMask;

            auto agglomerationResult = segment_puzzler::runWatershedRagAgglomeration(
                watershedInput,
                castFilter->GetOutput(),
                thresholdMask,
                options);

            auto canonicalAgglomertionLabels = projectClusterLabelsOntoReference(
                watershedInput,
                agglomerationResult.agglomeratedLabels);

            auto derivedPartition = deriveBoundaryConsistentPartition(
                canonicalAgglomertionLabels,
                thresholdInput,
                makeBoundaryRepairOptions(watershedAlgorithm),
                /*repairCanonicalLabels=*/false,
                distanceMapAlgorithm,
                workerThreadCount);

            return BoundaryConsistentPartitionResult{
                canonicalAgglomertionLabels,
                derivedPartition.displayLabels,
                std::move(derivedPartition.splitComponentIds)};
        },
        [this, signalName, replaceTargetSignalIndex](const BoundaryConsistentPartitionResult &agglomertionOutputs) {
            pAgglomertionFragments = agglomertionOutputs.canonicalLabels;
            pAgglomertion = agglomertionOutputs.displayLabels;
            auto previousWatershedIndices = watershedOutputSignalIndices;
            if (replaceTargetSignalIndex != kInvalidSignalIndex) {
                previousWatershedIndices.erase(
                    std::remove(previousWatershedIndices.begin(), previousWatershedIndices.end(), replaceTargetSignalIndex),
                    previousWatershedIndices.end());
            }
            size_t signalIndex = replaceTargetSignalIndex;
            itkSignal<unsigned int> *agglomertionSignal = nullptr;
            if (replaceTargetSignalIndex != kInvalidSignalIndex) {
                auto *existingSignal = dynamic_cast<itkSignal<unsigned int> *>(allSignalList[replaceTargetSignalIndex]);
                if (existingSignal == nullptr) {
                    throw std::logic_error("Selected agglomertion replacement target is invalid.");
                }
                existingSignal->updateImage(pAgglomertion);
                updateRegisteredSignalName(replaceTargetSignalIndex, signalName);
                agglomertionSignal = existingSignal;
            } else {
                auto pSignal = std::make_unique<itkSignal<unsigned int>>(pAgglomertion);
                agglomertionSignal = pSignal.get();
                signalIndex = registerSignal(std::move(pSignal), SignalStage::Agglomertion, signalName, /*categorical=*/true);
            }
            generatedStageOutputs[signalIndex] = GeneratedStageOutput{
                SignalStage::Agglomertion,
                agglomertionOutputs.canonicalLabels,
                agglomertionOutputs.displayLabels,
                agglomertionOutputs.splitComponentIds};
            setSignalActive(signalIndex, true);
            rebuildGraphFromSegmentsImage(pAgglomertion);
            attachSegmentsSignalToGraph(agglomertionSignal);
            scheduleAgglomertionPreviewRefresh();
            updateStepEnablement();
            deactivateSignalsByIndices(previousWatershedIndices);
            std::vector<size_t> previousBoundaryIndices;
            if (boundarySignalIndex >= 0) {
                previousBoundaryIndices.push_back(static_cast<size_t>(boundarySignalIndex));
            }
            if (thresholdPreviewSignalIndex >= 0) {
                previousBoundaryIndices.push_back(static_cast<size_t>(thresholdPreviewSignalIndex));
            }
            deactivateSignalsByIndices(previousBoundaryIndices);
            const int agglomComboIdx = finalOutputInputComboBox->findData(static_cast<int>(signalIndex));
            if (agglomComboIdx >= 0) finalOutputInputComboBox->setCurrentIndex(agglomComboIdx);
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
    attachLayerWidgetToLastItem(signalTreeWidget);
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
            agglomertionInputSignalIndices.push_back(idx);
            break;
        case SignalStage::Agglomertion:
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
    SP_LOG_INFO("io", QStringLiteral("Adding watershed input image %1").arg(fileName));
    if (!fileName.isEmpty()) {
        itk::ImageIOBase::IOComponentType dataType;
        size_t signalIndexGlobal;
        bool loadSuccessFull = loadImage(fileName, dataType, signalIndexGlobal, false);
        if (loadSuccessFull) {
            allSignalList[signalIndexGlobal]->setLUTContinuous();
            allSignalList[signalIndexGlobal]->setName(
                signal_name_utils::makeUniqueSignalName(allSignalList, QFileInfo(fileName).baseName()));
            allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
            attachLayerWidgetToLastItem(signalTreeWidget);
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
    pWatershedFragments = nullptr;
    pAgglomertion = nullptr;
    pAgglomertionFragments = nullptr;
    pThresholdPreviewSignal = nullptr;
    pAgglomertionPreviewSignal = nullptr;
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
    runAgglomertionButton = nullptr;
    agglomertionInputComboBox = nullptr;
    agglomertionThresholdMaskComboBox = nullptr;
    agglomertionLinkageComboBox = nullptr;
    agglomertionBoundaryModeComboBox = nullptr;
    agglomertionStrategyComboBox = nullptr;
    agglomertionBiasSlider = nullptr;
    agglomertionBiasSpinBox = nullptr;
    agglomertionBiasValueLabel = nullptr;
    agglomertionPreviewCheckBox = nullptr;
    agglomertionApproximatePreviewCheckBox = nullptr;
    agglomertionPreviewBoundariesCheckBox = nullptr;
    agglomertionSizeBiasCheckBox = nullptr;
    agglomertionSizeBiasMaskCheckBox = nullptr;
    agglomertionSizeBiasStrategyComboBox = nullptr;
    agglomertionSizeBiasThresholdSlider = nullptr;
    agglomertionSizeBiasThresholdSpinBox = nullptr;
    agglomertionSizeBiasStrengthSlider = nullptr;
    agglomertionSizeBiasStrengthSpinBox = nullptr;
    agglomertionSizeBiasProtectionSlider = nullptr;
    agglomertionSizeBiasProtectionSpinBox = nullptr;
    inspectSegmentsButton = nullptr;
    finalOutputInputComboBox = nullptr;
    workerThreadCount = defaultWatershedThreadCount();
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Expanding);
    setWatershedLogSink([](const std::string &message) {
        SP_LOG_INFO("watershed", QString::fromStdString(message));
    });
    segment_puzzler::setAgglomerationLogSink([](const std::string &message) {
        SP_LOG_INFO("watershed", QString::fromStdString(message));
    });

    useROI = false;
    fx = 0;
    fy = 0;
    fz = 0;
    tx = 0;
    ty = 0;
    tz = 0;


    setupWorkflowUi();
    setupAlgorithmComboBoxes();
    connectAgglomertionPreviewSignals();
    refreshInputSelectors();
    updateStepEnablement();

    connect(taskRunner, &TaskRunner::busyChanged, this, &WatershedControl::setGuiBusy);
    agglomertionPreviewTimer = new QTimer(this);
    agglomertionPreviewTimer->setSingleShot(true);
    agglomertionPreviewTimer->setInterval(120);
    connect(agglomertionPreviewTimer, &QTimer::timeout, this, &WatershedControl::refreshAgglomertionPreview);
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
    setupAgglomertionWidget();
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

QWidget *WatershedControl::createLabeledInputRow(const QString &labelText, QComboBox *comboBox, const QString &tooltipText) const {
    return createLabeledInputRow(labelText, static_cast<QWidget *>(comboBox), tooltipText);
}

QWidget *WatershedControl::createLabeledInputRow(const QString &labelText, QWidget *widget) const {
    return createLabeledInputRow(labelText, widget, QString());
}

QWidget *WatershedControl::createLabeledInputRow(const QString &labelText, QWidget *widget, const QString &tooltipText) const {
    auto *rowWidget = new QWidget(workflowContentWidget);
    auto *rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    auto *label = new QLabel(labelText, rowWidget);
    label->setMinimumWidth(80);
    rowLayout->addWidget(label);
    if (QLabel *helpLabel = createHelpBadgeLabel(tooltipText, rowWidget)) {
        rowLayout->addWidget(helpLabel);
    }
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
    spinBox->setFixedWidth(70);
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
    SignalLayerWidget::configureHostTree(treeWidget);
    setTreeVisibleRows(treeWidget, 2);
    connect(treeWidget, &QTreeWidget::itemDoubleClicked, this, &WatershedControl::treeDoubleClicked);
    connect(treeWidget, &QTreeWidget::itemClicked, this, &WatershedControl::treeClicked);
    connect(treeWidget, &QTreeWidget::currentItemChanged, this, [this, treeWidget](QTreeWidgetItem *, QTreeWidgetItem *) {
        updateLayerSelectionState(treeWidget);
    });
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
    SP_LOG_DEBUG_CHANGED("watershed",
                         QStringLiteral("thresholdPreviewValue"),
                         QStringLiteral("Threshold preview value=%1").arg(value));
    if (pThresholdPreviewSignal) {
        pThresholdPreviewSignal->thresholdValue = value;
        if (thresholdPreviewSignalIndex >= 0) {
            setSignalActive(static_cast<size_t>(thresholdPreviewSignalIndex), true);
            return;
        }
    }
    refreshViewers();
}

void WatershedControl::setupThresholdWidget() {
    thresholdBoundariesButton = new QPushButton("Threshold Boundaries", this);
    thresholdBoundariesButton->setObjectName("thresholdBoundariesButton");
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
    calculateDistanceMapButton->setObjectName("calculateDistanceMapButton");
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
    calculateSeedsButton->setObjectName("calculateSeedsButton");
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
    runWatershedButton->setObjectName("runWatershedButton");
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

void WatershedControl::setupAgglomertionWidget() {
    runAgglomertionButton = new QPushButton("Run", this);
    runAgglomertionButton->setObjectName("runAgglomertionButton");
    agglomertionInputComboBox = new QComboBox(this);
    agglomertionThresholdMaskComboBox = new QComboBox(this);
    agglomertionLinkageComboBox = new QComboBox(this);
    agglomertionBoundaryModeComboBox = new QComboBox(this);
    agglomertionStrategyComboBox = new QComboBox(this);
    auto *biasControl = createSliderWithSpinBox(agglomertionBiasSlider, agglomertionBiasSpinBox);
    agglomertionBiasSlider->setObjectName("agglomertionBiasSlider");
    agglomertionBiasSpinBox->setObjectName("agglomertionBiasSpinBox");
    agglomertionBiasValueLabel = new QLabel(this);
    agglomertionPreviewCheckBox = new QCheckBox("Live Preview", this);
    agglomertionPreviewCheckBox->setChecked(true);
    agglomertionApproximatePreviewCheckBox = new QCheckBox("Fast Approx.", this);
    agglomertionApproximatePreviewCheckBox->setChecked(false);
    agglomertionPreviewBoundariesCheckBox = new QCheckBox("Inject Boundaries", this);
    agglomertionPreviewBoundariesCheckBox->setChecked(true);
    agglomertionReplaceCheckBox = new QCheckBox(this);
    agglomertionReplaceCheckBox->setObjectName("agglomertionReplaceCheckBox");
    agglomertionReplaceCheckBox->setChecked(true);
    configureInputCombo(agglomertionInputComboBox);
    configureInputCombo(agglomertionThresholdMaskComboBox);
    agglomertionBiasSlider->setRange(0, 100);
    agglomertionBiasSpinBox->setRange(0, 100);
    agglomertionBiasSlider->setValue(50);
    agglomertionBiasValueLabel->setText(agglomertionBiasLabelText());

    auto *biasWidget = new QWidget(this);
    auto *biasLayout = new QVBoxLayout(biasWidget);
    biasLayout->setContentsMargins(0, 0, 0, 0);
    biasLayout->setSpacing(4);
    biasLayout->addWidget(biasControl);
    auto *biasLabelsLayout = new QHBoxLayout();
    biasLabelsLayout->setContentsMargins(0, 0, 0, 0);
    biasLabelsLayout->addWidget(new QLabel("More Splits", this));
    biasLabelsLayout->addStretch(1);
    biasLabelsLayout->addWidget(agglomertionBiasValueLabel);
    biasLabelsLayout->addStretch(1);
    biasLabelsLayout->addWidget(new QLabel("More Merges", this));
    biasLayout->addLayout(biasLabelsLayout);

    const QString thresholdMaskTooltip =
        "Binary mask from the threshold step. It tells agglomeration which contacts are open. "
        "Needed for Open Interface Mean and Open Fraction Weighted.";
    const QString scoringStrategyTooltip =
        "<b>Raw Interface Mean</b>: Uses the whole contact.<br><br>"
        "<b>Open Interface Mean</b>: Uses only unmasked contact.<br><br>"
        "<b>Open Fraction Weighted</b>: Uses only unmasked contact, but weakens merges when "
        "only a small part of the contact is open. <i>Default.</i>";
    const QString linkageTooltip =
        "<b>Average</b>: Uses mean evidence after each merge. Safer default.<br><br>"
        "<b>Sum</b>: Uses total accumulated evidence and is usually more aggressive on large contacts.";
    const QString boundaryScaleTooltip =
        "How boundary values are interpreted before scoring.<br><br>"
        "<b>Auto Detect</b> is usually correct. Override this only when you know the image scale.";
    const QString mergeBiasTooltip =
        "Left keeps more splits. Right merges more.<br><br>"
        "This changes tau.";
    const QString sizeBiasTooltip =
        "Optional small-region cleanup.<br><br>"
        "<b>Soft Bias</b> makes small regions merge earlier.<br>"
        "<b>Cleanup</b> removes leftover tiny regions after the main agglomeration.<br>"
        "<b>Soft Bias + Cleanup</b> does both.";
    const QString boundaryAwareTooltip =
        "Only used when Size Bias is on.<br><br>"
        "<b>On</b>: Size bias respects the threshold mask and avoids pulling tiny regions "
        "through blocked boundaries.<br>"
        "<b>Off</b>: Size bias may ignore the mask and use raw boundary evidence.";
    const QString replaceExistingTooltip =
        "Reuses the agglomertion output currently selected in stage 6 instead of creating a new volume.<br><br>"
        "If no agglomertion result is currently selected there, a new output is created.";

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Watershed", agglomertionInputComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Threshold Mask", agglomertionThresholdMaskComboBox, thresholdMaskTooltip));
    controlsLayout->addWidget(createLabeledInputRow(
        "Scoring Strategy",
        agglomertionStrategyComboBox,
        scoringStrategyTooltip));
    controlsLayout->addWidget(createLabeledInputRow(
        "Linkage",
        agglomertionLinkageComboBox,
        linkageTooltip));
    controlsLayout->addWidget(createLabeledInputRow(
        "Boundary Scale",
        agglomertionBoundaryModeComboBox,
        boundaryScaleTooltip));
    controlsLayout->addWidget(createLabeledInputRow(
        "Merge Bias",
        biasWidget,
        mergeBiasTooltip));
    controlsLayout->addWidget(agglomertionPreviewCheckBox);
    controlsLayout->addWidget(agglomertionApproximatePreviewCheckBox);
    controlsLayout->addWidget(agglomertionPreviewBoundariesCheckBox);
    controlsLayout->addWidget(createLabeledInputRow("Replace Existing", agglomertionReplaceCheckBox, replaceExistingTooltip));

    // Size bias group
    agglomertionSizeBiasCheckBox = new QCheckBox("Size Bias", this);
    agglomertionSizeBiasCheckBox->setObjectName("agglomertionSizeBiasCheckBox");
    agglomertionSizeBiasCheckBox->setChecked(false);
    agglomertionSizeBiasMaskCheckBox = new QCheckBox(this);
    agglomertionSizeBiasMaskCheckBox->setObjectName("agglomertionSizeBiasMaskCheckBox");
    agglomertionSizeBiasMaskCheckBox->setChecked(true);
    agglomertionSizeBiasMaskCheckBox->setToolTip(boundaryAwareTooltip);
    agglomertionSizeBiasStrategyComboBox = new QComboBox(this);
    agglomertionSizeBiasStrategyComboBox->setObjectName("agglomertionSizeBiasStrategyComboBox");
    auto *sizeBiasThresholdControl = createSliderWithSpinBox(agglomertionSizeBiasThresholdSlider, agglomertionSizeBiasThresholdSpinBox);
    agglomertionSizeBiasThresholdSlider->setObjectName("agglomertionSizeBiasThresholdSlider");
    agglomertionSizeBiasThresholdSpinBox->setObjectName("agglomertionSizeBiasThresholdSpinBox");

    auto *sizeBiasStrengthControl = createSliderWithSpinBox(agglomertionSizeBiasStrengthSlider, agglomertionSizeBiasStrengthSpinBox);
    agglomertionSizeBiasStrengthSlider->setObjectName("agglomertionSizeBiasStrengthSlider");
    agglomertionSizeBiasStrengthSpinBox->setObjectName("agglomertionSizeBiasStrengthSpinBox");

    auto *sizeBiasProtectionControl = createSliderWithSpinBox(agglomertionSizeBiasProtectionSlider, agglomertionSizeBiasProtectionSpinBox);
    agglomertionSizeBiasProtectionSlider->setObjectName("agglomertionSizeBiasProtectionSlider");
    agglomertionSizeBiasProtectionSpinBox->setObjectName("agglomertionSizeBiasProtectionSpinBox");

    agglomertionSizeBiasThresholdSlider->setRange(100, 50000);
    agglomertionSizeBiasThresholdSpinBox->setRange(100, 50000);
    agglomertionSizeBiasThresholdSlider->setValue(5000);
    agglomertionSizeBiasStrengthSlider->setRange(0, 100);
    agglomertionSizeBiasStrengthSpinBox->setRange(0, 100);
    agglomertionSizeBiasStrengthSlider->setValue(30);
    agglomertionSizeBiasProtectionSlider->setRange(0, 100);
    agglomertionSizeBiasProtectionSpinBox->setRange(0, 100);
    agglomertionSizeBiasProtectionSlider->setValue(30);

    agglomertionSizeBiasStrategyComboBox->setEnabled(false);
    agglomertionSizeBiasThresholdSlider->setEnabled(false);
    agglomertionSizeBiasThresholdSpinBox->setEnabled(false);
    agglomertionSizeBiasStrengthSlider->setEnabled(false);
    agglomertionSizeBiasStrengthSpinBox->setEnabled(false);
    agglomertionSizeBiasProtectionSlider->setEnabled(false);
    agglomertionSizeBiasProtectionSpinBox->setEnabled(false);

    controlsLayout->addWidget(createLabeledInputRow("Size Bias", agglomertionSizeBiasCheckBox, sizeBiasTooltip));
    controlsLayout->addWidget(createLabeledInputRow("Boundary-Aware", agglomertionSizeBiasMaskCheckBox, boundaryAwareTooltip));
    controlsLayout->addWidget(createLabeledInputRow("Strategy", agglomertionSizeBiasStrategyComboBox));
    controlsLayout->addWidget(createLabeledInputRow("Small Size", sizeBiasThresholdControl));
    controlsLayout->addWidget(createLabeledInputRow("Strength", sizeBiasStrengthControl));
    controlsLayout->addWidget(createLabeledInputRow("Protection", sizeBiasProtectionControl));

    auto *groupBox = createStepGroup("5. Agglomertion");
    addStepSection(groupBox, controlsWidget, runAgglomertionButton);

    connect(runAgglomertionButton, &QPushButton::clicked, this, &WatershedControl::agglomertionPressed);
    connect(agglomertionInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
    connect(agglomertionInputComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionThresholdMaskComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
    connect(agglomertionThresholdMaskComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionStrategyComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::updateStepEnablement);
    connect(agglomertionStrategyComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionLinkageComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionBoundaryModeComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionBiasSlider, &QSlider::valueChanged, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionPreviewCheckBox, &QCheckBox::toggled, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionApproximatePreviewCheckBox, &QCheckBox::toggled, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionPreviewBoundariesCheckBox, &QCheckBox::toggled, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionSizeBiasCheckBox, &QCheckBox::toggled, this, [this](bool on) {
        agglomertionSizeBiasStrategyComboBox->setEnabled(on);
        agglomertionSizeBiasThresholdSlider->setEnabled(on);
        agglomertionSizeBiasThresholdSpinBox->setEnabled(on);
        agglomertionSizeBiasStrengthSlider->setEnabled(on);
        agglomertionSizeBiasStrengthSpinBox->setEnabled(on);
        agglomertionSizeBiasProtectionSlider->setEnabled(on);
        agglomertionSizeBiasProtectionSpinBox->setEnabled(on);
        agglomertionPreviewSettingsChanged();
    });
    connect(agglomertionSizeBiasMaskCheckBox, &QCheckBox::toggled, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionSizeBiasStrategyComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionSizeBiasThresholdSlider, &QSlider::valueChanged, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionSizeBiasStrengthSlider, &QSlider::valueChanged, this, &WatershedControl::agglomertionPreviewSettingsChanged);
    connect(agglomertionSizeBiasProtectionSlider, &QSlider::valueChanged, this, &WatershedControl::agglomertionPreviewSettingsChanged);
}

void WatershedControl::setupFinalizeWidget() {
    inspectSegmentsButton = new QPushButton("Inspect Segments", this);
    inspectSegmentsButton->setObjectName("inspectSegmentsButton");
    createRefinementButton = new QPushButton(
        outputMode == OutputMode::Segments ? "Export Segments" : "Create Refinement",
        this);
    createRefinementButton->setObjectName("createRefinementButton");
    finalOutputInputComboBox = new QComboBox(this);
    finalOutputInputComboBox->setObjectName("finalOutputInputComboBox");
    configureInputCombo(finalOutputInputComboBox);

    auto *controlsWidget = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(controlsWidget);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(createLabeledInputRow("Agglomertion", finalOutputInputComboBox));

    auto *groupBox = createStepGroup(
        outputMode == OutputMode::Segments ? "6. Export Segments" : "6. Create Refinement");
    auto *layout = qobject_cast<QVBoxLayout *>(groupBox->layout());
    layout->addWidget(controlsWidget);
    layout->addWidget(inspectSegmentsButton);
    layout->addWidget(createRefinementButton);
    workflowLayout->addWidget(groupBox, 0);

    auto *inspectShortcut = new QShortcut(QKeySequence(Qt::Key_F8), this);
    connect(inspectShortcut, &QShortcut::activated, this, &WatershedControl::inspectSegmentsPressed);
    connect(inspectSegmentsButton, &QPushButton::clicked, this, &WatershedControl::inspectSegmentsPressed);
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

    configureAlgorithmCombo(agglomertionLinkageComboBox);
    agglomertionLinkageComboBox->addItem(
        agglomertionLinkageLabel(segment_puzzler::RagLinkage::Average),
        static_cast<int>(segment_puzzler::RagLinkage::Average));
    agglomertionLinkageComboBox->addItem(
        agglomertionLinkageLabel(segment_puzzler::RagLinkage::Sum),
        static_cast<int>(segment_puzzler::RagLinkage::Sum));
    agglomertionLinkageComboBox->setCurrentIndex(0);

    configureAlgorithmCombo(agglomertionStrategyComboBox);
    agglomertionStrategyComboBox->addItem(
        agglomertionBoundaryEvidenceStrategyLabel(segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted),
        static_cast<int>(segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted));
    agglomertionStrategyComboBox->addItem(
        agglomertionBoundaryEvidenceStrategyLabel(segment_puzzler::BoundaryEvidenceStrategy::OpenInterfaceMean),
        static_cast<int>(segment_puzzler::BoundaryEvidenceStrategy::OpenInterfaceMean));
    agglomertionStrategyComboBox->addItem(
        agglomertionBoundaryEvidenceStrategyLabel(segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean),
        static_cast<int>(segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean));
    agglomertionStrategyComboBox->setCurrentIndex(0);

    configureAlgorithmCombo(agglomertionBoundaryModeComboBox);
    agglomertionBoundaryModeComboBox->addItem(
        agglomertionBoundaryModeLabel(segment_puzzler::BoundaryNormalizationMode::AutoDetect),
        static_cast<int>(segment_puzzler::BoundaryNormalizationMode::AutoDetect));
    agglomertionBoundaryModeComboBox->addItem(
        agglomertionBoundaryModeLabel(segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne),
        static_cast<int>(segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne));
    agglomertionBoundaryModeComboBox->addItem(
        agglomertionBoundaryModeLabel(segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToTwo),
        static_cast<int>(segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToTwo));
    agglomertionBoundaryModeComboBox->addItem(
        agglomertionBoundaryModeLabel(segment_puzzler::BoundaryNormalizationMode::UInt8FullRange),
        static_cast<int>(segment_puzzler::BoundaryNormalizationMode::UInt8FullRange));
    agglomertionBoundaryModeComboBox->addItem(
        agglomertionBoundaryModeLabel(segment_puzzler::BoundaryNormalizationMode::UInt16FullRange),
        static_cast<int>(segment_puzzler::BoundaryNormalizationMode::UInt16FullRange));
    agglomertionBoundaryModeComboBox->setCurrentIndex(0);

    configureAlgorithmCombo(agglomertionSizeBiasStrategyComboBox);
    agglomertionSizeBiasStrategyComboBox->addItem(
        sizeBiasStrategyLabel(segment_puzzler::SizeBiasStrategy::SoftBias),
        static_cast<int>(segment_puzzler::SizeBiasStrategy::SoftBias));
    agglomertionSizeBiasStrategyComboBox->addItem(
        sizeBiasStrategyLabel(segment_puzzler::SizeBiasStrategy::Cleanup),
        static_cast<int>(segment_puzzler::SizeBiasStrategy::Cleanup));
    agglomertionSizeBiasStrategyComboBox->addItem(
        sizeBiasStrategyLabel(segment_puzzler::SizeBiasStrategy::SoftBiasAndCleanup),
        static_cast<int>(segment_puzzler::SizeBiasStrategy::SoftBiasAndCleanup));
    agglomertionSizeBiasStrategyComboBox->setCurrentIndex(0);
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
    refreshComboSelection(agglomertionInputComboBox, agglomertionInputSignalIndices);
    refreshComboSelection(agglomertionThresholdMaskComboBox, thresholdOutputSignalIndices);
    refreshComboSelection(finalOutputInputComboBox, watershedOutputSignalIndices);
    scheduleAgglomertionPreviewRefresh();
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

const WatershedControl::GeneratedStageOutput *WatershedControl::generatedStageOutput(size_t signalIndex) const {
    auto it = generatedStageOutputs.find(signalIndex);
    return it != generatedStageOutputs.end() ? &it->second : nullptr;
}

const WatershedControl::GeneratedStageOutput *WatershedControl::selectedAgglomertionStageOutput() const {
    if (!comboHasValidSelection(agglomertionInputComboBox)) {
        return nullptr;
    }
    return generatedStageOutput(selectedSignalIndex(agglomertionInputComboBox));
}

const WatershedControl::GeneratedStageOutput *WatershedControl::selectedFinalAgglomertionStageOutput() const {
    if (!comboHasValidSelection(finalOutputInputComboBox)) {
        return nullptr;
    }

    const size_t signalIndex = selectedSignalIndex(finalOutputInputComboBox);
    const auto *stageOutput = generatedStageOutput(signalIndex);
    if (stageOutput == nullptr || stageOutput->stage != SignalStage::Agglomertion) {
        return nullptr;
    }
    return stageOutput;
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
    const size_t signalIndex = selectedSignalIndex(finalOutputInputComboBox);
    if (const auto *stageOutput = generatedStageOutput(signalIndex)) {
        return stageOutput->displayLabels;
    }
    auto *signal = dynamic_cast<itkSignal<GraphSegmentType> *>(allSignalList[signalIndex]);
    return signal ? signal->pImage : nullptr;
}

itkSignal<WatershedControl::GraphSegmentType> *WatershedControl::selectedFinalOutputSignal() const {
    if (!comboHasValidSelection(finalOutputInputComboBox)) {
        return nullptr;
    }
    return dynamic_cast<itkSignal<GraphSegmentType> *>(allSignalList[selectedSignalIndex(finalOutputInputComboBox)]);
}

itkSignal<WatershedControl::GraphSegmentType> *WatershedControl::selectedFinalAgglomertionSignal() const {
    if (selectedFinalAgglomertionStageOutput() == nullptr || !comboHasValidSelection(finalOutputInputComboBox)) {
        return nullptr;
    }
    return dynamic_cast<itkSignal<GraphSegmentType> *>(allSignalList[selectedSignalIndex(finalOutputInputComboBox)]);
}

bool WatershedControl::hasActiveAgglomertionPreview() const {
    return pAgglomertionPreviewSignal != nullptr
           && pAgglomertionPreviewSignal->pImage.IsNotNull()
           && pAgglomertionPreviewSignal->getIsActive();
}

bool WatershedControl::tryResolveInspectSegmentsTarget(dataType::SegmentsImageType::Pointer &segmentsImage,
                                                       itkSignal<GraphSegmentType> *&segmentsSignal,
                                                       QString &errorMessage) const {
    if (const auto *stageOutput = selectedFinalAgglomertionStageOutput()) {
        segmentsImage = stageOutput->displayLabels;
        segmentsSignal = selectedFinalAgglomertionSignal();
        return segmentsImage != nullptr && segmentsSignal != nullptr;
    }

    if (hasActiveAgglomertionPreview()) {
        segmentsImage = pAgglomertionPreviewSignal->pImage;
        segmentsSignal = pAgglomertionPreviewSignal;
        return true;
    }

    errorMessage = "Please run agglomertion first, or enable Live Preview and wait for the preview to appear.";
    return false;
}

void WatershedControl::updateRegisteredSignalName(size_t signalIndex, const QString &requestedName) {
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        return;
    }

    const QString uniqueName = makeUniqueSignalNameExcludingIndex(allSignalList, signalIndex, requestedName);
    allSignalList[signalIndex]->setName(uniqueName);

    if (QTreeWidgetItem *treeItem = findSignalTreeItem(signalTreeWidget, signalIndex)) {
        treeItem->setText(0, uniqueName);
    }

    if (finalOutputInputComboBox != nullptr) {
        const int comboIndex = finalOutputInputComboBox->findData(static_cast<int>(signalIndex));
        if (comboIndex >= 0) {
            finalOutputInputComboBox->setItemText(comboIndex, uniqueName);
        }
    }
}

dataType::SegmentsImageType::Pointer WatershedControl::selectedAgglomertionInput() const {
    const auto *stageOutput = selectedAgglomertionStageOutput();
    return stageOutput != nullptr ? stageOutput->canonicalLabels : nullptr;
}

itk::Image<unsigned char, 3>::Pointer WatershedControl::selectedAgglomertionThresholdMask() const {
    if (!comboHasValidSelection(agglomertionThresholdMaskComboBox)) {
        return nullptr;
    }
    auto *signal = dynamic_cast<itkSignal<unsigned char> *>(allSignalList[selectedSignalIndex(agglomertionThresholdMaskComboBox)]);
    return signal ? signal->pImage : nullptr;
}

segment_puzzler::RagLinkage WatershedControl::selectedAgglomertionLinkage() const {
    return static_cast<segment_puzzler::RagLinkage>(agglomertionLinkageComboBox->currentData().toInt());
}

segment_puzzler::BoundaryNormalizationMode WatershedControl::selectedAgglomertionBoundaryNormalization() const {
    return static_cast<segment_puzzler::BoundaryNormalizationMode>(agglomertionBoundaryModeComboBox->currentData().toInt());
}

segment_puzzler::BoundaryEvidenceStrategy WatershedControl::selectedAgglomertionBoundaryEvidenceStrategy() const {
    return static_cast<segment_puzzler::BoundaryEvidenceStrategy>(agglomertionStrategyComboBox->currentData().toInt());
}

double WatershedControl::selectedAgglomertionTau() const {
    return static_cast<double>(agglomertionBiasSlider->value()) / 100.0;
}

segment_puzzler::SizeBiasStrategy WatershedControl::selectedSizeBiasStrategy() const {
    return static_cast<segment_puzzler::SizeBiasStrategy>(agglomertionSizeBiasStrategyComboBox->currentData().toInt());
}

uint64_t WatershedControl::selectedSizeBiasThreshold() const {
    return static_cast<uint64_t>(agglomertionSizeBiasThresholdSlider->value());
}

double WatershedControl::selectedSizeBiasStrength() const {
    return static_cast<double>(agglomertionSizeBiasStrengthSlider->value()) / 100.0;
}

double WatershedControl::selectedSizeBiasProtection() const {
    return static_cast<double>(agglomertionSizeBiasProtectionSlider->value()) / 100.0;
}

bool WatershedControl::agglomertionNeedsThresholdMask() const {
    return selectedAgglomertionBoundaryEvidenceStrategy() != segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean;
}

segment_puzzler::WatershedRagAgglomerationOptions WatershedControl::currentAgglomertionOptions() const {
    segment_puzzler::WatershedRagAgglomerationOptions options;
    options.linkage = selectedAgglomertionLinkage();
    options.boundaryNormalization = selectedAgglomertionBoundaryNormalization();
    options.boundaryEvidenceStrategy = selectedAgglomertionBoundaryEvidenceStrategy();
    options.tau = selectedAgglomertionTau();
    options.threadCount = workerThreadCount;
    if (agglomertionSizeBiasCheckBox != nullptr && agglomertionSizeBiasCheckBox->isChecked()) {
        options.sizeBiasStrategy = selectedSizeBiasStrategy();
        options.sizeBiasThreshold = selectedSizeBiasThreshold();
        options.sizeBiasStrength = selectedSizeBiasStrength();
        options.sizeBiasProtection = selectedSizeBiasProtection();
        options.sizeBiasRespectMask = agglomertionSizeBiasMaskCheckBox->isChecked();
    }
    return options;
}

void WatershedControl::updateStepEnablement() {
    const bool hasBoundary = selectedBoundaryInput().IsNotNull();
    const bool hasThreshold = selectedThresholdInput().IsNotNull();
    const bool hasDistanceMap = selectedDistanceMapInput().IsNotNull();
    const bool hasWatershedDistanceMap = selectedWatershedDistanceMapInput().IsNotNull();
    const bool hasWatershedSeeds = selectedWatershedSeedsInput().IsNotNull();
    const bool hasWatershedThreshold = selectedWatershedThresholdInput().IsNotNull();
    const bool hasAgglomertionInput = selectedAgglomertionInput().IsNotNull();
    const bool hasAgglomertionThresholdMask = selectedAgglomertionThresholdMask().IsNotNull();
    const bool hasRequiredAgglomertionMask = !agglomertionNeedsThresholdMask() || hasAgglomertionThresholdMask;
    const bool hasFinalOutput = selectedFinalOutputInput().IsNotNull();
    const bool hasPersistedAgglomertionOutput = selectedFinalAgglomertionStageOutput() != nullptr;
    const bool hasInspectableAgglomertion = hasPersistedAgglomertionOutput || hasActiveAgglomertionPreview();

    thresholdBoundariesButton->setEnabled(hasBoundary);
    thresholdValueSlider->setEnabled(hasBoundary);
    calculateDistanceMapButton->setEnabled(hasThreshold);
    calculateSeedsButton->setEnabled(hasDistanceMap);
    runWatershedButton->setEnabled(hasWatershedDistanceMap && hasWatershedSeeds && hasWatershedThreshold);
    runAgglomertionButton->setEnabled(hasAgglomertionInput && hasBoundary && hasRequiredAgglomertionMask);
    agglomertionPreviewCheckBox->setEnabled(hasAgglomertionInput && hasBoundary && hasRequiredAgglomertionMask && !taskRunner->isBusy());
    agglomertionApproximatePreviewCheckBox->setEnabled(hasAgglomertionInput && hasBoundary && hasRequiredAgglomertionMask && !taskRunner->isBusy());
    agglomertionReplaceCheckBox->setEnabled(hasAgglomertionInput && hasBoundary && hasRequiredAgglomertionMask && hasPersistedAgglomertionOutput && !taskRunner->isBusy());
    inspectSegmentsButton->setEnabled(hasInspectableAgglomertion);
    createRefinementButton->setEnabled(hasFinalOutput);
    if (!(hasAgglomertionInput && hasBoundary && hasRequiredAgglomertionMask)) {
        clearAgglomertionPreview();
    }
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

QString WatershedControl::agglomertionLinkageLabel(segment_puzzler::RagLinkage linkage) const {
    switch (linkage) {
        case segment_puzzler::RagLinkage::Average:
            return "Average";
        case segment_puzzler::RagLinkage::Sum:
            return "Sum";
    }
    return "Average";
}

QString WatershedControl::agglomertionBoundaryModeLabel(segment_puzzler::BoundaryNormalizationMode mode) const {
    switch (mode) {
        case segment_puzzler::BoundaryNormalizationMode::AutoDetect:
            return "Auto Detect";
        case segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne:
            return "0..1 Probability";
        case segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToTwo:
            return "0..2 Probability";
        case segment_puzzler::BoundaryNormalizationMode::UInt8FullRange:
            return "UInt8 Full Range";
        case segment_puzzler::BoundaryNormalizationMode::UInt16FullRange:
            return "UInt16 Full Range";
    }
    return "UInt16 Full Range";
}

QString WatershedControl::agglomertionBoundaryEvidenceStrategyLabel(segment_puzzler::BoundaryEvidenceStrategy strategy) const {
    switch (strategy) {
        case segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean:
            return "Raw Interface Mean";
        case segment_puzzler::BoundaryEvidenceStrategy::OpenInterfaceMean:
            return "Open Interface Mean";
        case segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted:
            return "Open Fraction Weighted";
    }
    return "Open Fraction Weighted";
}

QString WatershedControl::agglomertionBiasLabelText() const {
    const double tau = selectedAgglomertionTau();
    return QString("tau %1").arg(tau, 0, 'f', 2);
}

QString WatershedControl::sizeBiasStrategyLabel(segment_puzzler::SizeBiasStrategy strategy) const {
    switch (strategy) {
        case segment_puzzler::SizeBiasStrategy::Off:          return "Off";
        case segment_puzzler::SizeBiasStrategy::SoftBias:     return "Soft Bias";
        case segment_puzzler::SizeBiasStrategy::Cleanup:      return "Cleanup";
        case segment_puzzler::SizeBiasStrategy::SoftBiasAndCleanup: return "Soft Bias + Cleanup";
    }
    return "Soft Bias";
}


void WatershedControl::treeDoubleClicked(QTreeWidgetItem *item, int) {
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


void WatershedControl::treeClicked(QTreeWidgetItem *item, int) {
    if (signal_tree::rowKind(item) != signal_tree::RowKind::Root) {
        return;
    }

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
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Opening color picker for watershed signal index=%1").arg(signalIndex));
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

void WatershedControl::setDescription(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Renaming watershed signal index=%1").arg(signalIndex));
    }

    bool inputSuccessful;
    QString newName =
            QInputDialog::getText(this, "Set New Description", "Enter new Descriptor:", QLineEdit::Normal, QString(),
                                  &inputSuccessful);

    if (inputSuccessful) {
        if (isSegmentsItem(item)) { // TODO: put some unique identifier besides name/descriptor for segments
            SP_LOG_WARNING("watershed", QStringLiteral("Segments descriptor renaming is still not implemented"));
        } else {
            allSignalList[signalIndex]->setName(newName);
            refreshLayerWidget(item->treeWidget(), item);
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
        if (signal_tree::signalIndex(treeItem) == signalIdx) {
            const bool treeWasActive = treeItem->checkState(0) == Qt::Checked;
            if (treeWasActive != active) {
                treeItem->setCheckState(0, active ? Qt::Checked : Qt::Unchecked);
            }
            setIsActive(treeItem, active);
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
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Setting watershed signal active state to %1").arg(isActiveIn));
    }
    const size_t signalIndex = signalIndexForItem(item);

    allSignalList[signalIndex]->setIsActive(isActiveIn);

    for (auto *viewer : orthoViewer->viewerList) {
        viewer->setSliceIndex(viewer->getSliceIndex()); // update slice indices of newly activated signals
        viewer->recalculateQImages();
    }

    refreshLayerWidget(item->treeWidget(), item);
}

void WatershedControl::setUserNorm(QTreeWidgetItem *item) {
    openNormPopup(item, this);
}

void WatershedControl::setUserAlpha(QTreeWidgetItem *item) {
    openOpacityPopup(item, this);
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
        if (signal_tree::signalIndex(treeItem) == static_cast<size_t>(registeredEdgeSignalIndex)) {
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
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    SP_LOG_INFO("watershed", QStringLiteral("Building graph from watershed segments volume"));
    graphBase->pGraph->constructFromVolume(segmentsImage);
    SP_LOG_INFO("watershed",
                QStringLiteral("Finished building graph from watershed segments volume in %1 ms")
                    .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
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
        SP_LOG_INFO("io", QStringLiteral("Detected watershed image dimension=%1 for %2").arg(dimension).arg(fileName));
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
        SP_LOG_INFO("io", QStringLiteral("Loading watershed boundaries from %1").arg(fileName));
        QSettings MySettings;
        QDir CurrentDir;
        const QString DEFAULT_SAVE_DIR_KEY("default_save_dir");
        MySettings.setValue(DEFAULT_SAVE_DIR_KEY, CurrentDir.absoluteFilePath(fileName));
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

    SP_LOG_INFO("watershed",
                QStringLiteral("Adding watershed boundaries with requested ROI x=%1-%2 y=%3-%4 z=%5-%6")
                    .arg(fx).arg(tx).arg(fy).arg(ty).arg(fz).arg(tz));

    fxIn = fxIn < 0 ? 0 : fxIn;
    fyIn = fyIn < 0 ? 0 : fyIn;
    fzIn = fzIn < 0 ? 0 : fzIn;

    auto originalSize = pBoundariesIn->GetLargestPossibleRegion().GetSize();
    SP_LOG_DEBUG("watershed",
                 QStringLiteral("Boundary image size=%1x%2x%3")
                     .arg(originalSize[0]).arg(originalSize[1]).arg(originalSize[2]));

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

    SP_LOG_INFO("watershed",
                QStringLiteral("Clamped watershed ROI to x=%1-%2 y=%3-%4 z=%5-%6")
                    .arg(fxIn).arg(txIn).arg(fyIn).arg(tyIn).arg(fzIn).arg(tzIn));

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
    SP_LOG_INFO("watershed", QStringLiteral("Boundary ROI intensity range min=%1 max=%2").arg(min).arg(max));

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
    pThresholdPreviewSignal->setMainColor(255, 255, 0); // Yellow
    thresholdPreviewSignalIndex = static_cast<int>(registerSignal(
        std::move(previewSignal), SignalStage::None, "Threshold Preview", false, true, true));

    int minValue = pThresholdPreviewSignal->getMinimumValue();
    int maxValue = pThresholdPreviewSignal->getMaximumValue();

    const QSignalBlocker sliderBlocker(thresholdValueSlider);
    const QSignalBlocker spinBlocker(thresholdValueSpinBox);
    thresholdValueSlider->setRange(minValue, maxValue);
    thresholdValueSpinBox->setRange(minValue, maxValue);
    const int initialThresholdValue = static_cast<int>((minValue + maxValue)/2);
    thresholdValueSlider->setValue(initialThresholdValue);
    thresholdValueSpinBox->setValue(initialThresholdValue);
    pThresholdPreviewSignal->thresholdValue = initialThresholdValue;
    
    refreshInputSelectors();
    updateStepEnablement();
    refreshViewers();
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

void WatershedControl::agglomertionPressed() {
    if (selectedAgglomertionInput() != nullptr && selectedBoundaryInput() != nullptr) {
        agglomertionAsync();
    } else {
        QMessageBox msgBox;
        msgBox.setText("Please generate a watershed first.");
        msgBox.exec();
    }
}

void WatershedControl::inspectSegmentsPressed() {
    itkSignal<GraphSegmentType> *selectedSignal = nullptr;
    dataType::SegmentsImageType::Pointer selectedOutput;
    QString errorMessage;
    if (!tryResolveInspectSegmentsTarget(selectedOutput, selectedSignal, errorMessage)) {
        QMessageBox msgBox;
        msgBox.setText(errorMessage);
        msgBox.exec();
        return;
    }

    graphBase->pSelectedSegmentation = selectedOutput;
    graphBase->pSelectedSegmentationSignal = selectedSignal;
    graphBase->selectedSegmentationMaxSegmentId =
        graphBase->pGraph != nullptr ? graphBase->pGraph->getLargestIdInSegmentVolume(selectedOutput) : 0;

    if (orthoViewer != nullptr) {
        orthoViewer->flashShortcutLegendKey("f8");
    }

    if (!segmentTableDialog) {
        segmentTableDialog = new SegmentTableDialog(graphBase, orthoViewer, this);
        segmentTableDialog->setAttribute(Qt::WA_DeleteOnClose);
        segmentTableDialog->setQuickComputeMode();
    }
    segmentTableDialog->show();
    segmentTableDialog->raise();
    segmentTableDialog->activateWindow();
    segmentTableDialog->startCompute(selectedOutput);
}

void WatershedControl::finalizeOutputPressed() {
    if (selectedFinalOutputInput() != nullptr) {
        createRefinementAsync();
    } else {
        QMessageBox msgBox;
        msgBox.setText(outputMode == OutputMode::Segments
                           ? "Please generate a watershed or agglomertion before exporting segments."
                           : "Please generate a watershed or agglomertion first.");
        msgBox.exec();
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
    return signal_tree::topLevelSignalItem(item);
}

size_t WatershedControl::signalIndexForItem(QTreeWidgetItem *item) const {
    if (topLevelItem(item) == nullptr) {
        throw std::logic_error("signal item not found!");
    }

    const size_t signalIndex = signal_tree::signalIndex(item);
    if (signalIndex >= allSignalList.size() || allSignalList[signalIndex] == nullptr) {
        throw std::logic_error("signal index not found!");
    }

    return signalIndex;
}

itkSignalBase *WatershedControl::signalForItem(QTreeWidgetItem *item) const {
    return allSignalList[signalIndexForItem(item)];
}

bool WatershedControl::isSegmentsItem(QTreeWidgetItem *item) const {
    return itkSignalSegmentsGraph != nullptr && signalForItem(item) == itkSignalSegmentsGraph;
}
