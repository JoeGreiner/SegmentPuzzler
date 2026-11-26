#include "src/image_storage/LabelImageStorage.h"
#include "SignalControl.h"
#include "src/segment_handling/graphBase.h"
#include "src/segment_handling/Graph.h"
#include "src/viewers/fileIO.h"
#include "MainWindowWatershedControl.h"
#include "src/viewers/OrthoViewer.h"
#include "src/qtUtils/SignalLayerWidget.h"
#include "src/qtUtils/SliderUpdateTimer.h"
#include "src/qtUtils/LabelBoundaryWidthControl.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SignalTreeWidgetUtils.h"
#include "src/qtUtils/ImageNormalizationSettingsDialog.h"
#include "src/qtUtils/NonOverlappingTransferSettings.h"
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include <QFileDialog>
#include <QColorDialog>
#include <QCheckBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QActionGroup>
#include <QInputDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringList>
#include <QToolButton>
#include <QWidgetAction>
#include <src/qtUtils/QImageSelectionRadioButtons.h>
#include <QSettings>
#include <QThread>
#include <QApplication>
#include <QImageReader>
#include <QTimer>
#include <clocale>
#include <climits>
#include <array>
#include <algorithm>
#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>
#include <itkImageIORegion.h>
#include <itkTIFFImageIO.h>
#include <itkVectorImage.h>
#include <itkVectorIndexSelectionCastImageFilter.h>
#include <limits>
#include <cmath>
#include <type_traits>

#include "src/utils/SignalNameUtils.h"
#include "src/utils/AppLogger.h"
#include "src/utils/ExportPathUtils.h"

namespace {

constexpr int kSectionSpacing = 4;
// Default visible-row targets for the initial layout. Trees scroll once more
// items are loaded.
constexpr int kLayersVisibleRows = 4;
constexpr int kOtherSectionVisibleRows = 2;
constexpr int kApproximateTreeRowPadding = 8;

QString neighborSelectionDisplayName(Graph::SegmentationNeighborSelection selection) {
    using Selection = Graph::SegmentationNeighborSelection;
    switch (selection) {
        case Selection::Smallest:
            return QCoreApplication::translate("SignalControl", "Smallest neighbor");
        case Selection::Largest:
            return QCoreApplication::translate("SignalControl", "Largest neighbor");
        case Selection::MostConnected:
            return QCoreApplication::translate("SignalControl", "Most connected neighbor");
    }
    return QCoreApplication::translate("SignalControl", "Most connected neighbor");
}

Graph::SegmentationNeighborSelection loadNeighborSelection(QSettings &settings) {
    using Selection = Graph::SegmentationNeighborSelection;
    const int storedValue = settings.value(
        QStringLiteral("Segmentation/neighborMergeSelection"),
        static_cast<int>(Selection::MostConnected)).toInt();
    const auto selection = static_cast<Selection>(storedValue);
    return Graph::segmentationNeighborSelectionName(selection) != nullptr
               ? selection
               : Selection::MostConnected;
}

void saveNeighborSelection(
    QSettings &settings,
    Graph::SegmentationNeighborSelection selection) {
    settings.setValue(
        QStringLiteral("Segmentation/neighborMergeSelection"),
        static_cast<int>(selection));
}

QComboBox *createNeighborSelectionComboBox(
    QWidget *parent,
    Graph::SegmentationNeighborSelection selectedSelection) {
    using Selection = Graph::SegmentationNeighborSelection;
    auto *comboBox = new QComboBox(parent);
    for (const Selection selection :
         {Selection::Smallest, Selection::Largest, Selection::MostConnected}) {
        comboBox->addItem(neighborSelectionDisplayName(selection), static_cast<int>(selection));
    }

    int selectedIndex = comboBox->findData(static_cast<int>(selectedSelection));
    if (selectedIndex < 0) {
        selectedIndex = comboBox->findData(static_cast<int>(Selection::MostConnected));
    }
    comboBox->setCurrentIndex(selectedIndex);
    comboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    comboBox->setMaxVisibleItems(comboBox->count());
    return comboBox;
}

std::optional<Graph::SegmentationNeighborSelection> askNeighborSelection(
    QWidget *parent,
    Graph::SegmentationNeighborSelection previousSelection) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("SignalControl", "Merge with Neighbor"));
    auto *form = new QFormLayout(&dialog);
    auto *comboBox = createNeighborSelectionComboBox(&dialog, previousSelection);
    form->addRow(QCoreApplication::translate("SignalControl", "Merge with:"), comboBox);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return static_cast<Graph::SegmentationNeighborSelection>(
        comboBox->currentData().toInt());
}

std::vector<Graph::SegmentIdType> newLabelsBelowVoxelCount(
    const Graph::SegmentationNeighborMergeResult &result,
    std::size_t exclusiveVoxelThreshold) {
    std::vector<Graph::SegmentIdType> labels;
    labels.reserve(result.voxelCountByNewLabel.size());
    for (const auto &[label, voxelCount] : result.voxelCountByNewLabel) {
        if (voxelCount < exclusiveVoxelThreshold) {
            labels.push_back(label);
        }
    }
    return labels;
}

const std::array<QColor, 5> &defaultAdditiveImageColors() {
    static const std::array<QColor, 5> colors = {
        QColor(QStringLiteral("#00ff66")),
        QColor(QStringLiteral("#ff4dff")),
        QColor(QStringLiteral("#00dfff")),
        QColor(QStringLiteral("#ff9f1c")),
        QColor(QStringLiteral("#7a8cff"))
    };
    return colors;
}

QString formatVoxelSpacing(const voxel_geometry::VoxelSpacing &spacing) {
    return QStringLiteral("X: %1, Y: %2, Z: %3")
        .arg(spacing.x, 0, 'g', 12)
        .arg(spacing.y, 0, 'g', 12)
        .arg(spacing.z, 0, 'g', 12);
}

void applyVoxelSpacingOverride(
    itk::ImageBase<3> *image,
    const std::optional<voxel_geometry::VoxelSpacing> &spacingOverride) {
    if (image == nullptr || !spacingOverride.has_value()) {
        return;
    }
    auto itkSpacing = image->GetSpacing();
    voxel_geometry::assignToItkSpacing(*spacingOverride, itkSpacing);
    image->SetSpacing(itkSpacing);
}

class FirstPageTIFFImageIO final : public itk::TIFFImageIO {
public:
    ITK_DISALLOW_COPY_AND_MOVE(FirstPageTIFFImageIO);

    using Self = FirstPageTIFFImageIO;
    using Superclass = itk::TIFFImageIO;
    using Pointer = itk::SmartPointer<Self>;

    itkNewMacro(Self);
    itkOverrideGetNameOfClassMacro(FirstPageTIFFImageIO);

    void ReadImageInformation() override {
        Superclass::ReadImageInformation();
        SetNumberOfDimensions(2);
    }

    void Read(void *buffer) override {
        const itk::ImageIORegion originalRegion = GetIORegion();
        itk::ImageIORegion firstPageRegion(2);
        for (unsigned int axis = 0; axis < 2; ++axis) {
            firstPageRegion.SetIndex(axis, originalRegion.GetIndex(axis));
            firstPageRegion.SetSize(axis, originalRegion.GetSize(axis));
        }

        SetIORegion(firstPageRegion);
        try {
            Superclass::Read(buffer);
        } catch (...) {
            SetIORegion(originalRegion);
            throw;
        }
        SetIORegion(originalRegion);
    }

protected:
    FirstPageTIFFImageIO() = default;
    ~FirstPageTIFFImageIO() override = default;
};

bool hasConfirmedDifferentlySizedTiffPages(const QString &fileName) {
    QImageReader reader(fileName, QByteArrayLiteral("tiff"));
    auto inspectionFailed = [&](const QString &reason) {
        SP_LOG_WARNING(
            "io",
            QStringLiteral(
                "Could not inspect TIFF page dimensions for %1 (%2); using the normal ITK reader")
                .arg(fileName, reason));
        return false;
    };

    if (!reader.canRead()) {
        return inspectionFailed(reader.errorString());
    }

    const int pageCount = reader.imageCount();
    if (pageCount <= 1) {
        return inspectionFailed(
            QStringLiteral("QImageReader reported %1 pages").arg(pageCount));
    }
    if (!reader.jumpToImage(0)) {
        return inspectionFailed(
            QStringLiteral("could not select page 0: %1").arg(reader.errorString()));
    }

    const QSize firstPageSize = reader.size();
    if (!firstPageSize.isValid()) {
        return inspectionFailed(
            QStringLiteral("invalid size for page 0: %1").arg(reader.errorString()));
    }

    for (int pageIndex = 1; pageIndex < pageCount; ++pageIndex) {
        if (!reader.jumpToImage(pageIndex)) {
            return inspectionFailed(
                QStringLiteral("could not select page %1: %2")
                    .arg(pageIndex)
                    .arg(reader.errorString()));
        }
        const QSize pageSize = reader.size();
        if (!pageSize.isValid()) {
            return inspectionFailed(
                QStringLiteral("invalid size for page %1: %2")
                    .arg(pageIndex)
                    .arg(reader.errorString()));
        }
        if (pageSize != firstPageSize) {
            return true;
        }
    }
    return false;
}

template<typename ComponentType>
std::array<volume_io::LoadedVolume<itk::Image<ComponentType, 3>>, 3>
loadRgbChannels(const QString &fileName, bool firstTiffPageOnly, const volume_io::ImageLoadOptions &options) {
    using VectorImageType = itk::VectorImage<ComponentType, 3>;
    using ScalarImageType = itk::Image<ComponentType, 3>;
    using ChannelFilterType =
        itk::VectorIndexSelectionCastImageFilter<VectorImageType, ScalarImageType>;

    SP_LOG_INFO("io", QStringLiteral("Reading RGB image %1").arg(fileName));

    auto customIO = firstTiffPageOnly ? FirstPageTIFFImageIO::New() : nullptr;
    auto loaded = volume_io::readWithITK<VectorImageType>(fileName, options, customIO);
    auto vectorImage = loaded.image;
    if (firstTiffPageOnly) {
        SP_LOG_WARNING(
            "io",
            QStringLiteral(
                "TIFF contains differently sized pages; loaded only the first page of %1")
                .arg(fileName));
    }

    if (vectorImage->GetNumberOfComponentsPerPixel() != 3) {
        throw std::logic_error(
            "Expected an RGB image with exactly three components: " + fileName.toStdString());
    }

    std::array<volume_io::LoadedVolume<ScalarImageType>, 3> channels;
    for (unsigned int channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
        typename ChannelFilterType::Pointer channelFilter = ChannelFilterType::New();
        channelFilter->SetInput(vectorImage);
        channelFilter->SetIndex(channelIndex);
        channelFilter->Update();

        channels[channelIndex].image = channelFilter->GetOutput();
        channels[channelIndex].context = std::make_shared<volume_io::VolumeContext>(*loaded.context);
        channels[channelIndex].image->SetMetaDataDictionary(vectorImage->GetMetaDataDictionary());
        channels[channelIndex].image->DisconnectPipeline();
    }
    return channels;
}

const itk::ImageBase<3> *asImageBase(const itk::DataObject::Pointer &image) {
    return dynamic_cast<const itk::ImageBase<3> *>(image.GetPointer());
}

bool hasSameGeometry(const itk::ImageBase<3> &left, const itk::ImageBase<3> &right) {
    return left.GetLargestPossibleRegion() == right.GetLargestPossibleRegion() &&
           left.GetSpacing() == right.GetSpacing() &&
           left.GetOrigin() == right.GetOrigin() &&
           left.GetDirection() == right.GetDirection();
}

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

dataType::SegmentsStorage::Pointer duplicateSegmentsImage(
    const dataType::SegmentsStorage::Pointer &sourceImage) {
    auto copy = segment_puzzler::StreamVByteImageStorage<dataType::SegmentIdType>::create(*sourceImage->imageInformation());
    const auto full = sourceImage->GetLargestPossibleRegion();
    std::vector<dataType::SegmentIdType> pixels;
    for (std::size_t z = 0; z < full.GetSize()[2]; z += 16) {
        auto region = full;
        region.SetIndex(2, full.GetIndex()[2] + z);
        region.SetSize(2, std::min<std::size_t>(16, full.GetSize()[2] - z));
        pixels.resize(region.GetNumberOfPixels());
        sourceImage->readRegion(region, pixels.data(), pixels.size());
        copy->writeRegion(region, pixels.data(), pixels.size());
    }
    return copy;
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
    convertedImage->SetMetaDataDictionary(sourceImage->GetMetaDataDictionary());
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
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
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
        .arg(std::round(signal->getNormLower()), 0, 'f', 0)
        .arg(std::round(signal->getNormUpper()), 0, 'f', 0);
}

QString opacityChipText(itkSignalBase *signal) {
    if (signal == nullptr) {
        return QString();
    }
    const int opacityPercent = static_cast<int>(std::lround((static_cast<double>(signal->getAlpha()) / 255.0) * 100.0));
    return QString("%1%").arg(opacityPercent);
}

QString blendModeChipText(itkSignalBase *signal) {
    if (signal == nullptr || !signal->supportsNormControl()) {
        return QString();
    }
    return signal->getBlendMode() == itkSignalBase::BlendMode::Additive
               ? QStringLiteral("Add")
               : QStringLiteral("Over");
}

int alphaToPercent(unsigned int alpha) {
    return static_cast<int>(std::lround((static_cast<double>(alpha) / 255.0) * 100.0));
}

unsigned char percentToAlpha(int percent) {
    const int clampedPercent = std::clamp(percent, 0, 100);
    return static_cast<unsigned char>(std::lround((static_cast<double>(clampedPercent) / 100.0) * 255.0));
}

void applyInitialDisplay(itkSignalBase *signal, const LayerDisplayOptions &display) {
    if (display.visible) signal->setIsActive(*display.visible);
    if (display.opacityPercent) signal->setAlpha(percentToAlpha(*display.opacityPercent));
    if (display.view) signal->setLabelRenderMode(*display.view);
    if (display.outlineWidth) signal->setLabelBoundaryWidth(*display.outlineWidth);
}

void configureContinuousLayer(itkSignalBase *signal, const LayerDisplayOptions &display) {
    using namespace segment_puzzler::image_normalization;
    configureContinuousDisplay(signal);
    if (display.range)
        signal->setNorm(display.range->lower, display.range->upper);
    else
        applyToSignal(signal);
    if (display.blend) signal->setBlendMode(*display.blend);
    if (display.color) signal->setMainColor(*display.color);
}

QString layerToolTipText(itkSignalBase *signal) {
    if (signal == nullptr) {
        return QString();
    }

    QString toolTip = QString("Type: %1").arg(signal->getDisplayDataTypeName());
    if (signal->volumeContext && signal->volumeContext->crop)
        toolTip += "\n\n" + volume_io::contextDescription(*signal->volumeContext);
    if (signal->usesEdgeStatusColors()) {
        toolTip += "\nEdge colors are fixed: white, red, green.";
    } else if (signal->usesCategoricalColors()) {
        toolTip += "\nClick the heart to switch between filled segments and boundaries.";
    } else {
        toolTip += "\nClick the heart to change the display color.";
    }
    if (signal->supportsNormControl()) {
        toolTip += "\nClick contrast to adjust the display range.";
        toolTip += "\nClick Add/Over to change the blend mode.";
    }
    toolTip += "\nClick opacity to adjust the overlay strength.";
    if (signal->usesCategoricalColors() &&
        signal->getLabelRenderMode() == itkSignalBase::LabelRenderMode::Boundaries) {
        toolTip += "\nClick line width to adjust the segment outlines.";
    }
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

void SignalControl::prepareWorkingSegmentsGraph(const GraphSegmentImageType::Pointer &workingSegmentsImage, std::optional<GraphSegmentType> background) {
    prepareWorkingSegmentsGraph(segment_puzzler::storeLabels(*workingSegmentsImage), background);
}

void SignalControl::prepareWorkingSegmentsGraph(const dataType::SegmentsStorage::Pointer &workingSegmentsImage, std::optional<GraphSegmentType> background) {
    graphBase->ignoredSegmentLabels.clear();
    graphBase->edgeStatus.clear();
    graphBase->colorLookUpEdgesStatus.clear();
    if (!background) graphBase->pGraph->setBackgroundIdStrategy("backgroundIsLowestId");
    graphBase->pWorkingSegmentsImage = workingSegmentsImage;
    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graphBase->pGraph->constructFromVolume(workingSegmentsImage, background);
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

std::optional<voxel_geometry::VoxelSpacing> SignalControl::voxelSpacing() const {
    for (const auto *signal : allSignalList) {
        if (signal == nullptr) {
            continue;
        }
        const auto image = signal->getImageBase();
        if (image.IsNotNull()) {
            return voxel_geometry::fromItkSpacing(image->GetSpacing());
        }
    }

    const itk::ImageBase<3> *fallbackImages[] = {
        graphBase->pWorkingSegmentsImage ? graphBase->pWorkingSegmentsImage->imageInformation() : nullptr,
        graphBase->pSelectedSegmentation ? graphBase->pSelectedSegmentation->imageInformation() : nullptr,
        graphBase->pSelectedRefinement ? graphBase->pSelectedRefinement->imageInformation() : nullptr,
        graphBase->pSelectedBoundary.GetPointer(),
        graphBase->pEdgesInitialSegmentsImage ? graphBase->pEdgesInitialSegmentsImage->imageInformation() : nullptr,
        graphBase->pGroundTruth.GetPointer()
    };
    for (const auto *image : fallbackImages) {
        if (image != nullptr) {
            return voxel_geometry::fromItkSpacing(image->GetSpacing());
        }
    }
    return std::nullopt;
}

void SignalControl::setVoxelSpacing(const voxel_geometry::VoxelSpacing &spacing) {
    if (!voxel_geometry::isValid(spacing)) {
        throw std::invalid_argument("Voxel spacing values must be finite and greater than zero");
    }

    std::unordered_set<itk::ImageBase<3> *> seenImages;
    std::vector<itk::ImageBase<3> *> images;
    const auto addImage = [&seenImages, &images](itk::ImageBase<3> *image) {
        if (image != nullptr && seenImages.insert(image).second) {
            images.push_back(image);
        }
    };

    for (const auto *signal : allSignalList) {
        if (signal == nullptr) {
            continue;
        }
        const auto image = signal->getImageBase();
        addImage(image.GetPointer());
    }
    addImage(graphBase->pWorkingSegmentsImage ? graphBase->pWorkingSegmentsImage->imageInformation() : nullptr);
    addImage(graphBase->pSelectedSegmentation ? graphBase->pSelectedSegmentation->imageInformation() : nullptr);
    addImage(graphBase->pSelectedRefinement ? graphBase->pSelectedRefinement->imageInformation() : nullptr);
    addImage(graphBase->pSelectedBoundary.GetPointer());
    addImage(graphBase->pEdgesInitialSegmentsImage ? graphBase->pEdgesInitialSegmentsImage->imageInformation() : nullptr);
    addImage(graphBase->pGroundTruth.GetPointer());

    for (auto *image : images) {
        auto itkSpacing = image->GetSpacing();
        voxel_geometry::assignToItkSpacing(spacing, itkSpacing);
        image->SetSpacing(itkSpacing);
    }

    SP_LOG_INFO(
        "io",
        QStringLiteral("Updated voxel spacing to [%1,%2,%3] on %4 loaded image object(s)")
            .arg(spacing.x, 0, 'g', 12)
            .arg(spacing.y, 0, 'g', 12)
            .arg(spacing.z, 0, 'g', 12)
            .arg(images.size()));

    if (orthoViewer != nullptr) {
        orthoViewer->setVoxelSpacing(spacing);
    }
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
    if (paintModeActive) {
        orthoViewer->refreshPaintSelectionColor();
    }
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
    for (auto &entry : layerGroupVisibility) {
        LayerGroupVisibility &group = entry.second;
        if (group.button != nullptr) {
            group.button->setEnabled(enabled);
        }
    }

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
    mergeSmallSegmentsAction->setEnabled(
        enabled && hasWorkingSegments() && hasSelectedSegmentation()
        && graphBase != nullptr && graphBase->pGraph != nullptr);
    transferWithVolumeAction->setEnabled(enabled && hasSelectedSegmentation());
    transferWithRefinementAction->setEnabled(enabled && hasSelectedSegmentation() && hasSelectedRefinement());
    transferAllAction->setEnabled(enabled && hasSelectedSegmentation());
    const bool canTransferNonOverlapping = enabled && hasWorkingSegments() && hasSelectedSegmentation();
    transferNonOverlappingAction->setEnabled(canTransferNonOverlapping);
    transferNonOverlappingWithVolumeAction->setEnabled(canTransferNonOverlapping);
    scheduleSidebarLayoutUpdate();
}

int SignalControl::preferredSidebarWidthHint() const {
    int preferredWidth = QWidget::minimumSizeHint().width();
    for (QTreeWidget *tree : {signalTreeWidget, probabilityTreeWidget, segmentationTreeWidget, refinementTreeWidget}) {
        preferredWidth = std::max(preferredWidth, layerTreePreferredWidth(tree));
    }
    return preferredWidth;
}

void SignalControl::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    scheduleSidebarLayoutUpdate();
}

void SignalControl::fitSectionHeights() {
    if (sectionHeightsSetByUser || !isVisible()) {
        return;
    }

    auto sizes = sectionSplitter->sizes();
    for (auto *tree : {signalTreeWidget, probabilityTreeWidget, refinementTreeWidget, segmentationTreeWidget}) {
        QWidget *section = tree->parentWidget();
        int preferredHeight = section->minimumSizeHint().height();
        if (auto *first = tree->topLevelItem(0)) {
            // Layer cards have uniform height, even before Qt lays out the first row.
            QWidget *card = tree->itemWidget(first, 0);
            const int rowHeight = card ? card->sizeHint().height() : tree->visualItemRect(first).height();
            const int listHeight = tree->topLevelItemCount() * rowHeight + 2 * tree->frameWidth();
            preferredHeight += std::max(0, listHeight - tree->minimumHeight());
        }
        sizes[sectionSplitter->indexOf(section)] = preferredHeight;
    }
    sectionSplitter->setSizes(sizes);
}

void SignalControl::scheduleSidebarLayoutUpdate() {
    if (sidebarLayoutUpdatePending) {
        return;
    }

    sidebarLayoutUpdatePending = true;
    QPointer<SignalControl> guardedThis(this);
    QTimer::singleShot(0, this, [guardedThis]() {
        if (guardedThis == nullptr) {
            return;
        }

        guardedThis->sidebarLayoutUpdatePending = false;
        guardedThis->fitSectionHeights();
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
        label->setToolTip(QString());
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
        label->setToolTip(signal->name);
    } else {
        label->setText("Selected: none");
        label->setToolTip(QString());
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
    rememberNewLayerWhileGroupHidden(tree, signalForItem(item));
    item->setFirstColumnSpanned(true);
    tree->setItemWidget(item, 0, layerWidget);
    SignalLayerWidget::requestHostTreeLayoutSync(tree);
    scheduleSidebarLayoutUpdate();

    connect(layerWidget, &SignalLayerWidget::sizeHintChanged, this, [this, tree]() {
        SignalLayerWidget::requestHostTreeLayoutSync(tree);
        scheduleSidebarLayoutUpdate();
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

    connect(layerWidget, &SignalLayerWidget::blendModeRequested, this, [this, tree, item]() {
        itkSignalBase *signal = signalForItem(item);
        if (signal == nullptr || !signal->supportsNormControl()) {
            return;
        }
        const auto nextMode =
            signal->getBlendMode() == itkSignalBase::BlendMode::Additive
                ? itkSignalBase::BlendMode::SourceOver
                : itkSignalBase::BlendMode::Additive;
        signal->setBlendMode(nextMode);
        refreshLayerWidget(tree, item);
        refreshViewers();
    });

    connect(layerWidget, &SignalLayerWidget::opacityRequested, this, [this, item](QWidget *anchor) {
        openOpacityPopup(item, anchor);
    });

    refreshLayerWidget(tree, item);
    scheduleSidebarLayoutUpdate();
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
    const bool active = layerVisibilityForUi(tree, signal);

    item->setText(0, QString());
    item->setText(1, QString());

    SignalLayerWidget::Presentation presentation;
    presentation.layerName = name + (signal->volumeContext && signal->volumeContext->crop ? QStringLiteral(" [Crop]") : QString());
    presentation.layerColor = QColor::fromRgba(signal->getColor());
    presentation.usesCategoricalPalette = signal->usesCategoricalColors();
    presentation.usesEdgeStatusColors = signal->usesEdgeStatusColors();
    presentation.showsLabelBoundaries =
        signal->getLabelRenderMode() == itkSignalBase::LabelRenderMode::Boundaries;
    presentation.layerVisible = active;
    presentation.selected = item == signal_tree::topLevelSignalItem(tree->currentItem());
    presentation.contrastText = contrastChipText(signal);
    presentation.blendModeText = blendModeChipText(signal);
    presentation.opacityText = opacityChipText(signal);
    presentation.boundaryWidthText = QStringLiteral("%1 px").arg(signal->getLabelBoundaryWidth());
    presentation.toolTip = layerToolTipText(signal);
    presentation.contrastAvailable = signal->supportsNormControl();
    presentation.blendModeAvailable = signal->supportsNormControl();
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
    auto *lowerEdit = new QLineEdit(container);
    auto *upperEdit = new QLineEdit(container);
    lowerEdit->setObjectName("contrastLower");
    upperEdit->setObjectName("contrastUpper");
    auto *autoButton = new QPushButton("Auto", container);
    auto *resetButton = new QPushButton("Reset", container);

    const double minimum = signal->getMinimumValueAsDouble();
    const double maximum = signal->getMaximumValueAsDouble();
    for (auto *slider : {lowerSlider, upperSlider}) {
        if (std::isfinite(minimum) && std::isfinite(maximum) && minimum < maximum) {
            slider->setRange(clampPopupValue(minimum, INT_MIN, INT_MAX),
                             clampPopupValue(maximum, INT_MIN, INT_MAX));
        } else {
            slider->setRange(0, 0);
            slider->setEnabled(false);
        }
    }

    auto *lowerRow = new QWidget(container);
    auto *lowerLayout = new QHBoxLayout(lowerRow);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    lowerLayout->setSpacing(6);
    lowerLayout->addWidget(new QLabel("Min", lowerRow));
    lowerLayout->addWidget(lowerSlider, 1);
    lowerLayout->addWidget(lowerEdit);

    auto *upperRow = new QWidget(container);
    auto *upperLayout = new QHBoxLayout(upperRow);
    upperLayout->setContentsMargins(0, 0, 0, 0);
    upperLayout->setSpacing(6);
    upperLayout->addWidget(new QLabel("Max", upperRow));
    upperLayout->addWidget(upperSlider, 1);
    upperLayout->addWidget(upperEdit);

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

    auto *update = new SliderUpdateTimer(menu, [this, tree, item, signal] {
        refreshLayerWidget(tree, item);
        orthoViewer->refreshViewers(signal);
    });
    connect(lowerSlider, &QSlider::sliderReleased, update, [update] { update->flush(); });
    connect(upperSlider, &QSlider::sliderReleased, update, [update] { update->flush(); });
    connect(menu, &QMenu::aboutToHide, update, [update] { update->flush(); });

    auto numberLocale = locale();
    numberLocale.setNumberOptions(numberLocale.numberOptions() | QLocale::OmitGroupSeparator | QLocale::RejectGroupSeparator);
    auto syncControls = [lowerSlider, upperSlider, lowerEdit, upperEdit, numberLocale](double lower, double upper) {
        const QSignalBlocker lowerBlocker(lowerSlider);
        const QSignalBlocker upperBlocker(upperSlider);
        lowerSlider->setValue(clampPopupValue(lower, lowerSlider->minimum(), lowerSlider->maximum()));
        upperSlider->setValue(clampPopupValue(upper, upperSlider->minimum(), upperSlider->maximum()));
        lowerEdit->setText(numberLocale.toString(lower, 'g', QLocale::FloatingPointShortest));
        upperEdit->setText(numberLocale.toString(upper, 'g', QLocale::FloatingPointShortest));
    };
    syncControls(signal->getNormLower(), signal->getNormUpper());
    auto applyNorm = [=](double lower, double upper) {
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper) return;
        signal->setNorm(lower, upper);
        syncControls(lower, upper);
        update->request(lowerSlider->isSliderDown() || upperSlider->isSliderDown());
    };
    auto setLower = [signal, applyNorm](double value) {
        const double upper = signal->getNormUpper();
        if (std::isfinite(upper)) applyNorm(std::min(value, upper), upper);
        else applyNorm(value, value);
    };
    auto setUpper = [signal, applyNorm](double value) {
        const double lower = signal->getNormLower();
        if (std::isfinite(lower)) applyNorm(lower, std::max(lower, value));
        else applyNorm(value, value);
    };
    connect(lowerSlider, &QSlider::valueChanged, this, setLower);
    connect(upperSlider, &QSlider::valueChanged, this, setUpper);
    auto bindEdit = [&](QLineEdit *edit, auto setBound) {
        connect(edit, &QLineEdit::editingFinished, this, [=]() {
            bool ok = false;
            const double value = numberLocale.toDouble(edit->text(), &ok);
            if (ok && std::isfinite(value)) setBound(value);
            else syncControls(signal->getNormLower(), signal->getNormUpper());
        });
    };
    bindEdit(lowerEdit, setLower);
    bindEdit(upperEdit, setUpper);
    connect(autoButton, &QPushButton::clicked, this, [=]() {
        double lower, upper;
        if (segment_puzzler::image_normalization::computeRange(
                signal, segment_puzzler::image_normalization::loadSettings(), lower, upper))
            applyNorm(lower, upper);
        else
            applyNorm(minimum, maximum);
    });
    connect(resetButton, &QPushButton::clicked, this, [=]() { applyNorm(minimum, maximum); });

    action->setDefaultWidget(container);
    menu->addAction(action);
    menu->setFixedWidth(640);
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

    addLabelBoundaryWidthControl(layout, signal, [this, tree, item]() {
        refreshLayerWidget(tree, item);
        refreshViewers();
    });

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
        graphBase->pSelectedBoundarySignal = nullptr;
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    graphBase->pSelectedBoundarySignal = allSignalList[signalIndex];
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
    graphBase->pSelectedRefinement = dynamic_cast<itkSignal<GraphSegmentType> *>(allSignalList[signalIndex])->imageStorage;
    graphBase->pSelectedRefinementSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(
        allSignalList[signalIndex]);
}

void SignalControl::selectSegmentationItem(QTreeWidgetItem *item) {
    const auto *previousImage = graphBase->pSelectedSegmentation.GetPointer();
    const auto *previousSignal = graphBase->pSelectedSegmentationSignal;
    if (signal_tree::topLevelSignalItem(item) == nullptr) {
        graphBase->pSelectedSegmentation = nullptr;
        graphBase->pSelectedSegmentationSignal = nullptr;
        graphBase->selectedSegmentationContext = std::make_shared<volume_io::VolumeContext>();
        if (orthoViewer != nullptr) {
            orthoViewer->resetPaintSelection();
        }
        if (previousImage != graphBase->pSelectedSegmentation.GetPointer()
            || previousSignal != graphBase->pSelectedSegmentationSignal) {
            emit selectedSegmentationChanged();
        }
        return;
    }

    const size_t signalIndex = signalIndexForItem(item);
    graphBase->pSelectedSegmentation = dynamic_cast<itkSignal<GraphSegmentType> *>(allSignalList[signalIndex])->imageStorage;
    graphBase->pSelectedSegmentationSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(
        allSignalList[signalIndex]);
    graphBase->selectedSegmentationContext = graphBase->pSelectedSegmentationSignal->volumeContext;
    if (orthoViewer != nullptr) {
        orthoViewer->resetPaintSelection();
    }
    if (previousImage != graphBase->pSelectedSegmentation.GetPointer()
        || previousSignal != graphBase->pSelectedSegmentationSignal) {
        emit selectedSegmentationChanged();
    }
}

void SignalControl::setGuiBusy(bool busy) {
    guiBusy = busy;
    refreshUiState();
}

std::size_t SignalControl::deleteSelectedSegmentationLabels(
    dataType::SegmentsStorage::Pointer expectedSegmentation,
    const std::unordered_set<dataType::SegmentIdType> &labels)
{
    if (labels.empty() || expectedSegmentation == nullptr
        || taskRunner == nullptr || taskRunner->isBusy()
        || graphBase == nullptr || graphBase->pGraph == nullptr
        || graphBase->pSelectedSegmentation != expectedSegmentation) {
        SP_LOG_WARNING(
            "segmentation",
            QStringLiteral("operation=delete_segmentation_labels status=rejected labels=%1 "
                           "task_busy=%2 selection_matches=%3")
                .arg(labels.size())
                .arg(taskRunner != nullptr && taskRunner->isBusy())
                .arg(graphBase != nullptr
                     && graphBase->pSelectedSegmentation == expectedSegmentation));
        return 0;
    }

    QElapsedTimer timer;
    timer.start();
    const std::size_t deletedVoxelCount =
        graphBase->pGraph->deleteSegmentationLabels(labels);
    if (deletedVoxelCount == 0) {
        return 0;
    }

    std::vector<dataType::SegmentIdType> deletedLabels(labels.begin(), labels.end());
    std::sort(deletedLabels.begin(), deletedLabels.end());
    refreshViewers();
    emit selectedSegmentationLabelsDeleted(
        reinterpret_cast<quintptr>(expectedSegmentation.GetPointer()),
        reinterpret_cast<quintptr>(graphBase->pSelectedSegmentationSignal),
        deletedLabels);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("operation=delete_segmentation_labels status=deleted labels=%1 "
                       "deleted_voxels=%2 elapsed_ms=%3")
            .arg(deletedLabels.size())
            .arg(deletedVoxelCount)
            .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000.0, 0, 'f', 3));
    return deletedVoxelCount;
}

void SignalControl::refreshViewers() {
    orthoViewer->refreshViewers();
}

void SignalControl::refreshAfterVisibilityChange() {
    for (auto *viewer : orthoViewer->viewerList) {
        viewer->setSliceIndex(viewer->getSliceIndex());
        viewer->recalculateQImages();
    }
}

QString SignalControl::resolvedDisplayName(const QString &fileName,
                                           const QString &displayedName,
                                           const std::vector<QString> &suffixes) const {
    const QString requestedName = !displayedName.isEmpty() ? displayedName : export_path_utils::sourceStem(fileName);
    return signal_name_utils::makeUniqueSignalName(allSignalList, requestedName, suffixes);
}

void SignalControl::rememberLoadedSourceFile(const QString &fileName) {
    if (fileName.isEmpty()) {
        return;
    }

    const QFileInfo fileInfo(fileName);
    graphBase->lastLoadedSourcePath = fileInfo.absoluteFilePath();
}

QString SignalControl::suggestedSegmentationExportPath(const QString &storedDefaultSavePath) const {
    const QString selectedSegmentationName =
        graphBase->pSelectedSegmentationSignal != nullptr
            ? graphBase->pSelectedSegmentationSignal->name
            : QString();
    const QString sourceBasedPath = export_path_utils::suggestedExportPath(
        storedDefaultSavePath,
        {graphBase->pSelectedSegmentationSignal != nullptr
             ? graphBase->pSelectedSegmentationSignal->sourceFilePath
             : QString(),
         graphBase->pWorkingSegments != nullptr
             ? graphBase->pWorkingSegments->sourceFilePath
             : QString(),
         graphBase->lastLoadedSourcePath},
        selectedSegmentationName,
        QStringLiteral("Segmentation"),
        QStringLiteral(".nrrd"));

    if (graphBase->lastExportPathThisSession.isEmpty()) {
        return sourceBasedPath;
    }
    return QDir(QFileInfo(graphBase->lastExportPathThisSession).absolutePath())
        .filePath(QFileInfo(sourceBasedPath).fileName());
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

SignalControl::LoadedFileSpacingDecision SignalControl::askForLoadedFileSpacing(
    const QString &fileName) {
    const auto currentSpacing = voxelSpacing();
    if (!currentSpacing.has_value()) {
        return {};
    }

    ImageFileInfo imageInfo;
    try {
        imageInfo = getImageFileInfo(fileName);
    } catch (const std::exception &error) {
        SP_LOG_DEBUG(
            "io",
            QStringLiteral("Could not inspect voxel spacing before loading %1: %2")
                .arg(fileName, QString::fromUtf8(error.what())));
        return {};
    } catch (...) {
        SP_LOG_DEBUG(
            "io",
            QStringLiteral("Could not inspect voxel spacing before loading %1")
                .arg(fileName));
        return {};
    }

    const auto loadedSpacing = voxel_geometry::fromItkSpacing(imageInfo.spacing);
    if (voxel_geometry::nearlyEqual(*currentSpacing, loadedSpacing)) {
        return {};
    }

    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(tr("Voxel Spacing Differs"));
    dialog.setText(
        tr("The loaded file \"%1\" has different voxel spacing.\n\n"
           "Current dataset: %2\n"
           "Loaded file: %3")
            .arg(QFileInfo(fileName).fileName(),
                 formatVoxelSpacing(*currentSpacing),
                 formatVoxelSpacing(loadedSpacing)));
    dialog.setInformativeText(
        tr("Layers are displayed aligned by voxel index using the current dataset proportions. "
           "Choose which spacing should be stored for the newly loaded layer(s)."));

    auto *keepFileSpacingButton = dialog.addButton(
        tr("Keep File"), QMessageBox::AcceptRole);
    auto *useDatasetSpacingButton = dialog.addButton(
        tr("Use Dataset"), QMessageBox::ActionRole);
    auto *cancelButton = dialog.addButton(QMessageBox::Cancel);
    dialog.setDefaultButton(keepFileSpacingButton);
    dialog.setEscapeButton(cancelButton);
    dialog.exec();

    if (dialog.clickedButton() == useDatasetSpacingButton) {
        SP_LOG_INFO(
            "io",
            QStringLiteral("Using current dataset spacing [%1] while loading %2")
                .arg(formatVoxelSpacing(*currentSpacing), fileName));
        return {true, currentSpacing};
    }
    if (dialog.clickedButton() == keepFileSpacingButton) {
        SP_LOG_WARNING(
            "io",
            QStringLiteral("Keeping differing file spacing [%1] while loading %2; current dataset spacing is [%3]")
                .arg(formatVoxelSpacing(loadedSpacing),
                     fileName,
                     formatVoxelSpacing(*currentSpacing)));
        return {};
    }

    SP_LOG_INFO("io", QStringLiteral("Canceled loading %1 after voxel spacing mismatch").arg(fileName));
    return {false, std::nullopt};
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

void SignalControl::invokeLoadCallbackOnce(
    const std::shared_ptr<LoadCallback> &then,
    LoadResult result)
{
    if (then == nullptr || !*then) {
        return;
    }

    LoadCallback callback = std::move(*then);
    *then = {};
    invokeLoadCallbackLater(std::move(callback), std::move(result));
}

SignalControl::LoadedImageData SignalControl::loadImageData(
    QString fileName, const volume_io::ImageLoadOptions &options) {
    LoadedImageData loadedImage;
    if (fileName.isEmpty()) {
        return loadedImage;
    }

    const auto imageInfo = getImageFileInfo(fileName);
    SP_LOG_INFO("io", QStringLiteral("Reading image %1").arg(fileName));
    SP_LOG_INFO(
        "io",
        QStringLiteral("Detected image dimension=%1 pixelType=%2 components=%3 componentType=%4 for %5")
            .arg(imageInfo.dimension)
            .arg(QString::fromStdString(itk::ImageIOBase::GetPixelTypeAsString(imageInfo.pixelType)))
            .arg(imageInfo.componentCount)
            .arg(QString::fromStdString(itk::ImageIOBase::GetComponentTypeAsString(imageInfo.componentType)))
            .arg(fileName));

    if (imageInfo.dimension != 2 && imageInfo.dimension != 3) {
        throw std::logic_error("Only 2D and 3D images are supported.");
    }

    const bool isScalar =
        imageInfo.pixelType == itk::IOPixelEnum::SCALAR && imageInfo.componentCount == 1;
    const bool isRgb =
        imageInfo.pixelType == itk::IOPixelEnum::RGB && imageInfo.componentCount == 3;

    if (!isScalar && !isRgb) {
        throw std::logic_error(
            QStringLiteral(
                "Unsupported image pixel type %1 with %2 components: %3. "
                "Only scalar images and three-component RGB images are supported.")
                .arg(QString::fromStdString(itk::ImageIOBase::GetPixelTypeAsString(imageInfo.pixelType)))
                .arg(imageInfo.componentCount)
                .arg(fileName)
                .toStdString());
    }

    const bool hasSupportedRgbComponentType =
        imageInfo.componentType == itk::ImageIOBase::IOComponentType::UCHAR ||
        imageInfo.componentType == itk::ImageIOBase::IOComponentType::USHORT;
    if (isRgb && !hasSupportedRgbComponentType) {
        throw std::logic_error(
            QStringLiteral(
                "Unsupported RGB component type %1: %2. "
                "Only unsigned 8-bit and unsigned 16-bit RGB images are supported.")
                .arg(QString::fromStdString(
                    itk::ImageIOBase::GetComponentTypeAsString(imageInfo.componentType)))
                .arg(fileName)
                .toStdString());
    }

    const QString fileSuffix = QFileInfo(fileName).suffix().toLower();
    const bool isMultipageTiff =
        imageInfo.dimension == 3 &&
        (fileSuffix == QStringLiteral("tif") || fileSuffix == QStringLiteral("tiff"));
    const bool firstTiffPageOnly =
        isRgb && isMultipageTiff && hasConfirmedDifferentlySizedTiffPages(fileName);

    loadedImage.dataType = imageInfo.componentType;

    auto loadTypedImage = [&](auto componentValue) {
        using ComponentType = decltype(componentValue);
        if constexpr (std::is_same_v<ComponentType, unsigned char> ||
                      std::is_same_v<ComponentType, unsigned short>) {
            if (isRgb) {
                const auto channels =
                    loadRgbChannels<ComponentType>(fileName, firstTiffPageOnly, options);
                loadedImage.layers = {
                    {channels[0].image.GetPointer(), QStringLiteral(" [R]"), QColor(255, 0, 0), channels[0].context},
                    {channels[1].image.GetPointer(), QStringLiteral(" [G]"), QColor(0, 255, 0), channels[1].context},
                    {channels[2].image.GetPointer(), QStringLiteral(" [B]"), QColor(0, 0, 255), channels[2].context}
                };
                return;
            }
        }
        auto loaded = loadVolume<ComponentType>(fileName, options);
        loadedImage.layers.push_back({loaded.image.GetPointer(), {}, std::nullopt, loaded.context});
    };

    switch (loadedImage.dataType) {
        case itk::ImageIOBase::IOComponentType::UCHAR:
            loadTypedImage(static_cast<unsigned char>(0));
            break;
        case itk::ImageIOBase::IOComponentType::CHAR:
            loadTypedImage(static_cast<char>(0));
            break;
        case itk::ImageIOBase::IOComponentType::USHORT:
            loadTypedImage(static_cast<unsigned short>(0));
            break;
        case itk::ImageIOBase::IOComponentType::SHORT:
            loadTypedImage(static_cast<short>(0));
            break;
        case itk::ImageIOBase::IOComponentType::UINT:
            loadTypedImage(static_cast<unsigned int>(0));
            break;
        case itk::ImageIOBase::IOComponentType::INT:
            loadTypedImage(static_cast<int>(0));
            break;
        case itk::ImageIOBase::IOComponentType::ULONG:
            loadTypedImage(static_cast<unsigned long>(0));
            break;
        case itk::ImageIOBase::IOComponentType::LONG:
            loadTypedImage(static_cast<long>(0));
            break;
        case itk::ImageIOBase::IOComponentType::ULONGLONG:
            loadTypedImage(static_cast<unsigned long long>(0));
            break;
        case itk::ImageIOBase::IOComponentType::LONGLONG:
            loadTypedImage(static_cast<long long>(0));
            break;
        case itk::ImageIOBase::IOComponentType::FLOAT:
            loadTypedImage(0.0F);
            break;
        case itk::ImageIOBase::IOComponentType::DOUBLE:
            loadTypedImage(0.0);
            break;
        case itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE:
        default:
            throw std::logic_error("SignalControl::loadImageData unknown component type encountered.");
    }

    if (loadedImage.layers.empty()) {
        throw std::logic_error("SignalControl::loadImageData produced no image layers.");
    }

    const itk::ImageBase<3> *referenceImage = asImageBase(loadedImage.layers.front().image);
    if (referenceImage == nullptr) {
        throw std::logic_error("SignalControl::loadImageData produced an invalid image layer.");
    }
    for (const LoadedImageLayer &layer : loadedImage.layers) {
        const itk::ImageBase<3> *image = asImageBase(layer.image);
        if (image == nullptr || !hasSameGeometry(*referenceImage, *image)) {
            throw std::logic_error("Loaded image channels do not share the same geometry.");
        }
    }

    return loadedImage;
}

bool SignalControl::insertLoadedImage(const LoadedImageLayer &loadedLayer,
                                      itk::ImageIOBase::IOComponentType dataType,
                                      size_t &signalIndexGlobalOut,
                                      bool forceShapeOfSegments) {
    switch (dataType) {
        case itk::ImageIOBase::IOComponentType::UCHAR:
            return insertTypedImage<unsigned char>(
                dynamic_cast<itk::Image<unsigned char, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::CHAR:
            return insertTypedImage<char>(
                dynamic_cast<itk::Image<char, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::USHORT:
            return insertTypedImage<unsigned short>(
                dynamic_cast<itk::Image<unsigned short, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::SHORT:
            return insertTypedImage<short>(
                dynamic_cast<itk::Image<short, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::UINT:
            return insertTypedImage<unsigned int>(
                dynamic_cast<itk::Image<unsigned int, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::INT:
            return insertTypedImage<int>(
                dynamic_cast<itk::Image<int, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::ULONG:
            return insertTypedImage<unsigned long>(
                dynamic_cast<itk::Image<unsigned long, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::LONG:
            return insertTypedImage<long>(
                dynamic_cast<itk::Image<long, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::ULONGLONG:
            return insertTypedImage<unsigned long long>(
                dynamic_cast<itk::Image<unsigned long long, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::LONGLONG:
            return insertTypedImage<long long>(
                dynamic_cast<itk::Image<long long, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::FLOAT:
            return insertTypedImage<float>(
                dynamic_cast<itk::Image<float, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::DOUBLE:
            return insertTypedImage<double>(
                dynamic_cast<itk::Image<double, 3> *>(loadedLayer.image.GetPointer()),
                signalIndexGlobalOut,
                forceShapeOfSegments, loadedLayer.context);
        case itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE:
        default:
            throw std::logic_error("SignalControl::insertLoadedImage unknown component type encountered.");
    }
}

void SignalControl::registerImageSignal(
    size_t signalIndexGlobal,
    const QString &name,
    std::optional<QColor> color,
    LayerDisplayOptions display) {
    const bool centerFirstStandaloneLayer = !hasWorkingSegments() && allSignalList.size() == 1;
    itkSignalBase *signal = allSignalList[signalIndexGlobal];
    signal->setLayerRole(itkSignalBase::LayerRole::SourceImage);
    configureContinuousLayer(signal, display);
    if (color) {
        signal->setMainColor(*color);
    } else if (!display.color) {
        const auto &colors = defaultAdditiveImageColors();
        signal->setMainColor(colors[nextDefaultImageColorIndex % colors.size()]);
        ++nextDefaultImageColorIndex;
    }
    signal->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    signal->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    applyInitialDisplay(signal, display);
    attachLayerWidgetToLastItem(signalTreeWidget);
    orthoViewer->addSignal(signal);
    if (centerFirstStandaloneLayer) {
        orthoViewer->setViewToMiddleOfStack();
    }
    refreshUiState();
}

void SignalControl::registerSegmentationSignal(size_t signalIndexGlobal, const QString &name,
                                               LayerDisplayOptions display) {
    auto *signal = allSignalList[signalIndexGlobal];
    if (!signal->volumeContext) {
        signal->volumeContext = std::make_shared<volume_io::VolumeContext>();
        signal->volumeContext->background = 0;
        const auto image = dynamic_cast<itkSignal<GraphSegmentType> *>(signal)->imageStorage;
        image->forEachRun([&](const auto &, std::size_t, GraphSegmentType label) {
            volume_io::useLabel(*signal->volumeContext, label);
        });
    }
    allSignalList[signalIndexGlobal]->setCategoricalColorMode();
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setValueColorToTransparent(signal->volumeContext->background.value_or(0));
    allSignalList[signalIndexGlobal]->setupTreeWidget(segmentationTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setIsActive(true);
    applyInitialDisplay(allSignalList[signalIndexGlobal], display);
    attachLayerWidgetToLastItem(segmentationTreeWidget);
    QTreeWidgetItem *newItem = segmentationTreeWidget->topLevelItem(segmentationTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(segmentationTreeWidget, newItem, lastAutoSelectedSegmentationItem);

    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectSegmentationItem(segmentationTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::registerBoundarySignal(size_t signalIndexGlobal, const QString &name,
                                           LayerDisplayOptions display) {
    configureContinuousLayer(allSignalList[signalIndexGlobal], display);
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setupTreeWidget(probabilityTreeWidget, signalIndexGlobal);
    applyInitialDisplay(allSignalList[signalIndexGlobal], display);
    attachLayerWidgetToLastItem(probabilityTreeWidget);
    QTreeWidgetItem *newItem = probabilityTreeWidget->topLevelItem(probabilityTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(probabilityTreeWidget, newItem, lastAutoSelectedBoundaryItem);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectBoundaryItem(probabilityTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::registerRefinementSignal(size_t signalIndexGlobal, const QString &name,
                                             LayerDisplayOptions display) {
    allSignalList[signalIndexGlobal]->setCategoricalColorMode();
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    allSignalList[signalIndexGlobal]->setupTreeWidget(refinementTreeWidget, signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setIsActive(false);
    applyInitialDisplay(allSignalList[signalIndexGlobal], display);
    attachLayerWidgetToLastItem(refinementTreeWidget);
    QTreeWidgetItem *newItem = refinementTreeWidget->topLevelItem(refinementTreeWidget->topLevelItemCount() - 1);
    selectLoadedItemIfAppropriate(refinementTreeWidget, newItem, lastAutoSelectedRefinementItem);
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);
    selectRefinementItem(refinementTreeWidget->currentItem());
    refreshUiState();
}

void SignalControl::registerSegmentsGraphSignal(size_t signalIndexGlobal, bool createSegmentationVolume,
                                                LayerDisplayOptions display, const QString &displayedName) {
    segmentsGraph = allSignalList[signalIndexGlobal];
    auto *typedSegmentsSignal = dynamic_cast<itkSignal<GraphSegmentType> *>(segmentsGraph);
    allSignalList[signalIndexGlobal]->setCategoricalColorMode();
    allSignalList[signalIndexGlobal]->setName(
        signal_name_utils::makeUniqueSignalName(allSignalList, displayedName.isEmpty() ? QStringLiteral("Supervoxels") : displayedName));
    allSignalList[signalIndexGlobal]->setupTreeWidget(signalTreeWidget, signalIndexGlobal);
    applyInitialDisplay(segmentsGraph, display);
    attachLayerWidgetToLastItem(signalTreeWidget);

    graphBase->pWorkingSegments = typedSegmentsSignal;
    graphBase->pWorkingSegmentsImage = typedSegmentsSignal->imageStorage;
    for (const auto &[label, node] : graphBase->pGraph->initialNodes)
        node->setSegmentPointer(graphBase->pWorkingSegmentsImage);

    allSignalList[signalIndexGlobal]->setValueColorToTransparent(graphBase->ignoredSegmentLabels.front());
    orthoViewer->addSignal(allSignalList[signalIndexGlobal]);

    const size_t edgeSignalIndex = allSignalList.size();
    graphBase->pEdgesInitialSegmentsITKSignal->setName(
        signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Edges")));
    allSignalList.push_back(graphBase->pEdgesInitialSegmentsITKSignal);
    graphBase->pEdgesInitialSegmentsITKSignal->setupTreeWidget(signalTreeWidget, edgeSignalIndex);
    graphBase->rebuildEdgeColorPresentation();
    graphBase->pEdgesInitialSegmentsITKSignal->setIsActive(false);
    attachLayerWidgetToLastItem(signalTreeWidget);
    orthoViewer->addSignal(graphBase->pEdgesInitialSegmentsITKSignal);

    orthoViewer->setViewToMiddleOfStack();
    if (createSegmentationVolume) {
        createEmptySegmentation();
    }
    refreshUiState();
}

void SignalControl::addImageAsync(QString fileName, QString displayedName, LoadCallback then, volume_io::ImageLoadOptions options,
                                  LayerDisplayOptions display) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    if (display.color) {
        try {
            const auto info = getImageFileInfo(fileName);
            if (info.pixelType == itk::IOPixelEnum::RGB && info.componentCount == 3) {
                SP_LOG_WARNING("io", QStringLiteral("%1: --color requires a scalar image; skipping RGB file").arg(fileName));
                invokeLoadCallbackLater(std::move(then), std::nullopt);
                return;
            }
        } catch (const std::exception &) {
            // The normal load path reports unreadable files.
        }
    }
    if (!prepareLoadOptions(fileName, options)) { invokeLoadCallbackLater(std::move(then), std::nullopt); return; }
    const auto spacingDecision = askForLoadedFileSpacing(fileName);
    if (!spacingDecision.shouldLoad) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const auto completion = loadCompletion(options, std::move(then));
    taskRunner->runWithLabel(
        QStringLiteral("Loading image..."),
        [this, fileName, options, spacingOverride = spacingDecision.spacingOverride]() {
            auto loadedImage = loadImageData(fileName, options);
            for (auto &layer : loadedImage.layers) {
                applyVoxelSpacingOverride(
                    dynamic_cast<itk::ImageBase<3> *>(layer.image.GetPointer()),
                    spacingOverride);
            }
            return loadedImage;
        },
        [this, fileName, displayedName, display, completion](LoadedImageData loadedImage) {
            if (loadedImage.layers.empty()) {
                invokeLoadCallbackOnce(completion, std::nullopt);
                return;
            }

            const itk::ImageBase<3> *image = asImageBase(loadedImage.layers.front().image);
            if (image == nullptr) {
                invokeLoadCallbackOnce(completion, std::nullopt);
                return;
            }

            const auto dimensions = image->GetLargestPossibleRegion().GetSize();
            if (!dimensionsMatchExpectedDimensions(dimensions[0], dimensions[1], dimensions[2], true)) {
                reportDimensionMismatch(dimensions[0], dimensions[1], dimensions[2], true);
                invokeLoadCallbackOnce(completion, std::nullopt);
                return;
            }

            std::vector<QString> suffixes;
            suffixes.reserve(loadedImage.layers.size());
            for (const LoadedImageLayer &layer : loadedImage.layers) {
                suffixes.push_back(layer.suffix);
            }
            const QString baseName = resolvedDisplayName(fileName, displayedName, suffixes);
            LoadResult firstSignalIndex;
            bool insertedAllLayers = true;
            for (const LoadedImageLayer &layer : loadedImage.layers) {
                size_t signalIndexGlobal = 0;
                if (!insertLoadedImage(layer, loadedImage.dataType, signalIndexGlobal, true)) {
                    insertedAllLayers = false;
                    break;
                }

                registerImageSignal(
                    signalIndexGlobal,
                    baseName + layer.suffix,
                    layer.color, display);
                if (!firstSignalIndex) {
                    firstSignalIndex = signalIndexGlobal;
                }
            }

            if (insertedAllLayers) {
                rememberLoadedSourceFile(fileName);
            }
            invokeLoadCallbackOnce(
                completion,
                insertedAllLayers ? firstSignalIndex : std::nullopt);
        },
        [this, completion]() { invokeLoadCallbackOnce(completion, std::nullopt); });
}

dataType::SegmentsStorage::Pointer SignalControl::duplicateSegmentationAndBuildWorkingSegments(
    const dataType::SegmentsStorage::Pointer &segmentationImage, std::optional<GraphSegmentType> background) {
    auto workingSegmentsImage = duplicateSegmentsImage(segmentationImage);
    prepareWorkingSegmentsGraph(workingSegmentsImage, background);
    return graphBase->pWorkingSegmentsImage;
}

void SignalControl::loadSegmentationVolume(QString fileName, QString displayedName, LoadCallback then, volume_io::ImageLoadOptions options,
                                           LayerDisplayOptions display) {
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

    loadSegmentationVolumeAsync(fileName, displayedName, createWorkingSegments, std::move(then), options, display);
}

void SignalControl::loadSegmentationVolumeAsync(QString fileName,
                                                QString displayedName,
                                                bool createWorkingSegments,
                                                LoadCallback then, volume_io::ImageLoadOptions options,
                                                LayerDisplayOptions display) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    options.labelImage = true;
    options.backgroundIsHighest = graphBase->pGraph->backgroundIdStrategy == "backgroundIsHighestId";
    if (!prepareLoadOptions(fileName, options)) { invokeLoadCallbackLater(std::move(then), std::nullopt); return; }
    const auto spacingDecision = askForLoadedFileSpacing(fileName);
    if (!spacingDecision.shouldLoad) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const bool hadWorkingSegments = hasWorkingSegments();
    const auto expectedDimensions = expectedDimensionsForNewSignal(hadWorkingSegments);
    const auto completion = loadCompletion(options, std::move(then));
    taskRunner->runWithLabel(
        QStringLiteral("Loading segmentation..."),
        [this,
         fileName,
         createWorkingSegments,
         hadWorkingSegments,
         expectedDimensions,
         options,
         spacingOverride = spacingDecision.spacingOverride]() mutable {
            SegmentationLoadResultData result;
            auto loaded = loadLabelVolume(fileName, options);
            result.context = loaded.context;
            applyVoxelSpacingOverride(loaded.image->imageInformation(), spacingOverride);
            if (expectedDimensions && !imageMatchesDimensions(loaded.image, *expectedDimensions)) {
                result.segmentationImage = loaded.image;
                result.dimensionMismatch = true;
                return result;
            }
            if (!hadWorkingSegments)
                graphBase->pGraph->updateBackgroundIdFromVolume(loaded.image, result.context->background);
            if (createWorkingSegments)
                result.workingSegmentsImage = duplicateSegmentationAndBuildWorkingSegments(loaded.image, result.context->background);
            result.segmentationImage = loaded.image;
            return result;
        },
        [this, fileName, displayedName, hadWorkingSegments, display, completion](SegmentationLoadResultData result) {
            if (result.dimensionMismatch && result.segmentationImage != nullptr) {
                const auto dimensions = imageDimensions(result.segmentationImage);
                reportDimensionMismatch(dimensions.x, dimensions.y, dimensions.z, hadWorkingSegments);
                invokeLoadCallbackOnce(completion, std::nullopt);
                return;
            }

            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(result.segmentationImage, signalIndexGlobal, hadWorkingSegments, result.context);
            if (ok) {
                rememberLoadedSourceFile(fileName);
                allSignalList[signalIndexGlobal]->sourceFilePath =
                    graphBase->lastLoadedSourcePath;
                if (result.workingSegmentsImage != nullptr) {
                    size_t segmentIndexGlobal = 0;
                    const bool insertedSegments = insertImageSegmenttype(result.workingSegmentsImage, segmentIndexGlobal, false, std::make_shared<volume_io::VolumeContext>(*result.context));
                    if (insertedSegments) {
                        allSignalList[segmentIndexGlobal]->sourceFilePath =
                            graphBase->lastLoadedSourcePath;
                        registerSegmentsGraphSignal(segmentIndexGlobal, false);
                    }
                }
                registerSegmentationSignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName), display);
            }
            invokeLoadCallbackOnce(completion, ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        },
        [this, completion]() { invokeLoadCallbackOnce(completion, std::nullopt); });
}

void SignalControl::loadRefinementAsync(QString fileName, QString displayedName, LoadCallback then, volume_io::ImageLoadOptions options,
                                        LayerDisplayOptions display) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    if (!hasWorkingSegments()) {
        showInfoMessage("Please load supervoxels first.");
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    options.labelImage = true;
    options.backgroundIsHighest = graphBase->pGraph->backgroundIdStrategy == "backgroundIsHighestId";
    if (!prepareLoadOptions(fileName, options)) { invokeLoadCallbackLater(std::move(then), std::nullopt); return; }
    const auto spacingDecision = askForLoadedFileSpacing(fileName);
    if (!spacingDecision.shouldLoad) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const auto completion = loadCompletion(options, std::move(then));
    taskRunner->runWithLabel(
        QStringLiteral("Loading refinement..."),
        [fileName, options, spacingOverride = spacingDecision.spacingOverride]() mutable {
            auto loaded = loadLabelVolume(fileName, options);
            applyVoxelSpacingOverride(loaded.image->imageInformation(), spacingOverride);
            return loaded;
        },
        [this, fileName, displayedName, display, completion](volume_io::LoadedVolume<dataType::SegmentsStorage> loaded) {
            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(loaded.image, signalIndexGlobal, true, loaded.context);
            if (ok) {
                rememberLoadedSourceFile(fileName);
                registerRefinementSignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName), display);
            }
            invokeLoadCallbackOnce(completion, ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        },
        [this, completion]() { invokeLoadCallbackOnce(completion, std::nullopt); });
}

void SignalControl::addSegmentsGraphAsync(QString fileName, LoadCallback then, volume_io::ImageLoadOptions options,
                                          LayerDisplayOptions display, const QString &displayedName) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    options.labelImage = true;
    options.backgroundIsHighest = graphBase->pGraph->backgroundIdStrategy == "backgroundIsHighestId";
    if (!prepareLoadOptions(fileName, options)) { invokeLoadCallbackLater(std::move(then), std::nullopt); return; }
    const auto spacingDecision = askForLoadedFileSpacing(fileName);
    if (!spacingDecision.shouldLoad) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    struct SegmentsGraphLoadResult {
        dataType::SegmentsStorage::Pointer image;
        std::shared_ptr<volume_io::VolumeContext> context;
        bool dimensionMismatch = false;
    };

    const auto expectedDimensions = expectedDimensionsForNewSignal(false);
    const auto completion = loadCompletion(options, std::move(then));
    taskRunner->runWithLabel(
        QStringLiteral("Loading supervoxels and building graph..."),
        [this,
         fileName,
         expectedDimensions,
         options,
         spacingOverride = spacingDecision.spacingOverride]() mutable {
            SegmentsGraphLoadResult result;
            // Modifies graphBase on the worker thread. Safe only because
            // one task runs at a time and the owning window is blocked.
            auto loaded = loadLabelVolume(fileName, options);
            auto pImage = loaded.image;
            result.context = loaded.context;
            applyVoxelSpacingOverride(pImage->imageInformation(), spacingOverride);
            result.image = pImage;
            if (expectedDimensions && !imageMatchesDimensions(pImage, *expectedDimensions)) {
                result.dimensionMismatch = true;
                return result;
            }
            prepareWorkingSegmentsGraph(pImage, loaded.context->background);
            result.image = graphBase->pWorkingSegmentsImage;
            return result;
        },
        [this, fileName, displayedName, display, completion](SegmentsGraphLoadResult result) {
            if (result.dimensionMismatch && result.image != nullptr) {
                const auto dimensions = imageDimensions(result.image);
                reportDimensionMismatch(dimensions.x, dimensions.y, dimensions.z, false);
                invokeLoadCallbackOnce(completion, std::nullopt);
                return;
            }

            size_t signalIndexGlobal = 0;
            bool ok = insertImageSegmenttype(result.image, signalIndexGlobal, false, result.context);
            if (ok) {
                rememberLoadedSourceFile(fileName);
                allSignalList[signalIndexGlobal]->sourceFilePath =
                    graphBase->lastLoadedSourcePath;
                registerSegmentsGraphSignal(signalIndexGlobal, true, display, displayedName);
            } else {
                QMessageBox::critical(this, tr("Error"), tr("Failed to load the supervoxels."));
            }
            invokeLoadCallbackOnce(completion, ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        },
        [this, completion]() { invokeLoadCallbackOnce(completion, std::nullopt); });
}

void SignalControl::loadMembraneProbabilityAsync(QString fileName,
                                                 QString displayedName,
                                                 BoundaryLoadMode loadMode,
                                                 FloatBoundaryConversionMode floatConversionMode,
                                                 LoadCallback then, volume_io::ImageLoadOptions options,
                                                 LayerDisplayOptions display) {
    if (fileName.isEmpty()) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    if (!prepareLoadOptions(fileName, options)) { invokeLoadCallbackLater(std::move(then), std::nullopt); return; }
    const auto spacingDecision = askForLoadedFileSpacing(fileName);
    if (!spacingDecision.shouldLoad) {
        invokeLoadCallbackLater(std::move(then), std::nullopt);
        return;
    }

    const bool createEmptySegments = loadMode == BoundaryLoadMode::CreateEmptySegments;
    const auto expectedDimensions = expectedDimensionsForNewSignal(false);
    const auto completion = loadCompletion(options, std::move(then));
    taskRunner->runWithLabel(
        QStringLiteral("Loading boundaries..."),
        [this,
         fileName,
         createEmptySegments,
         floatConversionMode,
         expectedDimensions,
         options,
         spacingOverride = spacingDecision.spacingOverride]() mutable {
            BoundaryLoadResult result;
            unsigned int dimension = 0;
            itk::ImageIOBase::IOComponentType boundaryDataType = itk::ImageIOBase::UNKNOWNCOMPONENTTYPE;
            getDimensionAndDataTypeOfFile(fileName, dimension, boundaryDataType);

            auto loadBoundary = [&](auto pixel) {
                using Pixel = decltype(pixel);
                auto loaded = loadVolume<Pixel>(fileName, options);
                result.context = loaded.context;
                if constexpr (std::is_floating_point_v<Pixel>)
                    result.boundaryImage = convertFloatBoundaryImage<Pixel>(loaded.image, floatConversionMode);
                else result.boundaryImage = loaded.image;
            };
            switch (boundaryDataType) {
                case itk::ImageIOBase::FLOAT: loadBoundary(float{}); break;
                case itk::ImageIOBase::DOUBLE: loadBoundary(double{}); break;
                default: loadBoundary(dataType::BoundaryVoxelType{}); break;
            }
            applyVoxelSpacingOverride(result.boundaryImage.GetPointer(), spacingOverride);
            if (createEmptySegments) {
                if (expectedDimensions && !imageMatchesDimensions(result.boundaryImage, *expectedDimensions)) {
                    return result;
                }

                result.emptySegmentsImage = dataType::SegmentsImageType::New();
                result.emptySegmentsImage->SetRegions(result.boundaryImage->GetLargestPossibleRegion());
                result.emptySegmentsImage->SetSpacing(result.boundaryImage->GetSpacing());
                result.emptySegmentsImage->SetOrigin(result.boundaryImage->GetOrigin());
                result.emptySegmentsImage->SetDirection(result.boundaryImage->GetDirection());
                result.emptySegmentsImage->Allocate(true);

                // Modifies graphBase on the worker thread. Safe only because
                // one task runs at a time and the owning window is blocked.
                prepareWorkingSegmentsGraph(result.emptySegmentsImage);
                result.createdEmptySegments = true;
            }
            return result;
        },
        [this, fileName, displayedName, createEmptySegments, display, completion](BoundaryLoadResult result) {
            size_t signalIndexGlobal = 0;
            bool ok = insertTypedImage<dataType::BoundaryVoxelType>(
                result.boundaryImage,
                signalIndexGlobal,
                !createEmptySegments, result.context);
            if (ok) {
                rememberLoadedSourceFile(fileName);
                if (result.createdEmptySegments) {
                    size_t segmentIndexGlobal = 0;
                    const bool insertedSegments = insertImageSegmenttype(result.emptySegmentsImage, segmentIndexGlobal, false, volume_io::derivedContext(result.context));
                    if (insertedSegments) {
                        registerSegmentsGraphSignal(segmentIndexGlobal);
                    }
                }
                registerBoundarySignal(signalIndexGlobal, resolvedDisplayName(fileName, displayedName), display);
            }
            invokeLoadCallbackOnce(completion, ok ? LoadResult{signalIndexGlobal} : std::nullopt);
        },
        [this, completion]() { invokeLoadCallbackOnce(completion, std::nullopt); });
}

void SignalControl::addImage(QString fileName, QString displayedName, volume_io::ImageLoadOptions options) {
    SP_LOG_INFO("io", QStringLiteral("Adding image file %1").arg(fileName));
    addImageAsync(fileName, displayedName, {}, options);
}

QMenu *SignalControl::populateDataMenu(QMenu *menu, QAction *loadSampleDataAction) {
    if (menu == nullptr) {
        return nullptr;
    }

    if (loadSampleDataAction != nullptr) {
        menu->addAction(loadSampleDataAction);
        menu->addSeparator();
    }
    menu->addAction(addImageAction);
    menu->addAction(addSegmentsAction);
    menu->addAction(addBoundariesAction);
    menu->addAction(loadRefinementAction);
    menu->addAction(loadSegmentationAction);
    menu->addSeparator();
    menu->addAction(createEmptySegmentationAction);
    QMenu *exportMenu = menu->addMenu(tr("Export"));
    exportMenu->addAction(exportSegmentationAction);
    return exportMenu;
}

void SignalControl::populateSegmentationMenu(QMenu *menu) {
    if (menu == nullptr) {
        return;
    }

    QMenu *watershedMenu = menu->addMenu(tr("Watershed"));
    watershedMenu->addAction(toggleROISelectionAction);
    watershedMenu->addAction(runWatershedAction);

    QMenu *editMenu = menu->addMenu(tr("Edit"));
    editMenu->addAction(togglePaintModeAction);
    editMenu->addAction(setPaintIdAction);
    editMenu->addAction(dilateSegmentationAction);
    editMenu->addAction(erodeSegmentationAction);
    editMenu->addSeparator();
    QMenu *connectedComponentMenu = editMenu->addMenu(tr("Connected Component Split"));
    connectedComponentMenu->addAction(connectedComponentSplitAction);
    connectedComponentMenu->addSeparator();
    connectedComponentMenu->addAction(connectedComponentSplitTargetInitialAction);
    connectedComponentMenu->addAction(connectedComponentSplitTargetSegmentationAction);
    connectedComponentMenu->addSeparator();
    connectedComponentMenu->addAction(connectedComponentSplitConnectivityFullAction);
    connectedComponentMenu->addAction(connectedComponentSplitConnectivitySixAction);
    editMenu->addAction(mergeSmallSegmentsAction);

    QMenu *transferMenu = menu->addMenu(tr("Transfer"));
    transferMenu->addAction(transferAllAction);
    transferMenu->addAction(transferWithVolumeAction);
    transferMenu->addAction(transferNonOverlappingAction);
    transferMenu->addAction(transferNonOverlappingWithVolumeAction);
    transferMenu->addAction(transferWithRefinementAction);

    QMenu *refinementMenu = menu->addMenu(tr("Refinement"));
    refinementMenu->addAction(mergeWithRefinementAction);
    refinementMenu->addAction(setIdTransparentAction);
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
    createAction(exportSegmentationAction, tr("Selected Segmentation..."), &SignalControl::exportSelectedSegmentation);
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
    connectedComponentSplitAction->setAutoRepeat(false);
    createAction(mergeSmallSegmentsAction,
                 tr("Merge Small Segments with Neighbors..."),
                 &SignalControl::mergeSmallSegmentsWithNeighbors);
    mergeSmallSegmentsAction->setObjectName(QStringLiteral("mergeSmallSegmentsAction"));
    createAction(transferWithVolumeAction, tr("Transfer Supervoxels by Volume"), &SignalControl::transferSegmentsWithVolume);
    createAction(transferAllAction, tr("Transfer All Supervoxels"), &SignalControl::transferAllSegments);
    createAction(transferWithRefinementAction, tr("Transfer Supervoxels by Refinement Overlap"), &SignalControl::transferSupervoxelsByRefinementOverlap);
    createAction(transferNonOverlappingAction,
                 tr("Transfer Non-Overlapping Supervoxels…"),
                 [this]() { showNonOverlappingTransferDialog(NonOverlappingTransferDialogMode::All); });
    createAction(transferNonOverlappingWithVolumeAction,
                 tr("Transfer Non-Overlapping Supervoxels by Volume…"),
                 [this]() { showNonOverlappingTransferDialog(NonOverlappingTransferDialogMode::ByVolume); });

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

QVBoxLayout *SignalControl::createSectionLayout(
    const QString &title,
    QToolButton *&visibilityButton) {
    auto *sectionWidget = new QWidget(sectionSplitter);
    sectionWidget->setMinimumHeight(0);
    sectionWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *sectionLayout = new QVBoxLayout(sectionWidget);
    sectionLayout->setContentsMargins(4, 4, 4, 4);
    sectionLayout->setSpacing(kSectionSpacing);

    auto *header = new QWidget(sectionWidget);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(kSectionSpacing);

    visibilityButton = new QToolButton(header);
    visibilityButton->setAutoRaise(true);
    visibilityButton->setCheckable(true);
    visibilityButton->setChecked(true);
    visibilityButton->setCursor(Qt::PointingHandCursor);
    visibilityButton->setFixedSize(20, 20);
    visibilityButton->setIconSize(QSize(16, 16));
    visibilityButton->setIcon(SignalLayerWidget::visibilityIcon(visibilityButton, true));
    headerLayout->addWidget(visibilityButton);
    headerLayout->addWidget(createSectionLabel(title, header));
    headerLayout->addStretch(1);
    sectionLayout->addWidget(header);
    sectionSplitter->addWidget(sectionWidget);
    return sectionLayout;
}

void SignalControl::registerLayerGroup(
    QTreeWidget *tree,
    QToolButton *visibilityButton) {
    if (tree == nullptr || visibilityButton == nullptr) {
        return;
    }

    connect(tree->model(), &QAbstractItemModel::rowsInserted, this, &SignalControl::scheduleSidebarLayoutUpdate);
    connect(tree->model(), &QAbstractItemModel::rowsRemoved, this, &SignalControl::scheduleSidebarLayoutUpdate);
    connect(tree->model(), &QAbstractItemModel::modelReset, this, &SignalControl::scheduleSidebarLayoutUpdate);

    LayerGroupVisibility &group = layerGroupVisibility[tree];
    group.button = visibilityButton;
    group.visible = true;
    visibilityButton->setObjectName(tree->objectName() + QStringLiteral("GroupVisibilityButton"));
    visibilityButton->setToolTip(tr("Hide all layers in this group"));
    visibilityButton->setAccessibleName(tr("Toggle group visibility"));

    connect(visibilityButton, &QToolButton::toggled, this, [this, tree](bool visible) {
        setLayerGroupVisible(tree, visible);
    });
}

void SignalControl::setLayerGroupVisible(QTreeWidget *tree, bool visible) {
    const auto groupIt = layerGroupVisibility.find(tree);
    if (groupIt == layerGroupVisibility.end() || groupIt->second.visible == visible) {
        return;
    }

    LayerGroupVisibility &group = groupIt->second;
    if (!visible) {
        group.savedLayerVisibility.clear();
        for (int itemIndex = 0; itemIndex < tree->topLevelItemCount(); ++itemIndex) {
            itkSignalBase *signal = signalForItem(tree->topLevelItem(itemIndex));
            if (signal == nullptr) {
                continue;
            }
            group.savedLayerVisibility[signal] = signal->getIsActive();
            signal->setIsActive(false);
        }
    } else {
        for (int itemIndex = 0; itemIndex < tree->topLevelItemCount(); ++itemIndex) {
            itkSignalBase *signal = signalForItem(tree->topLevelItem(itemIndex));
            if (signal == nullptr) {
                continue;
            }
            const auto savedIt = group.savedLayerVisibility.find(signal);
            if (savedIt != group.savedLayerVisibility.end()) {
                signal->setIsActive(savedIt->second);
            }
        }
        group.savedLayerVisibility.clear();
    }

    group.visible = visible;
    group.button->setIcon(SignalLayerWidget::visibilityIcon(group.button, visible));
    group.button->setToolTip(
        visible
            ? tr("Hide all layers in this group")
            : tr("Show this group and restore previous layer visibility"));
    SP_LOG_DEBUG(
        "viewer.interaction",
        QStringLiteral("Layer group tree=%1 visible=%2 layers=%3")
            .arg(tree->objectName())
            .arg(visible)
            .arg(tree->topLevelItemCount()));
    updateLayerSelectionState(tree);
    refreshAfterVisibilityChange();
}

bool SignalControl::layerVisibilityForUi(
    QTreeWidget *tree,
    itkSignalBase *signal) const {
    if (signal == nullptr) {
        return false;
    }

    const auto groupIt = layerGroupVisibility.find(tree);
    if (groupIt == layerGroupVisibility.end() || groupIt->second.visible) {
        return signal->getIsActive();
    }

    const auto savedIt = groupIt->second.savedLayerVisibility.find(signal);
    return savedIt != groupIt->second.savedLayerVisibility.end()
               ? savedIt->second
               : false;
}

void SignalControl::rememberNewLayerWhileGroupHidden(
    QTreeWidget *tree,
    itkSignalBase *signal) {
    const auto groupIt = layerGroupVisibility.find(tree);
    if (signal == nullptr
        || groupIt == layerGroupVisibility.end()
        || groupIt->second.visible) {
        return;
    }

    groupIt->second.savedLayerVisibility[signal] = signal->getIsActive();
    signal->setIsActive(false);
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
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
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

    connect(sectionSplitter, &QSplitter::splitterMoved, this, [this]() {
        sectionHeightsSetByUser = true;
    });
    connect(taskRunner, &TaskRunner::busyChanged, this, &SignalControl::setGuiBusy);
    connect(taskRunner, &TaskRunner::busyChanged, this, [this](bool busy) {
        if (!busy) {
            scheduleDroppedFileProcessing();
        }
    });
    setGuiBusy(taskRunner->isBusy());

    const QPointer<SignalControl> guardedThis(this);
    const auto deleteSelectedLabel =
        [guardedThis](dataType::SegmentsStorage::Pointer segmentation,
                      dataType::SegmentIdType label) {
            return guardedThis != nullptr
                   && guardedThis->deleteSelectedSegmentationLabels(segmentation, {label}) > 0;
        };
    for (auto *viewer : {orthoViewer->xy, orthoViewer->xz, orthoViewer->zy}) {
        if (viewer != nullptr) {
            viewer->setDeleteSelectedSegmentationLabelHandler(deleteSelectedLabel);
        }
    }
}

void SignalControl::setupSignalTreeWidget() {
    QToolButton *groupVisibilityButton = nullptr;
    auto *signalWidgetLayout = createSectionLayout(tr("Layers"), groupVisibilityButton);

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
    registerLayerGroup(signalTreeWidget, groupVisibilityButton);
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
    QToolButton *groupVisibilityButton = nullptr;
    auto *probabilityWidgetLayout = createSectionLayout(tr("Boundaries"), groupVisibilityButton);

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
    registerLayerGroup(probabilityTreeWidget, groupVisibilityButton);
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
    QToolButton *groupVisibilityButton = nullptr;
    auto *refinementWidgetLayout = createSectionLayout(tr("Refinements"), groupVisibilityButton);

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
    registerLayerGroup(refinementTreeWidget, groupVisibilityButton);
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

void SignalControl::handleDroppedFiles(QStringList fileNames) {
    fileNames.removeAll(QString());
    if (fileNames.isEmpty()) {
        return;
    }
    SP_LOG_INFO("io", QStringLiteral("Handling %1 dropped file(s)").arg(fileNames.size()));
    droppedFileBatches.push_back({std::move(fileNames), std::nullopt});
    scheduleDroppedFileProcessing();
}

void SignalControl::scheduleDroppedFileProcessing() {
    if (processingDroppedFile || droppedFileProcessingScheduled
        || droppedFileBatches.empty() || taskRunner->isBusy()) {
        return;
    }

    droppedFileProcessingScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        droppedFileProcessingScheduled = false;
        processNextDroppedFile();
    });
}

void SignalControl::processNextDroppedFile() {
    if (processingDroppedFile || droppedFileBatches.empty() || taskRunner->isBusy()) {
        scheduleDroppedFileProcessing();
        return;
    }

    processingDroppedFile = true;
    DroppedFileBatch &batch = droppedFileBatches.front();
    ImageLoadChoice loadChoice = ImageLoadChoice::Image;
    if (batch.applyToRemaining) {
        loadChoice = *batch.applyToRemaining;
    } else {
        QImageSelectionRadioButtons chooser(this, batch.files.size() > 1, batch.files.front(), cropLoadDefaults);
        if (chooser.exec() != QDialog::Accepted) {
            droppedFileBatches.pop_front();
            processingDroppedFile = false;
            scheduleDroppedFileProcessing();
            return;
        }
        loadChoice = chooser.selectedChoice();
        batch.options = chooser.loadOptions();
        if (imageLoadChoiceSupportsApplyToAll(loadChoice) && chooser.applyToAll()) {
            batch.applyToRemaining = loadChoice;
        }
    }

    const QString fileName = batch.files.takeFirst();
    const auto options = batch.options;
    if (batch.files.isEmpty()) {
        droppedFileBatches.pop_front();
    }
    SP_LOG_INFO("io", QStringLiteral("Handling dropped file %1").arg(fileName));
    loadDroppedFileAs(fileName, loadChoice, options);
    processingDroppedFile = false;
    scheduleDroppedFileProcessing();
}

void SignalControl::loadDroppedFileAs(QString fileName, ImageLoadChoice loadChoice, volume_io::ImageLoadOptions options) {
    if (loadChoiceRequiresWorkingSegments(loadChoice) && !hasWorkingSegments()) {
        showInfoMessage("Please load supervoxels first.");
        return;
    }

    switch (loadChoice) {
        case ImageLoadChoice::Supervoxels:
            askForBackgroundStrategy();
            addSegmentsGraph(fileName, options);
            break;
        case ImageLoadChoice::Image:
            addImage(fileName, QString(""), options);
            break;
        case ImageLoadChoice::Boundaries:
            if (!hasWorkingSegments()) {
                loadMembraneProbability(fileName, {}, options);
            } else {
                loadMembraneProbability(fileName, QString(""), options);
            }
            break;
        case ImageLoadChoice::Refinement:
            loadRefinement(fileName, QString(""), options);
            break;
        case ImageLoadChoice::Segmentation:
            loadSegmentationVolume(fileName, QString(""), {}, options);
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


void SignalControl::showNonOverlappingTransferSettings() {
    showNonOverlappingTransferDialog(NonOverlappingTransferDialogMode::Settings);
}

void SignalControl::showNonOverlappingTransferDialog(NonOverlappingTransferDialogMode mode) {
    namespace transfer_settings = segment_puzzler::non_overlapping_transfer;
    const bool settingsOnly = mode == NonOverlappingTransferDialogMode::Settings;
    const bool filterByVolume = mode == NonOverlappingTransferDialogMode::ByVolume;
    if (!settingsOnly && (!hasWorkingSegments() || !hasSelectedSegmentation())) {
        showInfoMessage("Please load supervoxels and load or create a segmentation first.");
        return;
    }

    const auto defaults = transfer_settings::loadSettings();
    QDialog optionsDialog(this);
    optionsDialog.setWindowTitle(settingsOnly
        ? tr("Non-Overlapping Transfer Settings")
        : (filterByVolume ? tr("Transfer Non-Overlapping Supervoxels by Volume")
                          : tr("Transfer Non-Overlapping Supervoxels")));
    auto *form = new QFormLayout(&optionsDialog);

    auto *overlapSpinBox = new QDoubleSpinBox(&optionsDialog);
    overlapSpinBox->setRange(0.0, 100.0);
    overlapSpinBox->setDecimals(1);
    overlapSpinBox->setSuffix(tr(" %"));
    overlapSpinBox->setValue(100.0 * defaults.maximumOverlapFraction);
    form->addRow(tr("Maximum overlap with existing segmentation:"), overlapSpinBox);
    auto *overlapHint = new QLabel(
        tr("Maximum percentage of each supervoxel already covered by the existing segmentation. "
           "0% allows no overlap."), &optionsDialog);
    overlapHint->setWordWrap(true);
    form->addRow(overlapHint);

    QSpinBox *volumeSpinBox = nullptr;
    if (filterByVolume) {
        volumeSpinBox = new QSpinBox(&optionsDialog);
        volumeSpinBox->setRange(0, 1000000);
        volumeSpinBox->setValue(50000);
        volumeSpinBox->setSuffix(tr(" voxels"));
        volumeSpinBox->setToolTip(tr("Uses the entire supervoxel volume before preserving existing labels."));
        form->addRow(tr("Supervoxel volume greater than:"), volumeSpinBox);
    }

    auto *preserveSegmentationCheckBox = new QCheckBox(tr("Preserve existing segmentation"), &optionsDialog);
    preserveSegmentationCheckBox->setChecked(defaults.preserveExistingSegmentation);
    form->addRow(preserveSegmentationCheckBox);
    auto *priorityHint = new QLabel(
        tr("When checked, only empty voxels are filled. Otherwise, transferred supervoxels "
           "overwrite existing labels where they overlap."), &optionsDialog);
    priorityHint->setWordWrap(true);
    form->addRow(priorityHint);

    auto *buttons = new QDialogButtonBox(
        (settingsOnly ? QDialogButtonBox::Save : QDialogButtonBox::Ok) | QDialogButtonBox::Cancel,
        &optionsDialog);
    if (!settingsOnly) {
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Transfer"));
    }
    connect(buttons, &QDialogButtonBox::accepted, &optionsDialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &optionsDialog, &QDialog::reject);
    form->addRow(buttons);

    if (optionsDialog.exec() != QDialog::Accepted) {
        return;
    }
    const double maximumOverlapFraction = overlapSpinBox->value() / 100.0;
    const double minimumVoxelCount = volumeSpinBox != nullptr ? volumeSpinBox->value() : 0;
    const bool preserveExistingSegmentation = preserveSegmentationCheckBox->isChecked();
    if (settingsOnly) {
        transfer_settings::saveSettings({maximumOverlapFraction, preserveExistingSegmentation});
        return;
    }
    taskRunner->runWithLabel(
        QStringLiteral("Transferring non-overlapping supervoxels..."),
        [this, maximumOverlapFraction, minimumVoxelCount, preserveExistingSegmentation]() {
            graphBase->pGraph->transferNonOverlappingSegments(
                maximumOverlapFraction, minimumVoxelCount, preserveExistingSegmentation);
        },
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

    QDialog optionsDialog(this);
    optionsDialog.setWindowTitle(tr("Transfer by overlap"));
    auto *form = new QFormLayout(&optionsDialog);

    auto *overlapHint = new QLabel(
        tr("Transfer each working supervoxel if more than"),
        &optionsDialog);
    overlapHint->setWordWrap(true);
    form->addRow(overlapHint);

    auto *overlapPercentageSpinBox = new QDoubleSpinBox(&optionsDialog);
    overlapPercentageSpinBox->setRange(0.0, 100.0);
    overlapPercentageSpinBox->setDecimals(1);
    overlapPercentageSpinBox->setSingleStep(5.0);
    overlapPercentageSpinBox->setSuffix(tr(" %"));
    overlapPercentageSpinBox->setValue(100.0 * Graph::kDefaultRefinementOverlapThreshold);
    form->addRow(overlapPercentageSpinBox,
                 new QLabel(tr("of its voxels lie in one refinement segment."), &optionsDialog));

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &optionsDialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Transfer"));
    connect(buttons, &QDialogButtonBox::accepted, &optionsDialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &optionsDialog, &QDialog::reject);
    form->addRow(buttons);

    if (optionsDialog.exec() != QDialog::Accepted) {
        return;
    }
    const double overlapThreshold = overlapPercentageSpinBox->value() / 100.0;

    taskRunner->runWithLabel(
        QStringLiteral("Transferring supervoxels by refinement overlap..."),
        [this, overlapThreshold]() {
            graphBase->pGraph->transferSegmentsWithRefinementOverlap(overlapThreshold);
        },
        [this]() { refreshViewers(); });
}


void SignalControl::setupSegmentationTreeWidget() {
    // funcitonality: toggle visibility of segmentations
    // choose the active segmentation to transfer/delete to and from
    // rename segmentations

    // current active segmentation is implemented by a pointer set in graphbase
    // i.e., if the active segmentation is changed,
    // the pointer in graphbase should be pointing to the new segmentation volume

    QToolButton *groupVisibilityButton = nullptr;
    auto *segmentationWidgetLayout = createSectionLayout(tr("Segmentations"), groupVisibilityButton);

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
    registerLayerGroup(segmentationTreeWidget, groupVisibilityButton);
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
    mergeSmallSegmentsButton = new QPushButton();
    togglePaintBrushButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setPaintIdButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connectedComponentSplitButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mergeSmallSegmentsButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    segmentationButtonRow->addWidget(togglePaintBrushButton);
    segmentationButtonRow->addWidget(setPaintIdButton);
    segmentationButtonRow->addWidget(connectedComponentSplitButton);
    segmentationButtonRow->addWidget(mergeSmallSegmentsButton);
    segmentationButtonRow->addStretch();

    segmentationWidgetLayout->addLayout(segmentationButtonRow);

    // Keep the panel button short for space; the shared action keeps the full
    // wording used in menus and dialogs.
    bindButtonToAction(exportSegmentationButton, exportSegmentationAction, tr("Export Selected"));
    bindButtonToAction(togglePaintBrushButton, togglePaintModeAction);
    bindButtonToAction(setPaintIdButton, setPaintIdAction);
    bindButtonToAction(connectedComponentSplitButton, connectedComponentSplitAction, tr("CC Split"));
    bindButtonToAction(mergeSmallSegmentsButton, mergeSmallSegmentsAction, tr("Merge Small"));
    mergeSmallSegmentsButton->setObjectName(QStringLiteral("mergeSmallSegmentsButton"));

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
    graphBase->pSelectedRefinementSignal->setValueColorToTransparent(inputVal);
    refreshViewers();
}

void SignalControl::segmentationClicked(QTreeWidgetItem *item, int index) {
    treeClicked(item, index);
    selectSegmentationItem(item);
    SP_LOG_DEBUG("segmentation", QStringLiteral("Selected segmentation maxId=%1").arg(graphBase->pGraph->selectedLabelHighWaterMark()));
    refreshUiState();
}

void SignalControl::setUserColor(QTreeWidgetItem *item) {
    const size_t signalIndex = signalIndexForItem(item);
    itkSignalBase *signal = allSignalList[signalIndex];
    if (signal->usesCategoricalColors()) {
        const auto nextMode =
            signal->getLabelRenderMode() == itkSignalBase::LabelRenderMode::Filled
                ? itkSignalBase::LabelRenderMode::Boundaries
                : itkSignalBase::LabelRenderMode::Filled;
        signal->setLabelRenderMode(nextMode);
        SP_LOG_INFO("viewer.render",
                    QStringLiteral("Label render mode signal=%1 mode=%2")
                        .arg(signal->name,
                             nextMode == itkSignalBase::LabelRenderMode::Boundaries
                                 ? QStringLiteral("boundaries")
                                 : QStringLiteral("filled")));
        refreshLayerWidget(item->treeWidget(), item);
        refreshViewers();
        return;
    }

    if (signal->usesEdgeStatusColors()) {
        return;
    }

    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Opening color picker for signal index=%1").arg(signalIndex));
    }

    QColor newColor = QColorDialog::getColor();
    if (!newColor.isValid()) {
        return;
    }

    signal->setMainColor(newColor);

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
    const QString defaultSavePath = MySettings.value(DEFAULT_SAVE_DIR_KEY).toString();
    const QString suggestedPath = suggestedSegmentationExportPath(defaultSavePath);
    const auto exportedImage = graphBase->pSelectedSegmentation;
    auto *exportedSignal = graphBase->pSelectedSegmentationSignal;
    volume_io::Coordinates size{};
    for (unsigned int axis = 0; axis < 3; ++axis) size[axis] = exportedImage->GetBufferedRegion().GetSize(axis);
    const auto context = exportedSignal && exportedSignal->volumeContext
        ? std::make_shared<volume_io::VolumeContext>(*exportedSignal->volumeContext)
        : std::make_shared<volume_io::VolumeContext>();
    VolumeExportDialog dialog(this, suggestedPath, size, context);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString path = dialog.selectedFiles().front();
    if (dialog.insertsIntoVolume()) {
        const auto options = dialog.writeOptions();
        const auto start = dialog.targetStart();
        const auto savedMTime = exportedImage->GetMTime();
        taskRunner->runWithLabel(tr("Inserting region into volume..."),
            [exportedImage, path, start, options] {
                return volume_io::writeRegionToVolume(exportedImage, path, start, options);
            },
            [this, exportedImage, exportedSignal, savedMTime](volume_io::RegionWriteResult result) {
                if (exportedSignal) {
                    if (!exportedSignal->volumeContext) exportedSignal->volumeContext = std::make_shared<volume_io::VolumeContext>();
                    volume_io::recordWrite(*exportedSignal->volumeContext, result);
                }
                graphBase->lastExportPathThisSession = result.binding.path;
                emit selectedSegmentationSaved(reinterpret_cast<quintptr>(exportedImage.GetPointer()),
                    reinterpret_cast<quintptr>(exportedSignal), static_cast<quint64>(savedMTime), result.binding.path);
            });
        return;
    }
    const auto canonical = volume_io::canonicalPath(path);
    for (const auto *signal : allSignalList) {
        if (!signal->volumeContext) continue;
        if ((signal->volumeContext->crop || context->crop) && canonical == signal->volumeContext->sourcePath) {
            showInfoMessage(tr("This path belongs to a loaded full volume. Use 'Insert into existing volume' to update only the crop."));
            return;
        }
        for (const auto &target : signal->volumeContext->targets) {
            if (canonical == target.path) {
                showInfoMessage(tr("This file is an insertion target. Use 'Insert into existing volume' to preserve its surrounding voxels."));
                return;
            }
        }
    }
    SP_LOG_INFO("io", QStringLiteral("Export selected segmentation dialog returned path=%1").arg(path));
    if (path.isEmpty()) return;
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    const auto savedMTime = exportedImage->GetMTime();
    taskRunner->runWithLabel(tr("Exporting segmentation..."),
        [exportedImage, absolutePath] { return volume_io::writeLabelVolume(*exportedImage, absolutePath); },
        [this, exportedImage, exportedSignal, savedMTime, absolutePath](QByteArray fingerprint) {
            if (exportedSignal && exportedSignal->volumeContext &&
                volume_io::canonicalPath(absolutePath) == exportedSignal->volumeContext->sourcePath)
                exportedSignal->volumeContext->sourceFingerprint = std::move(fingerprint);
            graphBase->lastExportPathThisSession = absolutePath;
            QSettings().setValue(QStringLiteral("default_save_dir"), absolutePath);
            if (exportedSignal) exportedSignal->sourceFilePath = absolutePath;
            emit selectedSegmentationSaved(reinterpret_cast<quintptr>(exportedImage.GetPointer()),
                reinterpret_cast<quintptr>(exportedSignal), static_cast<quint64>(savedMTime), absolutePath);
        });

}

void SignalControl::setPaintId(){
    if (!hasSelectedSegmentation()) {
        showInfoMessage("Please load or create a segmentation first.");
        return;
    }
    const auto suggested = graphBase->pGraph->nextSelectedLabel();
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Set Paint Label ID"),
        tr("Existing object ID, or a new ID of at least %1:").arg(suggested), QLineEdit::Normal,
        QString::number(suggested <= std::numeric_limits<dataType::SegmentIdType>::max() ? suggested : 0), &ok);
    if (!ok) return;
    const auto value = text.toULongLong(&ok);
    if (!ok || value > std::numeric_limits<dataType::SegmentIdType>::max()) {
        showInfoMessage(tr("Enter a non-negative 32-bit label ID.")); return;
    }
    const auto paintId = static_cast<dataType::SegmentIdType>(value);
    const auto context = graphBase->pSelectedSegmentationSignal->volumeContext;
    bool known = paintId == context->background.value_or(0) || context->originalLabels.count(paintId);
    if (!known) graphBase->pSelectedSegmentation->forEachRun([&](const auto &, std::size_t, auto label) {
        if (label == paintId) known = true;
    });
    if (!known && value < context->nextLabel) {
        showInfoMessage(tr("This ID is not an existing object in the loaded region. Choose a new ID of at least %1.").arg(context->nextLabel)); return;
    }
    if (!known) volume_io::useLabel(*context, paintId);
    SP_LOG_INFO("segmentation", QStringLiteral("Setting paint id to %1").arg(paintId));
    orthoViewer->setPaintId(paintId);
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

void SignalControl::mergeSmallSegmentsWithNeighbors() {
    if (!hasWorkingSegments() || !hasSelectedSegmentation()
        || graphBase == nullptr || graphBase->pGraph == nullptr) {
        showInfoMessage(tr("Selected Segmentation and Working Segments must both be available."));
        return;
    }

    QSettings settings;
    constexpr int defaultThreshold = 1000;
    const int previousThreshold = std::max(
        1,
        settings.value(QStringLiteral("Segmentation/mergeSmallVoxelThreshold"),
                       defaultThreshold).toInt());
    const auto previousSelection = loadNeighborSelection(settings);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Merge Small Segments"));
    auto *form = new QFormLayout(&dialog);
    auto *thresholdSpinBox = new QSpinBox(&dialog);
    thresholdSpinBox->setRange(1, std::numeric_limits<int>::max());
    thresholdSpinBox->setValue(previousThreshold);
    auto *neighborSelectionComboBox =
        createNeighborSelectionComboBox(&dialog, previousSelection);
    form->addRow(tr("Fewer voxels than:"), thresholdSpinBox);
    form->addRow(tr("Merge with:"), neighborSelectionComboBox);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const int threshold = thresholdSpinBox->value();
    const auto neighborSelection = static_cast<Graph::SegmentationNeighborSelection>(
        neighborSelectionComboBox->currentData().toInt());
    settings.setValue(QStringLiteral("Segmentation/mergeSmallVoxelThreshold"), threshold);
    saveNeighborSelection(settings, neighborSelection);

    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("operation=merge_small_segments phase=ui_request threshold=%1 "
                       "neighbor_selection=%2 iterative=1")
            .arg(threshold)
            .arg(QString::fromLatin1(
                Graph::segmentationNeighborSelectionName(neighborSelection))));
    Graph::SegmentationNeighborMergeOptions options;
    options.neighborSelection = neighborSelection;
    runNeighborMerge({}, static_cast<std::size_t>(threshold), options);
}

void SignalControl::mergeSelectedSegmentsWithNeighbors(
    std::vector<dataType::SegmentIdType> labels)
{
    if (labels.empty()) {
        return;
    }

    QSettings settings;
    const auto previousSelection = loadNeighborSelection(settings);
    const auto selectedStrategy = askNeighborSelection(this, previousSelection);
    if (!selectedStrategy.has_value()) {
        return;
    }

    const auto neighborSelection = *selectedStrategy;
    saveNeighborSelection(settings, neighborSelection);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("operation=merge_with_neighbor phase=strategy_selected selected_count=%1 "
                       "neighbor_selection=%2")
            .arg(labels.size())
            .arg(QString::fromLatin1(
                Graph::segmentationNeighborSelectionName(neighborSelection))));

    Graph::SegmentationNeighborMergeOptions options;
    options.neighborSelection = neighborSelection;
    runNeighborMerge(std::move(labels), std::nullopt, options);
}

SignalControl::NeighborMergeAttempt
SignalControl::computeNeighborMergeAttempt(Graph *graph, std::vector<dataType::SegmentIdType> labels,
                                           std::optional<std::size_t> exclusiveVoxelThreshold,
                                           const Graph::SegmentationNeighborMergeOptions &options) {
    NeighborMergeAttempt attempt;
    attempt.labels = std::move(labels);
    if (!exclusiveVoxelThreshold.has_value()) {
        attempt.result = graph->mergeSelectedSegmentationLabelsWithNeighbors(attempt.labels, options);
        return attempt;
    }

    if (attempt.labels.empty()) {
        attempt.labels = graph->selectedSegmentationLabelsBelowVoxelCount(*exclusiveVoxelThreshold);
        attempt.initialMatchingLabelCount = attempt.labels.size();
        attempt.scannedInitialLabels = true;
    }
    // A skipped label has no eligible face-neighbor. Successful merge rounds
    // only relabel consumed components, which therefore cannot touch a skipped
    // label. Keep skipped labels out of later rounds and propagate only fresh
    // merge results. Connected-component splitting is the one operation that
    // can create additional labels, so that path falls back to an image scan.
    std::size_t deferredNoNeighborCount = 0;
    while (!attempt.labels.empty()) {
        auto result = graph->mergeSelectedSegmentationLabelsWithNeighbors(attempt.labels, options);
        if (result.status != Graph::SegmentationNeighborMergeResult::Status::Merged) {
            if (result.status == Graph::SegmentationNeighborMergeResult::Status::NothingToMerge) {
                attempt.remainingMatchingLabelCount =
                    deferredNoNeighborCount + result.selectedLabelCount;
            }
            attempt.result = std::move(result);
            return attempt;
        }
        if (result.consumedLabelCount <= result.mergedGroupCount) {
            result.status = Graph::SegmentationNeighborMergeResult::Status::Failed;
            result.message = QStringLiteral("A merge iteration did not reduce the number of segments.");
            attempt.result = std::move(result);
            return attempt;
        }

        const std::size_t newResultLabelCount = result.voxelCountByNewLabel.size();
        const bool requiresAuthoritativeRescan = result.disconnectedLabelCount > 0;
        if (requiresAuthoritativeRescan) {
            attempt.labels =
                graph->selectedSegmentationLabelsBelowVoxelCount(*exclusiveVoxelThreshold);
            deferredNoNeighborCount = 0;
        } else {
            deferredNoNeighborCount += result.skippedNoNeighborCount;
            attempt.labels = newLabelsBelowVoxelCount(result, *exclusiveVoxelThreshold);
        }
        SP_LOG_DEBUG(
            "segmentation",
            QStringLiteral("operation=merge_small_segments phase=candidate_propagation "
                           "new_result_labels=%1 below_threshold=%2 deferred_no_neighbor=%3 "
                           "fallback_scan=%4")
                .arg(newResultLabelCount)
                .arg(attempt.labels.size())
                .arg(deferredNoNeighborCount)
                .arg(requiresAuthoritativeRescan));
        attempt.completedIterations.push_back(std::move(result));
    }
    attempt.remainingMatchingLabelCount = deferredNoNeighborCount;
    attempt.result.status = Graph::SegmentationNeighborMergeResult::Status::NothingToMerge;
    attempt.result.message = deferredNoNeighborCount == 0
                                 ? QStringLiteral("No segments remain below the voxel threshold.")
                                 : QStringLiteral("No remaining segment below the voxel threshold has an "
                                                  "eligible face-neighbor.");
    return attempt;
}

void SignalControl::runNeighborMerge(std::vector<dataType::SegmentIdType> labels,
                                     std::optional<std::size_t> exclusiveVoxelThreshold,
                                     Graph::SegmentationNeighborMergeOptions options,
                                     std::shared_ptr<NeighborMergeLoopProgress> loopProgress) {
    if (taskRunner == nullptr || taskRunner->isBusy() || graphBase == nullptr ||
        graphBase->pGraph == nullptr || graphBase->pSelectedSegmentation == nullptr ||
        graphBase->pWorkingSegmentsImage == nullptr) {
        return;
    }

    Graph *const graph = graphBase->pGraph;
    const quintptr segmentationIdentity = reinterpret_cast<quintptr>(graphBase->pSelectedSegmentation.GetPointer());
    const quintptr segmentationSignalIdentity = reinterpret_cast<quintptr>(graphBase->pSelectedSegmentationSignal);

    const bool thresholdWorkflow = exclusiveVoxelThreshold.has_value();
    if (thresholdWorkflow && loopProgress == nullptr) {
        loopProgress = std::make_shared<NeighborMergeLoopProgress>();
    }
    const QString progressText = thresholdWorkflow
                                     ? tr("Merging segments below %1 voxels...").arg(*exclusiveVoxelThreshold)
                                     : tr("Merging selected segments with neighbors...");

    taskRunner->runWithLabel(
        progressText,
        [graph, labels = std::move(labels), exclusiveVoxelThreshold, options]() mutable {
            return computeNeighborMergeAttempt(graph, std::move(labels), exclusiveVoxelThreshold, options);
        },
        [this, exclusiveVoxelThreshold, options, loopProgress, segmentationIdentity,
         segmentationSignalIdentity](auto attempt) mutable {
            // Run prompts after TaskRunner has closed its modal progress dialog
            // and returned to idle, so a confirmed retry can start cleanly.
            QTimer::singleShot(0, this,
                               [this, attempt = std::move(attempt), exclusiveVoxelThreshold, options, loopProgress,
                                segmentationIdentity, segmentationSignalIdentity]() mutable {
                                   handleNeighborMergeAttempt(std::move(attempt), exclusiveVoxelThreshold, options,
                                                              loopProgress, segmentationIdentity,
                                                              segmentationSignalIdentity);
                               });
        });
}

void SignalControl::handleNeighborMergeAttempt(NeighborMergeAttempt attempt,
                                               std::optional<std::size_t> exclusiveVoxelThreshold,
                                               Graph::SegmentationNeighborMergeOptions options,
                                               std::shared_ptr<NeighborMergeLoopProgress> loopProgress,
                                               quintptr segmentationIdentity, quintptr segmentationSignalIdentity) {
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    const bool thresholdWorkflow = exclusiveVoxelThreshold.has_value();
    const QString operationName =
        thresholdWorkflow ? QStringLiteral("merge_small_segments") : QStringLiteral("merge_with_neighbor");
    const char *selectionName =
        Graph::segmentationNeighborSelectionName(options.neighborSelection);
    const QString selectionNameForLog =
        QString::fromLatin1(selectionName != nullptr ? selectionName : "unknown");

    auto labels = std::move(attempt.labels);
    auto result = std::move(attempt.result);
    auto completedIterations = std::move(attempt.completedIterations);
    const std::size_t remainingMatchingLabelCount = attempt.remainingMatchingLabelCount;
    const auto refreshChangedSegmentation = [this]() {
        graphBase->rebuildEdgeColorPresentation();
        refreshViewers();
    };

    bool completedDataChanged = false;
    if (thresholdWorkflow && loopProgress != nullptr) {
        if (attempt.scannedInitialLabels && !loopProgress->initialized) {
            loopProgress->initialized = true;
            loopProgress->initialMatchingLabelCount = attempt.initialMatchingLabelCount;
        }
        for (const auto &iterationResult : completedIterations) {
            ++loopProgress->iterationCount;
            loopProgress->mergedSourceCount += iterationResult.mergeableSelectedLabelCount;
            loopProgress->mergedGroupCount += iterationResult.mergedGroupCount;
            completedDataChanged = completedDataChanged || iterationResult.dataChanged;
        }
    }
    if (completedDataChanged) {
        refreshChangedSegmentation();
        for (const auto &iterationResult : completedIterations) {
            emit selectedSegmentationNeighborMergeFinished(segmentationIdentity, segmentationSignalIdentity,
                                                           iterationResult);
        }
    }

    if (result.status == Status::NeedsConnectedComponentConfirmation && !options.allowConnectedComponentSplit) {
        const auto answer = QMessageBox::question(this, tr("Disconnected Segments"),
                                                  tr("At least one involved segment is disconnected (%1 regions were "
                                                     "found).\n\n"
                                                     "Run Connected Components (6-connected), reinsert the resulting "
                                                     "segments "
                                                     "into Working Segments, and continue merging?")
                                                      .arg(result.disconnectedRegionCount),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        SP_LOG_INFO("segmentation",
                    QStringLiteral("operation=%1 phase=component_prompt regions=%2 answer=%3")
                        .arg(operationName)
                        .arg(result.disconnectedRegionCount)
                        .arg(answer == QMessageBox::Yes ? QStringLiteral("yes") : QStringLiteral("no")));
        if (answer == QMessageBox::Yes) {
            auto retryOptions = options;
            retryOptions.allowInsertion = true;
            retryOptions.allowConnectedComponentSplit = true;
            runNeighborMerge(
                thresholdWorkflow ? std::vector<dataType::SegmentIdType>{} : std::move(labels),
                exclusiveVoxelThreshold,
                retryOptions,
                loopProgress);
        }
        return;
    }

    if (result.status == Status::NeedsInsertionConfirmation && !options.allowInsertion) {
        const auto answer = QMessageBox::question(this, tr("Insert into Working Segments?"),
                                                  tr("%1 of %2 involved segments do not exactly match an existing "
                                                     "Working Segment.\n\n"
                                                     "Insert them into Working Segments before merging?")
                                                      .arg(result.requiredInsertionCount)
                                                      .arg(result.consumedLabelCount),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        SP_LOG_INFO("segmentation",
                    QStringLiteral("operation=%1 phase=insertion_prompt insertions=%2 answer=%3")
                        .arg(operationName)
                        .arg(result.requiredInsertionCount)
                        .arg(answer == QMessageBox::Yes ? QStringLiteral("yes") : QStringLiteral("no")));
        if (answer == QMessageBox::Yes) {
            auto retryOptions = options;
            retryOptions.allowInsertion = true;
            runNeighborMerge(
                thresholdWorkflow ? std::vector<dataType::SegmentIdType>{} : std::move(labels),
                exclusiveVoxelThreshold,
                retryOptions,
                loopProgress);
        }
        return;
    }

    if (result.status == Status::Merged) {
        refreshChangedSegmentation();
        emit selectedSegmentationNeighborMergeFinished(segmentationIdentity, segmentationSignalIdentity, result);
        SP_LOG_INFO("segmentation",
                    QStringLiteral("operation=%1 status=merged threshold=%2 "
                                   "neighbor_selection=%3 requested=%4 mergeable=%5 "
                                   "skipped=%6 groups=%7 consumed=%8")
                        .arg(operationName)
                        .arg(exclusiveVoxelThreshold.has_value() ? QString::number(*exclusiveVoxelThreshold)
                                                                 : QStringLiteral("none"))
                        .arg(selectionNameForLog)
                        .arg(labels.size())
                        .arg(result.mergeableSelectedLabelCount)
                        .arg(result.skippedNoNeighborCount)
                        .arg(result.mergedGroupCount)
                        .arg(result.consumedLabelCount));
        return;
    }

    if (result.status == Status::NothingToMerge) {
        QString message;
        if (thresholdWorkflow) {
            const std::size_t iterations = loopProgress != nullptr ? loopProgress->iterationCount : 0;
            if (remainingMatchingLabelCount == 0) {
                message = iterations == 0 ? tr("No segments have fewer than %1 voxels.").arg(*exclusiveVoxelThreshold)
                                          : tr("Iterative merging finished after %1 round(s).\n"
                                               "No segments remain below %2 voxels.")
                                                .arg(iterations)
                                                .arg(*exclusiveVoxelThreshold);
            } else {
                message = tr("Iterative merging stopped after %1 round(s).\n"
                             "%2 segment(s) remain below %3 voxels, but none has "
                             "an eligible face-neighbor.")
                              .arg(iterations)
                              .arg(remainingMatchingLabelCount)
                              .arg(*exclusiveVoxelThreshold);
            }
            SP_LOG_INFO("segmentation",
                        QStringLiteral("operation=%1 status=%2 threshold=%3 "
                                       "neighbor_selection=%4 iterations=%5 initial=%6 "
                                       "remaining=%7 merged_sources=%8 groups=%9")
                            .arg(operationName)
                            .arg(remainingMatchingLabelCount == 0
                                     ? QStringLiteral("complete")
                                     : QStringLiteral("fixed_point"))
                            .arg(*exclusiveVoxelThreshold)
                            .arg(selectionNameForLog)
                            .arg(iterations)
                            .arg(loopProgress != nullptr ? loopProgress->initialMatchingLabelCount : 0)
                            .arg(remainingMatchingLabelCount)
                            .arg(loopProgress != nullptr ? loopProgress->mergedSourceCount : 0)
                            .arg(loopProgress != nullptr ? loopProgress->mergedGroupCount : 0));
        } else {
            message =
                result.message.isEmpty() ? tr("No selected segment has an eligible face-neighbor.") : result.message;
        }
        QMessageBox::information(this, thresholdWorkflow ? tr("Merge Small Segments") : tr("Merge with Neighbor"),
                                 message);
        return;
    }

    if (result.dataChanged) {
        refreshChangedSegmentation();
        emit selectedSegmentationNeighborMergeFinished(segmentationIdentity, segmentationSignalIdentity, result);
    }
    SP_LOG_WARNING("segmentation",
                   QStringLiteral("operation=%1 status=failed threshold=%2 "
                                  "neighbor_selection=%3 iterations=%4 requested=%5 "
                                  "data_changed=%6 message=\"%7\"")
                       .arg(operationName)
                       .arg(exclusiveVoxelThreshold.has_value() ? QString::number(*exclusiveVoxelThreshold)
                                                                : QStringLiteral("none"))
                       .arg(selectionNameForLog)
                       .arg(loopProgress != nullptr ? loopProgress->iterationCount : 0)
                       .arg(labels.size())
                       .arg(result.dataChanged)
                       .arg(result.message));
    QMessageBox::warning(this, thresholdWorkflow ? tr("Merge Small Segments Failed") : tr("Merge with Neighbor Failed"),
                         result.message.isEmpty()
                             ? (thresholdWorkflow ? tr("The small segments could not be merged.")
                                                  : tr("The selected segments could not be merged."))
                             : result.message);
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
                graphBase->rebuildEdgeColorPresentation();
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
    ignoredLabels.insert(graphBase->pGraph->selectedBackground());

    const auto selectedSegmentation = graphBase->pSelectedSegmentation;
    const auto context = graphBase->selectedSegmentationContext;
    const auto nextFreeLabel = context->nextLabel;

    taskRunner->runWithLabel(
        tr("Splitting disconnected segmentation labels..."),
        [selectedSegmentation, ignoredLabels = std::move(ignoredLabels), connectivity, nextFreeLabel]() {
            ConnectedComponentSplitOptions options;
            options.connectivity = connectivity;
            options.ignoredLabels = ignoredLabels;
            options.nextFreeLabel = nextFreeLabel;
            auto stats = splitDisconnectedLabelComponentsInPlace(selectedSegmentation, options);
            if (stats.changed()) {
                selectedSegmentation->Modified();
            }
            return stats;
        },
        [this, connectivityName, context](ConnectedComponentSplitStats stats) {
            context->nextLabel = stats.nextFreeLabel;
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
    itkSignalBase *signal = allSignalList[signalIndex];
    const auto groupIt = layerGroupVisibility.find(item->treeWidget());
    if (groupIt != layerGroupVisibility.end() && !groupIt->second.visible) {
        groupIt->second.savedLayerVisibility[signal] = isActiveIn;
        refreshLayerWidget(item->treeWidget(), item);
        return;
    }

    signal->setIsActive(isActiveIn);

    refreshAfterVisibilityChange();

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

    orthoViewer->refreshViewers(allSignalList[signalIndex]);
}


void SignalControl::addSegmentsGraph(QString fileName, volume_io::ImageLoadOptions options) {
    addSegmentsGraphAsync(fileName, {}, options);
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
    pImage->SetDirection(graphBase->pSelectedBoundary->GetDirection());
    const auto context = volume_io::derivedContext(graphBase->pSelectedBoundarySignal ? graphBase->pSelectedBoundarySignal->volumeContext : nullptr);
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
    pSignal2->volumeContext = context;
    size_t signalIndexGlobal = allSignalList.size();
    itkSignalBase *pRaw = pSignal2.get();
    ownedSignals.push_back(std::move(pSignal2));
    allSignalList.push_back(pRaw);

    graphBase->ignoredSegmentLabels.clear();
    graphBase->edgeStatus.clear();
    graphBase->colorLookUpEdgesStatus.clear();
    graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<GraphSegmentType>::fromImage(pImage);
    graphBase->pGraph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    prepareWorkingSegmentsGraph(pImage);
    dynamic_cast<itkSignal<GraphSegmentType> *>(segmentsGraph)->updateStorage(graphBase->pWorkingSegmentsImage);
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
                                                        graphBase->ROI_fz, graphBase->ROI_tz, graphBase->pSelectedBoundarySignal->volumeContext);
        myMainWindow->show();
    } else {
        myMainWindow->myWatershedControl->addBoundaries(graphBase->pSelectedBoundary, graphBase->pSelectedBoundarySignal->volumeContext);
        myMainWindow->show();

    }

    setROISelectionActive(false);
}

void SignalControl::receiveNewRefinement(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage, std::shared_ptr<volume_io::VolumeContext> context) {
    size_t signalIndexGlobal;
    bool loadSuccessFull = insertImageSegmenttype(pImage, signalIndexGlobal, true, std::move(context));
    if (loadSuccessFull) {
        auto *typedSignal = dynamic_cast<itkSignal<GraphSegmentType>*>(allSignalList[signalIndexGlobal]);
        allSignalList[signalIndexGlobal]->setCategoricalColorMode();
        allSignalList[signalIndexGlobal]->setName(
            signal_name_utils::makeUniqueSignalName(allSignalList, QStringLiteral("Refinement")));
        allSignalList[signalIndexGlobal]->setupTreeWidget(refinementTreeWidget, signalIndexGlobal);
        allSignalList[signalIndexGlobal]->setIsActive(false);
        allSignalList[signalIndexGlobal]->setValueColorToTransparent(0);
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

void SignalControl::importGeneratedSegments(GraphSegmentImageType::Pointer pImage,
                                            const QString &name, std::shared_ptr<volume_io::VolumeContext> context) {
    if (pImage == nullptr) {
        showInfoMessage("No watershed segments available for import.");
        return;
    }

    prepareWorkingSegmentsGraph(pImage);
    size_t signalIndexGlobal = 0;
    if (!insertImageSegmenttype(graphBase->pWorkingSegmentsImage, signalIndexGlobal, false, std::move(context))) {
        showInfoMessage("Failed to import generated segments.");
        return;
    }

    registerSegmentsGraphSignal(signalIndexGlobal);
    allSignalList[signalIndexGlobal]->setName(signal_name_utils::makeUniqueSignalName(allSignalList, name));
    if (QTreeWidgetItem *item = findSignalTreeItem(signalTreeWidget, signalIndexGlobal)) {
        refreshLayerWidget(signalTreeWidget, item);
    }
}


void SignalControl::loadMembraneProbability(QString fileName, QString displayedName, volume_io::ImageLoadOptions options) {
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
        loadMembraneProbabilityAsync(fileName, displayedName, BoundaryLoadMode::BoundaryOnly, floatConversionMode, {}, options);
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
            floatConversionMode, {}, options);
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
            }, options);
    }
}


void SignalControl::createEmptySegmentation() {
    SP_LOG_INFO("segmentation", QStringLiteral("Creating an empty segmentation volume"));
    if (!hasWorkingSegments()) {
        showInfoMessage("Please load supervoxels first.");
        return;
    }

    dataType::SegmentsStorage::Pointer pImage = segment_puzzler::StreamVByteImageStorage<GraphSegmentType>::create(
        *graphBase->pWorkingSegmentsImage->imageInformation());
    const auto context = volume_io::derivedContext(graphBase->pWorkingSegments ? graphBase->pWorkingSegments->volumeContext : nullptr);

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
    pSignal2->volumeContext = context;
    pSignal2->initialEmptyMTime = pImage->GetMTime();
    auto *typedSignal = pSignal2.get();
    size_t signalIndexGlobal = allSignalList.size();
    allSignalList.push_back(typedSignal);
    ownedSignals.push_back(std::move(pSignal2));

    allSignalList[signalIndexGlobal]->setCategoricalColorMode();
    allSignalList[signalIndexGlobal]->setValueColorToTransparent(0);
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
    QString path;
    volume_io::ImageLoadOptions options;
    if (chooseVolumeFile(tr("Load Segmentation"), path, options)) loadSegmentationVolume(path, {}, {}, options);
}

void SignalControl::addSegmentsPressed() {
    if (hasWorkingSegments()) {
        showInfoMessage("Supervoxels are already loaded, please restart SegmentPuzzler for a new project.");
        return;
    }
    QString fileName;
    volume_io::ImageLoadOptions options;
    if (chooseVolumeFile(tr("Load Supervoxels"), fileName, options)) {
        askForBackgroundStrategy();
        addSegmentsGraph(fileName, options);
    }
}


void SignalControl::loadRefinementPressed() {
    QString path;
    volume_io::ImageLoadOptions options;
    if (chooseVolumeFile(tr("Load Refinement"), path, options)) loadRefinement(path, {}, options);
}

void SignalControl::mergeSupervoxelsByRefinementLabel() {
    if (!hasSelectedRefinement()) {
        showInfoMessage("Please load a refinement first.");
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Merging supervoxels that share a refinement label..."),
        [this]() { graphBase->pGraph->mergeSegmentsWithRefinement(); },
        [this]() {
            graphBase->rebuildEdgeColorPresentation();
            refreshViewers();
        });
}

void SignalControl::loadRefinement(QString fileName) {
    loadRefinement(fileName, "");
}

void SignalControl::loadRefinement(QString fileName, QString displayedName, volume_io::ImageLoadOptions options) {
    loadRefinementAsync(fileName, displayedName, {}, options);
}

void SignalControl::askForBackgroundStrategy() {
    QBackgroundIdRadioBox dialog;
    dialog.exec();
    graphBase->pGraph->setBackgroundIdStrategy(dialog.getStrategy().toStdString());
}

bool SignalControl::insertImageSegmenttype(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage,
                                           size_t &signalIndexGlobalOut,
                                           bool forceShapeOfSegments, std::shared_ptr<volume_io::VolumeContext> context) {
    if (!pImage) return false;
    return insertImageSegmenttype(segment_puzzler::storeLabels(*pImage), signalIndexGlobalOut, forceShapeOfSegments, std::move(context));
}

void SignalControl::addImagePressed() {
    QString path;
    volume_io::ImageLoadOptions options;
    if (chooseVolumeFile(tr("Load Image"), path, options)) addImage(path, {}, options);
}


void SignalControl::loadMembraneProbabilityPressed() {
    QString path;
    volume_io::ImageLoadOptions options;
    if (chooseVolumeFile(tr("Load Boundaries"), path, options)) loadMembraneProbability(path, {}, options);
}

bool SignalControl::chooseVolumeFile(const QString &title, QString &path, volume_io::ImageLoadOptions &options) {
    VolumeLoadDialog dialog(this, title, QSettings().value("default_load_dir").toString(), cropLoadDefaults);
    if (dialog.exec() != QDialog::Accepted) return false;
    path = dialog.selectedFiles().front();
    options = dialog.loadOptions();
    QSettings().setValue("default_load_dir", QFileInfo(path).absolutePath());
    return true;
}

bool SignalControl::prepareLoadOptions(const QString &path, volume_io::ImageLoadOptions &options) {
    return prepareVolumeLoad(this, path, options);
}

std::shared_ptr<SignalControl::LoadCallback> SignalControl::loadCompletion(
    const volume_io::ImageLoadOptions &options, LoadCallback then) {
    return std::make_shared<LoadCallback>([this, options, then = std::move(then)](LoadResult result) {
        if (result) {
            cropLoadDefaults.enabled = options.crop.has_value();
            if (options.crop) cropLoadDefaults.region = options.crop;
        }
        if (then) then(result);
    });
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

bool SignalControl::insertImageSegmenttype(dataType::SegmentsStorage::Pointer image,
    size_t &signalIndexGlobalOut, bool forceShapeOfSegments, std::shared_ptr<volume_io::VolumeContext> context) {
    return insertTypedImage<dataType::SegmentIdType>(image, signalIndexGlobalOut, forceShapeOfSegments, std::move(context));
}
