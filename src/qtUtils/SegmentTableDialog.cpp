#include "SegmentTableDialog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStringList>
#include <QTableView>
#include <QTextStream>
#include <QVBoxLayout>

#include <itkChangeInformationImageFilter.h>
#include <itkContinuousIndex.h>
#include <itkExtractImageFilter.h>
#include <itkLabelImageToShapeLabelMapFilter.h>

#include "src/segment_handling/Graph.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/utils/AppLogger.h"
#include "src/utils/ExportPathUtils.h"
#include "src/viewers/Segment3DViewerDialog.h"
#include "src/viewers/OrthoViewer.h"

// ---- helpers ----------------------------------------------------------------

namespace {

constexpr char kSettingsGroup[] = "SegmentFeatureTable";
using FeatureBoolMember = bool SegmentTableDialog::FeatureFlags::*;
const std::array<std::pair<const char *, FeatureBoolMember>, 17> kFeatureBoolSettings{{
    {"volume", &SegmentTableDialog::FeatureFlags::volume},
    {"isIsolated", &SegmentTableDialog::FeatureFlags::isIsolated},
    {"physicalSize", &SegmentTableDialog::FeatureFlags::physicalSize},
    {"pixelsOnBorder", &SegmentTableDialog::FeatureFlags::pixelsOnBorder},
    {"overridePixelSize", &SegmentTableDialog::FeatureFlags::overridePixelSize},
    {"perimeterOnBorder", &SegmentTableDialog::FeatureFlags::perimeterOnBorder},
    {"centroid", &SegmentTableDialog::FeatureFlags::centroid},
    {"bbox", &SegmentTableDialog::FeatureFlags::bbox},
    {"elongation", &SegmentTableDialog::FeatureFlags::elongation},
    {"flatness", &SegmentTableDialog::FeatureFlags::flatness},
    {"roundness", &SegmentTableDialog::FeatureFlags::roundness},
    {"equivSphRadius", &SegmentTableDialog::FeatureFlags::equivSphRadius},
    {"equivSphPerimeter", &SegmentTableDialog::FeatureFlags::equivSphPerimeter},
    {"equivEllipsoid", &SegmentTableDialog::FeatureFlags::equivEllipsoid},
    {"principalMoments", &SegmentTableDialog::FeatureFlags::principalMoments},
    {"perimeter", &SegmentTableDialog::FeatureFlags::perimeter},
    {"orientedBBox", &SegmentTableDialog::FeatureFlags::orientedBBox},
}};

QString physicalUnitLabel(const QString &unitId) {
    return unitId == QStringLiteral("um") ? QStringLiteral("µm") : unitId;
}

// t=0 → green, t=0.5 → yellow, t=1 → red (HSV hue 120°→0°)
QColor colorForNormalizedValue(double t) {
    const float hue = static_cast<float>(0.333 * (1.0 - t));
    return QColor::fromHsvF(hue, 0.80f, 0.88f);
}

// Returns black or white depending on the perceived luminance of bg.
QColor textColorForBackground(const QColor &bg) {
    const double luminance = 0.299 * bg.red() + 0.587 * bg.green() + 0.114 * bg.blue();
    return luminance > 128.0 ? Qt::black : Qt::white;
}

// Toolbar badge that shows the min→max color scale.
class ColorScaleLegend : public QWidget {
public:
    explicit ColorScaleLegend(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(160, 22);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        constexpr int lblW = 26;
        constexpr int pad  = 3;
        const QRect bar(lblW + pad, 2, width() - 2 * (lblW + pad), height() - 4);

        QLinearGradient grad(bar.left(), 0, bar.right(), 0);
        for (int i = 0; i <= 20; ++i) {
            grad.setColorAt(i / 20.0, colorForNormalizedValue(i / 20.0));
        }
        p.fillRect(bar, grad);
        p.setPen(QColor(100, 100, 100));
        p.drawRect(bar);

        QFont f = p.font();
        f.setPointSize(qMax(f.pointSize() - 1, 7));
        p.setFont(f);
        p.setPen(palette().windowText().color());
        p.drawText(QRect(0, 0, lblW, height()), Qt::AlignCenter, "min");
        p.drawText(QRect(width() - lblW, 0, lblW, height()), Qt::AlignCenter, "max");
    }
};

// -1.0 sentinel → display "-"; any non-negative value → right-aligned number.
QStandardItem *makeNumericItem(double v, int decimals = 3) {
    auto *item = new QStandardItem();
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (v < 0.0) {
        item->setText("-");
        item->setData(QVariant(-1.0), Qt::UserRole);
    } else {
        item->setText(QString::number(v, 'f', decimals));
        item->setData(QVariant(v), Qt::UserRole);
    }
    return item;
}

QStandardItem *makeBooleanItem(bool value) {
    auto *item = new QStandardItem(value ? "Yes" : "No");
    item->setTextAlignment(Qt::AlignCenter);
    item->setData(QVariant(value ? 1.0 : 0.0), Qt::UserRole);
    return item;
}

std::unordered_map<dataType::SegmentIdType, bool> computeIsolationByLabel(
    dataType::SegmentsImageType::Pointer segImage) {
    std::unordered_map<dataType::SegmentIdType, bool> isolationByLabel;
    if (segImage == nullptr) {
        return isolationByLabel;
    }

    const auto size = segImage->GetLargestPossibleRegion().GetSize();
    const size_t dimX = size[0];
    const size_t dimY = size[1];
    const size_t dimZ = size[2];
    const size_t planeXY = dimX * dimY;
    const auto *buffer = segImage->GetBufferPointer();

    for (size_t z = 0; z < dimZ; ++z) {
        for (size_t y = 0; y < dimY; ++y) {
            for (size_t x = 0; x < dimX; ++x) {
                const size_t index = x + y * dimX + z * planeXY;
                const dataType::SegmentIdType label = buffer[index];
                if (label == 0) {
                    continue;
                }

                isolationByLabel.try_emplace(label, true);

                const auto markNonIsolatedPair = [&](size_t neighborIndex) {
                    const dataType::SegmentIdType neighborLabel = buffer[neighborIndex];
                    if (neighborLabel == 0 || neighborLabel == label) {
                        return;
                    }
                    isolationByLabel[label] = false;
                    isolationByLabel[neighborLabel] = false;
                };

                if (x + 1 < dimX) {
                    markNonIsolatedPair(index + 1);
                }
                if (y + 1 < dimY) {
                    markNonIsolatedPair(index + dimX);
                }
                if (z + 1 < dimZ) {
                    markNonIsolatedPair(index + planeXY);
                }
            }
        }
    }

    return isolationByLabel;
}

template<typename ImageType>
std::vector<SegmentTableDialog::SegmentRow> computeShapeFeatureRows(
    typename ImageType::Pointer image,
    const SegmentTableDialog::FeatureFlags &flags) {
    static_assert(ImageType::ImageDimension == 2 || ImageType::ImageDimension == 3);

    using FilterType = itk::LabelImageToShapeLabelMapFilter<ImageType>;
    using LabelObjectType = typename FilterType::LabelObjectType;
    auto filter = FilterType::New();
    filter->SetInput(image);
    // Roundness is computed inside the perimeter pass.
    filter->SetComputePerimeter(flags.perimeter || flags.roundness);
    filter->SetComputeOrientedBoundingBox(flags.orientedBBox);
    const itk::ThreadIdType nThreads = itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads();
    filter->SetNumberOfWorkUnits(nThreads);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("Running %1D LabelImageToShapeLabelMapFilter with %2 ITK work units")
            .arg(ImageType::ImageDimension)
            .arg(nThreads));
    filter->Update();

    auto *labelMap = filter->GetOutput();
    const auto labelCount = labelMap->GetNumberOfLabelObjects();
    std::vector<SegmentTableDialog::SegmentRow> rows;
    rows.reserve(labelCount);

    const auto region = image->GetLargestPossibleRegion();
    const auto regionStart = region.GetIndex();
    const auto regionSize = region.GetSize();
    const long long borderDistance = std::max(0, flags.borderDistancePx);

    for (unsigned int i = 0; i < labelCount; ++i) {
        const auto *labelObject = labelMap->GetNthLabelObject(i);
        SegmentTableDialog::SegmentRow row;
        row.label = labelObject->GetLabel();

        const auto centroid = labelObject->GetCentroid();
        itk::ContinuousIndex<double, ImageType::ImageDimension> continuousIndex;
        [[maybe_unused]] const bool inside =
            image->TransformPhysicalPointToContinuousIndex(centroid, continuousIndex);
        row.centroidX = static_cast<int>(std::round(continuousIndex[0]));
        row.centroidY = static_cast<int>(std::round(continuousIndex[1]));
        if constexpr (ImageType::ImageDimension == 3) {
            row.centroidZ = static_cast<int>(std::round(continuousIndex[2]));
        }

        if (flags.volume) {
            row.volume = static_cast<double>(labelObject->GetNumberOfPixels());
        }
        if (flags.physicalSize) {
            row.physicalSize = labelObject->GetPhysicalSize();
        }
        if (flags.pixelsOnBorder) {
            std::size_t count = 0;
            typename LabelObjectType::ConstIndexIterator indexIt(labelObject);
            while (!indexIt.IsAtEnd()) {
                const auto &index = indexIt.GetIndex();
                for (unsigned int dimension = 0; dimension < ImageType::ImageDimension; ++dimension) {
                    const long long distanceFromMinimum =
                        static_cast<long long>(index[dimension]) -
                        static_cast<long long>(regionStart[dimension]);
                    const long long distanceFromMaximum =
                        static_cast<long long>(regionSize[dimension]) - 1 - distanceFromMinimum;
                    if (distanceFromMinimum <= borderDistance ||
                        distanceFromMaximum <= borderDistance) {
                        ++count;
                        break;
                    }
                }
                ++indexIt;
            }
            row.pixelsOnBorder = static_cast<double>(count);
        }
        if (flags.perimeterOnBorder) {
            row.perimeterOnBorder = labelObject->GetPerimeterOnBorder();
        }

        if (flags.bbox) {
            const auto bbox = labelObject->GetBoundingBox();
            row.bboxW = static_cast<double>(bbox.GetSize()[0]);
            row.bboxH = static_cast<double>(bbox.GetSize()[1]);
            if constexpr (ImageType::ImageDimension == 3) {
                row.bboxD = static_cast<double>(bbox.GetSize()[2]);
            }
        }

        if (flags.elongation) {
            row.elongation = labelObject->GetElongation();
        }
        if (flags.flatness) {
            row.flatness = labelObject->GetFlatness();
        }
        if (flags.roundness) {
            row.roundness = labelObject->GetRoundness();
        }
        if (flags.equivSphRadius) {
            row.equivSphRadius = labelObject->GetEquivalentSphericalRadius();
        }
        if (flags.equivSphPerimeter) {
            row.equivSphPerimeter = labelObject->GetEquivalentSphericalPerimeter();
        }

        if (flags.equivEllipsoid) {
            const auto diameters = labelObject->GetEquivalentEllipsoidDiameter();
            row.equivEllipD0 = diameters[0];
            row.equivEllipD1 = diameters[1];
            if constexpr (ImageType::ImageDimension == 3) {
                row.equivEllipD2 = diameters[2];
            }
        }
        if (flags.principalMoments) {
            const auto moments = labelObject->GetPrincipalMoments();
            row.principalMom0 = moments[0];
            row.principalMom1 = moments[1];
            if constexpr (ImageType::ImageDimension == 3) {
                row.principalMom2 = moments[2];
            }
        }

        if (flags.perimeter) {
            row.perimeter = labelObject->GetPerimeter();
        }

        if (flags.orientedBBox) {
            const auto size = labelObject->GetOrientedBoundingBoxSize();
            row.obboxW = size[0];
            row.obboxH = size[1];
            if constexpr (ImageType::ImageDimension == 2) {
                row.obboxVolume = size[0] * size[1];
            } else {
                row.obboxD = size[2];
                row.obboxVolume = size[0] * size[1] * size[2];
            }
        }

        rows.push_back(row);
    }

    return rows;
}

bool segmentationSelectionMatchesCurrent(const GraphBase *graphBase,
                                         const dataType::SegmentsImageType::Pointer &currentTableSegmentation,
                                         const itkSignalBase *currentTableSegmentationSignal) {
    return graphBase != nullptr
           && currentTableSegmentation != nullptr
           && graphBase->pSelectedSegmentation == currentTableSegmentation
           && graphBase->pSelectedSegmentationSignal != nullptr
           && graphBase->pSelectedSegmentationSignal == currentTableSegmentationSignal;
}

void navigateOrthoViewerToIndex(OrthoViewer *orthoViewer,
                                const dataType::SegmentsImageType::IndexType &index) {
    if (orthoViewer == nullptr) {
        return;
    }

    if (orthoViewer->xy->isSliceIndexValid(index[2])) orthoViewer->xy->setSliceIndex(index[2]);
    if (orthoViewer->xz->isSliceIndexValid(index[1])) orthoViewer->xz->setSliceIndex(index[1]);
    if (orthoViewer->zy->isSliceIndexValid(index[0])) orthoViewer->zy->setSliceIndex(index[0]);
    orthoViewer->centerViewportsToXYZImageSpace(index[0], index[1], index[2]);
}

struct RowRemovalStats {
    std::size_t rows = 0;
    std::size_t blocks = 0;
};

RowRemovalStats removeModelRowsInDescendingBlocks(
    QStandardItemModel *model,
    std::vector<int> rows) {
    RowRemovalStats stats;
    if (model == nullptr || rows.empty()) {
        return stats;
    }

    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    int blockHigh = rows.front();
    int blockLow = blockHigh;
    const auto removeBlock = [&]() {
        const int count = blockHigh - blockLow + 1;
        if (model->removeRows(blockLow, count)) {
            stats.rows += static_cast<std::size_t>(count);
            ++stats.blocks;
        }
    };

    for (std::size_t index = 1; index < rows.size(); ++index) {
        const int row = rows[index];
        if (row == blockLow - 1) {
            blockLow = row;
            continue;
        }
        removeBlock();
        blockHigh = row;
        blockLow = row;
    }
    removeBlock();
    return stats;
}

} // namespace

// ---- construction -----------------------------------------------------------

SegmentTableDialog::SegmentTableDialog(std::shared_ptr<GraphBase> graphBaseIn,
                                       OrthoViewer *orthoViewerIn,
                                       QWidget *parent)
    : QDialog(parent)
    , graphBase(std::move(graphBaseIn))
    , orthoViewer(orthoViewerIn)
{
    setWindowTitle("Segment Feature Table");
    setWindowFlags(windowFlags()
                   | Qt::WindowMaximizeButtonHint
                   | Qt::WindowMinimizeButtonHint);
    resize(820, 640);

    stack = new QStackedWidget(this);
    stack->addWidget(createSetupPage());
    stack->addWidget(createResultsPage());
    loadSettings();
    stack->setCurrentIndex(0);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(stack);

    auto *runner = taskRunner();
    Q_ASSERT(runner != nullptr);
    if (runner != nullptr) {
        connect(runner, &TaskRunner::busyChanged, this,
                [this](bool) { updateResultsActionState(); });
    }
}

// ---- page builders ----------------------------------------------------------

QWidget *SegmentTableDialog::createSetupPage() {
    auto *page   = new QWidget();
    auto *outer  = new QVBoxLayout(page);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(10);

    outer->addWidget(new QLabel("<b>Select features to compute</b>"));

    // Scrollable group area
    auto *scroll   = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content  = new QWidget();
    auto *vContent = new QVBoxLayout(content);
    vContent->setSpacing(10);

    // Helper: create a group box with a 2-column QGridLayout.
    const auto makeGroup = [](const QString &title, QGridLayout *&gridOut) -> QGroupBox * {
        auto *gb   = new QGroupBox(title);
        auto *grid = new QGridLayout(gb);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        gridOut = grid;
        return gb;
    };

    // --- Physical calibration ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("2D Physical Calibration", grid);
        overridePixelSizeCheckBox = new QCheckBox("Override image spacing");
        pixelSizeSpinBox = new QDoubleSpinBox();
        pixelSizeSpinBox->setRange(0.000000001, 1'000'000'000.0);
        pixelSizeSpinBox->setDecimals(9);
        pixelSizeSpinBox->setSingleStep(0.1);
        pixelSizeSpinBox->setValue(1.0);
        pixelSizeSpinBox->setKeyboardTracking(false);
        physicalUnitComboBox = new QComboBox();
        physicalUnitComboBox->addItem("nm", QStringLiteral("nm"));
        physicalUnitComboBox->addItem(QStringLiteral("µm"), QStringLiteral("um"));
        physicalUnitComboBox->addItem("mm", QStringLiteral("mm"));
        physicalUnitComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        physicalUnitComboBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        physicalUnitComboBox->setMinimumWidth(72);
        physicalUnitComboBox->setMaxVisibleItems(3);
        physicalUnitComboBox->setStyleSheet(
            "QComboBox { combobox-popup: 0; }"
            "QComboBox QAbstractItemView { padding: 0px; margin: 0px; outline: 0; }"
            "QComboBox QAbstractItemView::item { margin: 0px; padding: 2px 6px; min-height: 0px; }");
        auto *unitView = new QListView(physicalUnitComboBox);
        unitView->setUniformItemSizes(true);
        unitView->setSpacing(0);
        unitView->setMinimumWidth(72);
        physicalUnitComboBox->setView(unitView);

        auto *valueWidget = new QWidget(gb);
        auto *valueLayout = new QHBoxLayout(valueWidget);
        valueLayout->setContentsMargins(0, 0, 0, 0);
        valueLayout->addWidget(pixelSizeSpinBox, 1);
        valueLayout->addWidget(physicalUnitComboBox);

        const QString calibrationToolTip =
            "Overrides X/Y spacing only for this 2D feature calculation. "
            "The loaded segmentation is not modified.";
        overridePixelSizeCheckBox->setToolTip(calibrationToolTip);
        pixelSizeSpinBox->setToolTip(calibrationToolTip);
        physicalUnitComboBox->setToolTip(calibrationToolTip);

        grid->addWidget(overridePixelSizeCheckBox, 0, 0, 1, 2);
        grid->addWidget(new QLabel("Physical pixel size:"), 1, 0);
        grid->addWidget(valueWidget, 1, 1);
        vContent->addWidget(gb);

        connect(overridePixelSizeCheckBox, &QCheckBox::toggled,
                this, &SegmentTableDialog::updateCalibrationControls);
        updateCalibrationControls();
    }

    // --- Basic Measurements ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("Basic Measurements", grid);
        cbVolume            = new QCheckBox("Pixel / voxel count");              cbVolume->setChecked(true);
        cbIsIsolated        = new QCheckBox("Is Isolated");                      cbIsIsolated->setChecked(true);
        cbPhysicalSize      = new QCheckBox("Physical Size");                    cbPhysicalSize->setChecked(false);
        cbPixelsOnBorder    = new QCheckBox("Pixels on Border");                 cbPixelsOnBorder->setChecked(false);
        cbPerimeterOnBorder = new QCheckBox("Perimeter on Border (physical)");   cbPerimeterOnBorder->setChecked(false);
        auto *borderDistanceLabel = new QLabel("Pixels on Border distance:");
        borderDistanceSpinBox = new QSpinBox();
        borderDistanceSpinBox->setRange(0, 1'000'000);
        borderDistanceSpinBox->setValue(0);
        borderDistanceSpinBox->setSuffix(" px");
        borderDistanceSpinBox->setToolTip(
            "Used by Pixels on Border. 0 counts only the outermost pixels; "
            "N also counts pixels up to and including N pixels inward.");
        grid->addWidget(cbVolume,            0, 0);
        grid->addWidget(cbIsIsolated,        0, 1);
        grid->addWidget(cbPhysicalSize,      1, 0);
        grid->addWidget(borderDistanceLabel, 2, 0);
        grid->addWidget(borderDistanceSpinBox, 2, 1);
        grid->addWidget(cbPixelsOnBorder,    3, 0);
        grid->addWidget(cbPerimeterOnBorder, 3, 1);
        vContent->addWidget(gb);
    }

    // --- Position & Extent ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("Position & Extent", grid);
        cbCentroid = new QCheckBox("Centroid (voxel coordinates)");          cbCentroid->setChecked(true);
        cbBBox     = new QCheckBox("Axis-Aligned Bounding Box");              cbBBox->setChecked(false);
        grid->addWidget(cbCentroid, 0, 0);
        grid->addWidget(cbBBox,     1, 0);
        vContent->addWidget(gb);
    }

    // --- Shape Descriptors ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("Shape Descriptors", grid);
        cbElongation        = new QCheckBox("Elongation");                          cbElongation->setChecked(true);
        cbFlatness          = new QCheckBox("Flatness");                            cbFlatness->setChecked(true);
        cbRoundness         = new QCheckBox("Roundness");                           cbRoundness->setChecked(true);
        cbEquivSphRadius    = new QCheckBox("Equiv. Circle / Spherical Radius");    cbEquivSphRadius->setChecked(true);
        cbEquivSphPerimeter = new QCheckBox("Equiv. Circle / Spherical Perimeter"); cbEquivSphPerimeter->setChecked(false);
        cbEquivEllipsoid    = new QCheckBox("Equiv. Ellipsoid Diameters");          cbEquivEllipsoid->setChecked(false);
        cbPrincipalMoments  = new QCheckBox("Principal Moments");                   cbPrincipalMoments->setChecked(false);
        grid->addWidget(cbElongation,        0, 0);
        grid->addWidget(cbFlatness,          0, 1);
        grid->addWidget(cbRoundness,         1, 0);
        grid->addWidget(cbEquivSphRadius,    1, 1);
        grid->addWidget(cbEquivSphPerimeter, 2, 0);
        grid->addWidget(cbEquivEllipsoid,    2, 1);
        grid->addWidget(cbPrincipalMoments,  3, 0);
        vContent->addWidget(gb);
    }

    // --- Advanced (slower) ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("Advanced (slower)", grid);
        cbPerimeter    = new QCheckBox("Perimeter / Surface Area");                cbPerimeter->setChecked(false);
        cbOrientedBBox = new QCheckBox("Oriented Bounding Box (size + area/volume)"); cbOrientedBBox->setChecked(false);
        grid->addWidget(cbPerimeter,    0, 0);
        grid->addWidget(cbOrientedBBox, 1, 0);
        vContent->addWidget(gb);
    }

    vContent->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    computeButton = new QPushButton("Compute →");
    computeButton->setDefault(true);
    auto *selectAllButton  = new QPushButton("Select All");
    auto *selectNoneButton = new QPushButton("Select None");
    auto *resetDefaultsButton = new QPushButton("Reset Defaults");
    auto *btnRow = new QHBoxLayout();
    btnRow->addWidget(selectAllButton);
    btnRow->addWidget(selectNoneButton);
    btnRow->addWidget(resetDefaultsButton);
    btnRow->addStretch();
    btnRow->addWidget(computeButton);
    outer->addLayout(btnRow);

    connect(selectAllButton,  &QPushButton::clicked, this, [this]{ setAllChecked(true);  });
    connect(selectNoneButton, &QPushButton::clicked, this, [this]{ setAllChecked(false); });
    connect(resetDefaultsButton, &QPushButton::clicked,
            this, &SegmentTableDialog::resetSettingsToDefaults);
    connect(computeButton, &QPushButton::clicked,
            this, &SegmentTableDialog::onComputeClicked);

    return page;
}

QWidget *SegmentTableDialog::createResultsPage() {
    auto *page  = new QWidget();
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    // Toolbar
    backButton           = new QPushButton("← Back");
    recomputeButton      = new QPushButton("Recompute");
    recomputeButton->setEnabled(false);
    deleteSelectedButton = new QPushButton("Delete Selected");
    deleteSelectedButton->setEnabled(false);
    mergeWithNeighborButton = new QPushButton("Merge with Neighbor");
    mergeWithNeighborButton->setObjectName(QStringLiteral("mergeWithNeighborButton"));
    mergeWithNeighborButton->setEnabled(false);
    view3DButton = new QPushButton("View 3D");
    view3DButton->setEnabled(false);
    exportCsvButton = new QPushButton("Export CSV");
    exportCsvButton->setEnabled(false);
    auto *toolbar = new QHBoxLayout();
    toolbar->addWidget(backButton);
    toolbar->addStretch();
    toolbar->addWidget(new ColorScaleLegend());
    toolbar->addStretch();
    toolbar->addWidget(recomputeButton);
    toolbar->addWidget(deleteSelectedButton);
    toolbar->addWidget(mergeWithNeighborButton);
    toolbar->addWidget(view3DButton);
    toolbar->addWidget(exportCsvButton);
    outer->addLayout(toolbar);

    // Filter bar
    searchEdit = new QLineEdit();
    searchEdit->setPlaceholderText("Filter by label ID...");
    searchEdit->setClearButtonEnabled(true);
    outer->addWidget(searchEdit);

    // Table model with all possible columns
    model = new QStandardItemModel(this);
    model->setColumnCount(SegmentTableDialog::COL_COUNT);
    model->setHorizontalHeaderLabels({
        "Label",
        "Volume", "Isolated", "Physical Size", "Px on Border", "Perim on Border",
        "CX", "CY", "CZ",
        "BBox W", "BBox H", "BBox D",
        "Elongation", "Flatness", "Roundness",
        "Equiv Sph R", "Equiv Sph Perim",
        "Ellip D0", "Ellip D1", "Ellip D2",
        "PrinMom 0", "PrinMom 1", "PrinMom 2",
        "Perimeter",
        "OBBox W", "OBBox H", "OBBox D", "OBBox Vol"
    });

    sortModel = new QSortFilterProxyModel(this);
    sortModel->setSourceModel(model);
    sortModel->setSortRole(Qt::UserRole);
    sortModel->setFilterKeyColumn(SegmentTableDialog::COL_LABEL);
    sortModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

    tableView = new QTableView();
    tableView->setModel(sortModel);
    tableView->setSortingEnabled(true);
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView->horizontalHeader()->setSortIndicatorShown(true);
    tableView->horizontalHeader()->setStretchLastSection(false);
    tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tableView->verticalHeader()->hide();

    statusLabel = new QLabel();
    outer->addWidget(tableView, 1);
    outer->addWidget(statusLabel);

    connect(backButton,          &QPushButton::clicked, this, &SegmentTableDialog::onBackClicked);
    connect(recomputeButton,     &QPushButton::clicked, this, &SegmentTableDialog::onComputeClicked);
    connect(deleteSelectedButton,&QPushButton::clicked, this, &SegmentTableDialog::onDeleteSelectedClicked);
    connect(mergeWithNeighborButton, &QPushButton::clicked,
            this, &SegmentTableDialog::onMergeWithNeighborClicked);
    connect(view3DButton,        &QPushButton::clicked, this, &SegmentTableDialog::onView3DSelectedClicked);
    connect(exportCsvButton,     &QPushButton::clicked, this, &SegmentTableDialog::onExportCsvClicked);
    connect(searchEdit, &QLineEdit::textChanged,
            sortModel,  &QSortFilterProxyModel::setFilterFixedString);
    connect(tableView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &SegmentTableDialog::onSelectionChanged);
    connect(tableView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection &, const QItemSelection &) {
                updateResultsActionState();
            });

    return page;
}

void SegmentTableDialog::setDeleteSegmentationLabelsHandler(
    DeleteSegmentationLabelsHandler handler)
{
    deleteSegmentationLabelsHandler = std::move(handler);
    updateResultsActionState();
}

// ---- setup page actions -----------------------------------------------------

SegmentTableDialog::FeatureFlags SegmentTableDialog::collectFlags() const {
    FeatureFlags f;
    f.volume            = cbVolume->isChecked();
    f.isIsolated        = cbIsIsolated->isChecked();
    f.physicalSize      = cbPhysicalSize->isChecked();
    f.pixelsOnBorder    = cbPixelsOnBorder->isChecked();
    f.borderDistancePx  = borderDistanceSpinBox->value();
    f.overridePixelSize = overridePixelSizeCheckBox->isChecked();
    f.pixelSize         = pixelSizeSpinBox->value();
    f.physicalUnit      = physicalUnitComboBox->currentData().toString();
    f.perimeterOnBorder = cbPerimeterOnBorder->isChecked();
    f.centroid          = cbCentroid->isChecked();
    f.bbox              = cbBBox->isChecked();
    f.elongation        = cbElongation->isChecked();
    f.flatness          = cbFlatness->isChecked();
    f.roundness         = cbRoundness->isChecked();
    f.equivSphRadius    = cbEquivSphRadius->isChecked();
    f.equivSphPerimeter = cbEquivSphPerimeter->isChecked();
    f.equivEllipsoid    = cbEquivEllipsoid->isChecked();
    f.principalMoments  = cbPrincipalMoments->isChecked();
    f.perimeter         = cbPerimeter->isChecked();
    f.orientedBBox      = cbOrientedBBox->isChecked();
    return f;
}

void SegmentTableDialog::applyFlagsToUi(const FeatureFlags &flags) {
    cbVolume->setChecked(flags.volume);
    cbIsIsolated->setChecked(flags.isIsolated);
    cbPhysicalSize->setChecked(flags.physicalSize);
    cbPixelsOnBorder->setChecked(flags.pixelsOnBorder);
    borderDistanceSpinBox->setValue(flags.borderDistancePx);
    overridePixelSizeCheckBox->setChecked(flags.overridePixelSize);
    pixelSizeSpinBox->setValue(flags.pixelSize);
    const int unitIndex = physicalUnitComboBox->findData(flags.physicalUnit);
    physicalUnitComboBox->setCurrentIndex(unitIndex >= 0 ? unitIndex : 0);
    cbPerimeterOnBorder->setChecked(flags.perimeterOnBorder);
    cbCentroid->setChecked(flags.centroid);
    cbBBox->setChecked(flags.bbox);
    cbElongation->setChecked(flags.elongation);
    cbFlatness->setChecked(flags.flatness);
    cbRoundness->setChecked(flags.roundness);
    cbEquivSphRadius->setChecked(flags.equivSphRadius);
    cbEquivSphPerimeter->setChecked(flags.equivSphPerimeter);
    cbEquivEllipsoid->setChecked(flags.equivEllipsoid);
    cbPrincipalMoments->setChecked(flags.principalMoments);
    cbPerimeter->setChecked(flags.perimeter);
    cbOrientedBBox->setChecked(flags.orientedBBox);
    updateCalibrationControls();
}

void SegmentTableDialog::loadSettings() {
    FeatureFlags flags;
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    for (const auto &[key, member] : kFeatureBoolSettings) {
        flags.*member = settings.value(key, flags.*member).toBool();
    }
    flags.borderDistancePx =
        settings.value(QStringLiteral("borderDistancePx"), flags.borderDistancePx).toInt();
    flags.pixelSize = settings.value(QStringLiteral("pixelSize"), flags.pixelSize).toDouble();
    flags.physicalUnit =
        settings.value(QStringLiteral("physicalUnit"), flags.physicalUnit).toString();
    settings.endGroup();

    if (!std::isfinite(flags.pixelSize) || flags.pixelSize <= 0.0) {
        flags.pixelSize = FeatureFlags{}.pixelSize;
    }
    applyFlagsToUi(flags);
}

void SegmentTableDialog::saveSettings(const FeatureFlags &flags) const {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    for (const auto &[key, member] : kFeatureBoolSettings) {
        settings.setValue(key, flags.*member);
    }
    settings.setValue(QStringLiteral("borderDistancePx"), flags.borderDistancePx);
    settings.setValue(QStringLiteral("pixelSize"), flags.pixelSize);
    settings.setValue(QStringLiteral("physicalUnit"), flags.physicalUnit);
    settings.endGroup();
}

void SegmentTableDialog::resetSettingsToDefaults() {
    QSettings settings;
    settings.remove(kSettingsGroup);
    applyFlagsToUi(FeatureFlags{});
}

void SegmentTableDialog::updateCalibrationControls() {
    const bool enabled = overridePixelSizeCheckBox != nullptr
                         && overridePixelSizeCheckBox->isChecked();
    if (pixelSizeSpinBox != nullptr) {
        pixelSizeSpinBox->setEnabled(enabled);
    }
    if (physicalUnitComboBox != nullptr) {
        physicalUnitComboBox->setEnabled(enabled);
    }
}

void SegmentTableDialog::setQuickComputeMode() {
    setAllChecked(false);
    cbVolume->setChecked(true);
    cbIsIsolated->setChecked(true);
    cbCentroid->setChecked(true);
}

void SegmentTableDialog::startCompute(dataType::SegmentsImageType::Pointer segImg) {
    auto *runner = taskRunner();
    if (runner == nullptr || runner->isBusy()) {
        return;
    }
    if (segImg == nullptr) {
        segImg = graphBase->pSelectedSegmentation;
    }

    SP_LOG_DEBUG(
        "segmentation",
        QStringLiteral("Segment table startCompute requestedSegmentation=%1 selectedSegmentation=%2 "
                       "selectedSegmentationSignal=%3 selectedSegmentationMaxId=%4")
            .arg(reinterpret_cast<quintptr>(segImg.GetPointer()), 0, 16)
            .arg(reinterpret_cast<quintptr>(graphBase->pSelectedSegmentation.GetPointer()), 0, 16)
            .arg(reinterpret_cast<quintptr>(graphBase->pSelectedSegmentationSignal), 0, 16)
            .arg(graphBase->selectedSegmentationMaxSegmentId));

    if (segImg == nullptr) {
        QMessageBox::information(this, "No Segmentation",
                                 "No segmentation is currently selected.\n"
                                 "Load and select a segmentation first.");
        return;
    }

    const FeatureFlags flags = collectFlags();
    currentTableSegmentation = segImg;
    currentTableSegmentationSignal = graphBase->pSelectedSegmentationSignal;
    tableLabelIdsAreStale = true;
    SP_LOG_DEBUG(
        "segmentation",
        QStringLiteral("Segment table featureFlags volume=%1 isIsolated=%2 centroid=%3 elongation=%4 "
                       "flatness=%5 roundness=%6 bbox=%7 physicalSize=%8 pixelsOnBorder=%9 "
                       "borderDistancePx=%10 perimeterOnBorder=%11 equivSphRadius=%12 equivSphPerimeter=%13 "
                       "equivEllipsoid=%14 principalMoments=%15 perimeter=%16 orientedBBox=%17 "
                       "overridePixelSize=%18 pixelSize=%19 physicalUnit=%20")
            .arg(flags.volume)
            .arg(flags.isIsolated)
            .arg(flags.centroid)
            .arg(flags.elongation)
            .arg(flags.flatness)
            .arg(flags.roundness)
            .arg(flags.bbox)
            .arg(flags.physicalSize)
            .arg(flags.pixelsOnBorder)
            .arg(flags.borderDistancePx)
            .arg(flags.perimeterOnBorder)
            .arg(flags.equivSphRadius)
            .arg(flags.equivSphPerimeter)
            .arg(flags.equivEllipsoid)
            .arg(flags.principalMoments)
            .arg(flags.perimeter)
            .arg(flags.orientedBBox)
            .arg(flags.overridePixelSize)
            .arg(flags.pixelSize)
            .arg(flags.physicalUnit));

    // Switch to results page immediately so the user sees the computing state.
    computeButton->setEnabled(false);
    stack->setCurrentIndex(1);
    backButton->setEnabled(false);
    recomputeButton->setEnabled(false);
    deleteSelectedButton->setEnabled(false);
    mergeWithNeighborButton->setEnabled(false);
    view3DButton->setEnabled(false);
    exportCsvButton->setEnabled(false);
    statusLabel->setText("Computing features…");

    const QPointer<SegmentTableDialog> guardedThis(this);
    const auto committed = std::make_shared<bool>(false);
    runner->runInBackground(
        QStringLiteral("Computing segment features..."),
        [segImg, flags]() {
            return SegmentTableDialog::computeFeatures(segImg, flags);
        },
        [guardedThis, committed](ComputeResult result) {
            if (guardedThis == nullptr) {
                return;
            }
            guardedThis->onComputeFinished(std::move(result));
            *committed = true;
        },
        [guardedThis, committed]() {
            if (guardedThis == nullptr || *committed) {
                return;
            }
            guardedThis->backButton->setEnabled(true);
            guardedThis->statusLabel->setText(
                QStringLiteral("Feature computation failed; please recompute."));
            guardedThis->updateResultsActionState();
        });
}

void SegmentTableDialog::onComputeClicked() {
    saveSettings(collectFlags());
    startCompute();
}

void SegmentTableDialog::onBackClicked() {
    if (isBusy()) { return; }
    computeButton->setEnabled(true);
    stack->setCurrentIndex(0);
}

void SegmentTableDialog::onDeleteSelectedClicked() {
    const QModelIndexList selected = tableView->selectionModel()->selectedRows();
    if (selected.isEmpty() || tableLabelIdsAreStale || isBusy() ||
        !deleteSegmentationLabelsHandler || graphBase == nullptr ||
        graphBase->pSelectedSegmentation == nullptr) {
        return;
    }

    if (!segmentationSelectionMatchesCurrent(graphBase.get(), currentTableSegmentation, currentTableSegmentationSignal)) {
        SP_LOG_WARNING("segmentation",
                       QStringLiteral("Segment table refusing delete because selected segmentation wiring does not match the current table segmentation"));
        QMessageBox::warning(this, "Delete Disabled",
                             "The selected segmentation state does not match the table image.\n"
                             "Please reopen Inspect Segments from the current selection.");
        return;
    }

    std::unordered_set<dataType::SegmentIdType> labelsToDelete;
    labelsToDelete.reserve(static_cast<std::size_t>(selected.size()));
    for (const QModelIndex &proxyIdx : selected) {
        const QModelIndex src = sortModel->mapToSource(proxyIdx);
        const auto *labelItem = model->item(src.row(), SegmentTableDialog::COL_LABEL);
        if (labelItem != nullptr) {
            labelsToDelete.insert(static_cast<dataType::SegmentIdType>(
                labelItem->data(Qt::UserRole).toLongLong()));
        }
    }

    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("operation=delete_selected_segments phase=begin selected_rows=%1 labels=%2")
            .arg(selected.size())
            .arg(labelsToDelete.size()));
    deleteSegmentationLabelsHandler(currentTableSegmentation, labelsToDelete);
}

void SegmentTableDialog::onMergeWithNeighborClicked() {
    if (tableView == nullptr || tableView->selectionModel() == nullptr ||
        tableLabelIdsAreStale || isBusy()) {
        return;
    }

    if (!segmentationSelectionMatchesCurrent(
            graphBase.get(), currentTableSegmentation, currentTableSegmentationSignal) ||
        graphBase->pGraph == nullptr) {
        SP_LOG_WARNING(
            "segmentation",
            QStringLiteral("Segment table refusing neighbor merge because the selected segmentation wiring does not match the current table segmentation"));
        QMessageBox::warning(this, "Merge Disabled",
                             "The selected segmentation state does not match the table image.\n"
                             "Please reopen Inspect Segments from the current selection.");
        return;
    }

    std::vector<dataType::SegmentIdType> selectedLabels;
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();
    selectedLabels.reserve(static_cast<std::size_t>(selectedRows.size()));
    for (const QModelIndex &proxyIndex : selectedRows) {
        const QModelIndex sourceIndex = sortModel->mapToSource(proxyIndex);
        const auto *labelItem = model->item(sourceIndex.row(), SegmentTableDialog::COL_LABEL);
        if (labelItem == nullptr) {
            continue;
        }
        selectedLabels.push_back(static_cast<dataType::SegmentIdType>(
            labelItem->data(Qt::UserRole).toULongLong()));
    }
    if (selectedLabels.empty()) {
        return;
    }

    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("operation=merge_with_neighbor phase=ui_request selected_count=%1")
            .arg(selectedLabels.size()));
    emit neighborMergeRequested(std::move(selectedLabels));
}

void SegmentTableDialog::setAllChecked(bool checked) {
    for (QCheckBox *cb : {cbVolume, cbIsIsolated, cbPhysicalSize, cbPixelsOnBorder, cbPerimeterOnBorder,
                          cbCentroid, cbBBox, cbElongation, cbFlatness, cbRoundness,
                          cbEquivSphRadius, cbEquivSphPerimeter, cbEquivEllipsoid,
                          cbPrincipalMoments, cbPerimeter, cbOrientedBBox}) {
        cb->setChecked(checked);
    }
}

void SegmentTableDialog::updateResultsActionState() {
    const bool busy = isBusy();
    const bool hasSelection = tableView != nullptr
                              && tableView->selectionModel() != nullptr
                              && !tableView->selectionModel()->selectedRows().isEmpty();
    const bool selectionMatches = segmentationSelectionMatchesCurrent(
        graphBase.get(), currentTableSegmentation, currentTableSegmentationSignal);
    const bool labelIdsAreCurrent = !tableLabelIdsAreStale;

    if (computeButton != nullptr) {
        computeButton->setEnabled(!busy);
    }
    if (recomputeButton != nullptr) {
        recomputeButton->setEnabled(!busy && currentTableSegmentation != nullptr);
    }

    if (deleteSelectedButton != nullptr) {
        deleteSelectedButton->setEnabled(
            hasSelection && !busy && labelIdsAreCurrent && selectionMatches &&
            static_cast<bool>(deleteSegmentationLabelsHandler));
    }
    if (mergeWithNeighborButton != nullptr) {
        mergeWithNeighborButton->setEnabled(
            hasSelection && !busy && labelIdsAreCurrent && selectionMatches && graphBase != nullptr &&
            graphBase->pGraph != nullptr && graphBase->pWorkingSegmentsImage != nullptr);
    }
    if (view3DButton != nullptr) {
        view3DButton->setEnabled(hasSelection && !busy && labelIdsAreCurrent && selectionMatches);
    }
    if (exportCsvButton != nullptr) {
        exportCsvButton->setEnabled(
            !busy && labelIdsAreCurrent && model != nullptr && model->rowCount() > 0);
    }
}

TaskRunner *SegmentTableDialog::taskRunner() const {
    return orthoViewer != nullptr ? orthoViewer->getTaskRunner() : nullptr;
}

bool SegmentTableDialog::isBusy() const {
    const auto *runner = taskRunner();
    return runner == nullptr || runner->isBusy();
}

void SegmentTableDialog::applyExternalNeighborMergeResult(
    quintptr segmentationIdentity,
    quintptr segmentationSignalIdentity,
    const Graph::SegmentationNeighborMergeResult &result) {
    if (reinterpret_cast<quintptr>(currentTableSegmentation.GetPointer()) != segmentationIdentity
        || reinterpret_cast<quintptr>(currentTableSegmentationSignal) != segmentationSignalIdentity) {
        return;
    }

    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status == Status::Merged && !tableLabelIdsAreStale) {
        applyMergedLabelChanges(
            result.newLabelByConsumedLabel, result.voxelCountByConsumedLabel);
        tableLabelIdsAreStale = false;
    } else if (result.dataChanged) {
        tableLabelIdsAreStale = true;
    } else {
        return;
    }

    statusLabel->setText(QStringLiteral("Table is out of date."));
    updateResultsActionState();
}

void SegmentTableDialog::applyExternalDeletedLabels(
    quintptr segmentationIdentity,
    quintptr segmentationSignalIdentity,
    std::vector<dataType::SegmentIdType> labels)
{
    if (reinterpret_cast<quintptr>(currentTableSegmentation.GetPointer()) != segmentationIdentity
        || reinterpret_cast<quintptr>(currentTableSegmentationSignal) != segmentationSignalIdentity
        || model == nullptr || sortModel == nullptr || labels.empty()) {
        return;
    }

    const std::unordered_set<dataType::SegmentIdType> deletedLabels(
        labels.begin(), labels.end());
    std::vector<int> rowsToRemove;
    rowsToRemove.reserve(labels.size());
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto *labelItem = model->item(row, SegmentTableDialog::COL_LABEL);
        if (labelItem != nullptr
            && deletedLabels.count(static_cast<dataType::SegmentIdType>(
                   labelItem->data(Qt::UserRole).toULongLong())) > 0) {
            rowsToRemove.push_back(row);
        }
    }

    QElapsedTimer timer;
    timer.start();
    sortModel->setSourceModel(nullptr);
    const RowRemovalStats removalStats =
        removeModelRowsInDescendingBlocks(model, std::move(rowsToRemove));
    sortModel->setSourceModel(model);

    statusLabel->setText(
        tableLabelIdsAreStale
            ? QStringLiteral("Table is out of date.")
            : QStringLiteral("%1 labels").arg(model->rowCount()));
    SP_LOG_DEBUG(
        "segmentation",
        QStringLiteral("operation=delete_segmentation_labels phase=table_quick_update "
                       "requested_labels=%1 removed_rows=%2 row_blocks=%3 elapsed_ms=%4")
            .arg(labels.size())
            .arg(removalStats.rows)
            .arg(removalStats.blocks)
            .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000.0, 0, 'f', 3));
    updateResultsActionState();
}

std::vector<std::pair<dataType::SegmentIdType, quint32>> SegmentTableDialog::collectSelectedLabelsFor3D() const {
    std::vector<std::pair<dataType::SegmentIdType, quint32>> labels;
    if (tableView == nullptr || tableView->selectionModel() == nullptr || model == nullptr) {
        return labels;
    }

    std::set<dataType::SegmentIdType> seenLabels;
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();
    for (const QModelIndex &proxyIdx : selectedRows) {
        const QModelIndex src = sortModel->mapToSource(proxyIdx);
        const auto *labelItem = model->item(src.row(), SegmentTableDialog::COL_LABEL);
        if (labelItem == nullptr) {
            continue;
        }

        const auto label = static_cast<dataType::SegmentIdType>(labelItem->data(Qt::UserRole).toLongLong());
        if (label == 0 || seenLabels.count(label)) {
            continue;
        }
        seenLabels.insert(label);

        quint32 color = 0xFFAAAAAA;
        if (currentTableSegmentationSignal != nullptr
            && label < static_cast<dataType::SegmentIdType>(currentTableSegmentationSignal->LUT.size())) {
            color = currentTableSegmentationSignal->LUT[label];
        }
        labels.emplace_back(label, color);
    }
    return labels;
}

std::vector<dataType::SegmentIdType> SegmentTableDialog::collect3DNavigationLabels() const {
    std::vector<dataType::SegmentIdType> labels;
    if (model == nullptr) {
        return labels;
    }

    labels.reserve(static_cast<std::size_t>(model->rowCount()));
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto *labelItem = model->item(row, SegmentTableDialog::COL_LABEL);
        if (labelItem == nullptr) {
            continue;
        }
        const auto label = static_cast<dataType::SegmentIdType>(
            labelItem->data(Qt::UserRole).toULongLong());
        if (label != 0) {
            labels.push_back(label);
        }
    }

    std::sort(labels.begin(), labels.end());
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    return labels;
}

void SegmentTableDialog::requestSingleLabel3D(
    Segment3DViewerDialog *dialog,
    dataType::SegmentIdType labelId,
    const Roi &bounds)
{
    auto *runner = taskRunner();
    if (runner == nullptr || runner->isBusy() || dialog == nullptr
        || currentTableSegmentation == nullptr) {
        if (dialog != nullptr) {
            dialog->rejectPreparedScene(labelId);
        }
        return;
    }

    const auto currentLabels = collect3DNavigationLabels();
    if (!std::binary_search(currentLabels.begin(), currentLabels.end(), labelId)) {
        dialog->rejectPreparedScene(labelId);
        return;
    }

    quint32 color = 0xFFAAAAAA;
    const auto colorIndex = static_cast<std::size_t>(labelId);
    if (currentTableSegmentationSignal != nullptr
        && colorIndex < currentTableSegmentationSignal->LUT.size()) {
        color = currentTableSegmentationSignal->LUT[colorIndex];
    }

    view3DUpdateDialog = dialog;
    pendingView3DLabel = labelId;
    view3DNavigationInFlight = true;

    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DView] requested adjacent segment targetLabel=%1")
                     .arg(labelId));

    const auto segImage = currentTableSegmentation;
    const Segment3DViewerDialog::LabelWithColor label{labelId, color};
    const QPointer<SegmentTableDialog> guardedThis(this);
    const QPointer<Segment3DViewerDialog> guardedDialog(dialog);
    const auto committed = std::make_shared<bool>(false);
    runner->runInBackground(
        QStringLiteral("Preparing 3D segment %1...").arg(labelId),
        [segImage, label, bounds]() {
            return Segment3DViewerDialog::prepareSingleLabelSlideshowScene(
                segImage, label, bounds);
        },
        [guardedThis, committed](Segment3DViewerDialog::PreparedScene preparedScene) {
            if (guardedThis == nullptr) {
                return;
            }
            guardedThis->onView3DPreparationFinished(std::move(preparedScene));
            *committed = true;
        },
        [guardedThis, guardedDialog, labelId, committed]() {
            if (*committed) {
                return;
            }
            if (guardedDialog != nullptr) {
                guardedDialog->rejectPreparedScene(labelId);
            }
            if (guardedThis != nullptr) {
                guardedThis->view3DNavigationInFlight = false;
                guardedThis->view3DUpdateDialog.clear();
                guardedThis->pendingView3DLabel = 0;
                guardedThis->updateResultsActionState();
            }
        });
}

bool SegmentTableDialog::deleteLabelFrom3DViewer(dataType::SegmentIdType labelId) {
    if (labelId == 0 || isBusy() || tableLabelIdsAreStale
        || !deleteSegmentationLabelsHandler || graphBase == nullptr
        || !segmentationSelectionMatchesCurrent(
            graphBase.get(), currentTableSegmentation, currentTableSegmentationSignal)) {
        return false;
    }

    return deleteSegmentationLabelsHandler(currentTableSegmentation, {labelId}) > 0;
}

void SegmentTableDialog::selectTableLabel(dataType::SegmentIdType labelId) {
    if (model == nullptr || sortModel == nullptr || tableView == nullptr
        || tableView->selectionModel() == nullptr) {
        return;
    }

    for (int row = 0; row < model->rowCount(); ++row) {
        const auto *labelItem = model->item(row, SegmentTableDialog::COL_LABEL);
        if (labelItem == nullptr
            || static_cast<dataType::SegmentIdType>(
                   labelItem->data(Qt::UserRole).toULongLong()) != labelId) {
            continue;
        }

        const QModelIndex proxyIndex = sortModel->mapFromSource(
            model->index(row, SegmentTableDialog::COL_LABEL));
        if (proxyIndex.isValid()) {
            tableView->selectionModel()->setCurrentIndex(
                proxyIndex,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            tableView->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
        }
        return;
    }
}

void SegmentTableDialog::onView3DSelectedClicked() {
    auto *runner = taskRunner();
    if (runner == nullptr || runner->isBusy() || currentTableSegmentation == nullptr) {
        return;
    }

    auto labels = collectSelectedLabelsFor3D();
    if (labels.empty()) {
        return;
    }

    view3DUpdateDialog.clear();
    pendingView3DLabel = 0;
    view3DNavigationInFlight = false;

    const QString progressText = labels.size() == 1
                                     ? QStringLiteral("Preparing 3D view for selected segment...")
                                     : QStringLiteral("Preparing 3D view for selected segments...");
    const auto segImage = currentTableSegmentation;
    const QPointer<SegmentTableDialog> guardedThis(this);
    runner->runWithLabel(
        progressText,
        [segImage, labels = std::move(labels)]() mutable {
            if (labels.size() == 1) {
                return Segment3DViewerDialog::prepareSingleLabelSlideshowScene(
                    segImage, labels.front());
            }
            return Segment3DViewerDialog::prepareScene(segImage, std::move(labels));
        },
        [guardedThis](Segment3DViewerDialog::PreparedScene preparedScene) {
            if (guardedThis != nullptr) {
                guardedThis->onView3DPreparationFinished(std::move(preparedScene));
            }
        });
}

// ---- compute (worker thread) ------------------------------------------------

SegmentTableDialog::ComputeResult SegmentTableDialog::computeFeatures(
    dataType::SegmentsImageType::Pointer segImage,
    FeatureFlags flags)
{
    ComputeResult result;
    result.flags = flags;

    QElapsedTimer timer;
    timer.start();

    const auto region = segImage->GetLargestPossibleRegion();
    const auto size = region.GetSize();
    result.is2D = size[2] == 1;

    if (result.is2D) {
        using Image2D = itk::Image<dataType::SegmentIdType, 2>;
        using ExtractFilter = itk::ExtractImageFilter<dataType::SegmentsImageType, Image2D>;

        auto extractionRegion = region;
        auto extractionSize = extractionRegion.GetSize();
        extractionSize[2] = 0;
        extractionRegion.SetSize(extractionSize);

        auto extract = ExtractFilter::New();
        extract->SetInput(segImage);
        extract->SetExtractionRegion(extractionRegion);
        extract->SetDirectionCollapseToSubmatrix();
        if (flags.overridePixelSize && std::isfinite(flags.pixelSize) && flags.pixelSize > 0.0) {
            using ChangeInformationFilter = itk::ChangeInformationImageFilter<Image2D>;
            auto changeInformation = ChangeInformationFilter::New();
            Image2D::SpacingType spacing;
            spacing.Fill(flags.pixelSize);
            changeInformation->SetInput(extract->GetOutput());
            changeInformation->SetOutputSpacing(spacing);
            changeInformation->ChangeSpacingOn();
            result.rows = computeShapeFeatureRows<Image2D>(changeInformation->GetOutput(), flags);
        } else {
            result.rows = computeShapeFeatureRows<Image2D>(extract->GetOutput(), flags);
        }

        const int singletonZ = static_cast<int>(region.GetIndex()[2]);
        for (SegmentRow &row : result.rows) {
            row.centroidZ = singletonZ;
        }
    } else {
        result.rows = computeShapeFeatureRows<dataType::SegmentsImageType>(segImage, flags);
    }

    if (flags.isIsolated) {
        const auto isolationByLabel = computeIsolationByLabel(segImage);
        for (SegmentRow &row : result.rows) {
            const auto isolationIt = isolationByLabel.find(row.label);
            row.isIsolated = isolationIt == isolationByLabel.end() || isolationIt->second;
        }
    }

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    return result;
}

// ---- result arrival ---------------------------------------------------------

void SegmentTableDialog::onComputeFinished(ComputeResult result) {
    QElapsedTimer tableTimer;
    tableTimer.start();
    populateTable(result);
    const double tablePopulateSeconds = tableTimer.elapsed() / 1000.0;
    tableLabelIdsAreStale = false;

    backButton->setEnabled(true);
    computeButton->setEnabled(true);
    recomputeButton->setEnabled(true);
    exportCsvButton->setEnabled(!result.rows.empty());
    QString status = QString("%1 labels | Features: %2 s | Table: %3 s")
                         .arg(result.rows.size())
                         .arg(result.elapsedSeconds, 0, 'f', 2)
                         .arg(tablePopulateSeconds, 0, 'f', 2);
    if (result.is2D && result.flags.overridePixelSize) {
        status += QStringLiteral(" | Calibration: %1 %2/px")
                      .arg(result.flags.pixelSize, 0, 'g', 10)
                      .arg(physicalUnitLabel(result.flags.physicalUnit));
    }
    statusLabel->setText(status);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("Segment table computeFinished currentTableSegmentation=%1 "
                       "currentTableSegmentationSignal=%2 selectedSegmentation=%3 "
                       "selectedSegmentationSignal=%4 rows=%5 featureComputeSeconds=%6 "
                       "tablePopulateSeconds=%7")
            .arg(reinterpret_cast<quintptr>(currentTableSegmentation.GetPointer()), 0, 16)
            .arg(reinterpret_cast<quintptr>(currentTableSegmentationSignal), 0, 16)
            .arg(reinterpret_cast<quintptr>(graphBase->pSelectedSegmentation.GetPointer()), 0, 16)
            .arg(reinterpret_cast<quintptr>(graphBase->pSelectedSegmentationSignal), 0, 16)
            .arg(result.rows.size())
            .arg(result.elapsedSeconds, 0, 'f', 3)
            .arg(tablePopulateSeconds, 0, 'f', 3));
    updateResultsActionState();
    emit computeFinishedDebug();
}

void SegmentTableDialog::onView3DPreparationFinished(
    Segment3DViewerDialog::PreparedScene preparedScene)
{
    const bool navigationRequest = view3DNavigationInFlight;
    const QPointer<Segment3DViewerDialog> updateDialog = view3DUpdateDialog;
    const auto requestedLabel = pendingView3DLabel;
    QWidget *const messageParent = updateDialog != nullptr
                                       ? static_cast<QWidget *>(updateDialog.data())
                                       : static_cast<QWidget *>(this);
    view3DNavigationInFlight = false;
    view3DUpdateDialog.clear();
    pendingView3DLabel = 0;

    try {
        if (navigationRequest) {
            if (updateDialog == nullptr) {
                updateResultsActionState();
                return;
            }

            if (!updateDialog->acceptPreparedScene(std::move(preparedScene))) {
                if (!updateDialog->rejectPreparedScene(requestedLabel)) {
                    updateResultsActionState();
                    return;
                }
                QMessageBox::information(
                    updateDialog,
                    QStringLiteral("3D View"),
                    QStringLiteral("No 3D surface could be generated for segment %1.")
                        .arg(requestedLabel));
            }
            updateResultsActionState();
            return;
        }

        const auto segImage = currentTableSegmentation;
        if (!preparedScene.meshes.empty()) {
            const bool singleLabelScene = preparedScene.targetLabelId != 0
                                          && preparedScene.meshes.size() == 1;
            auto *dialog = new Segment3DViewerDialog(std::move(preparedScene), this);
            dialog->setNavigateToLabelHandler(
                [segImage, orthoViewer = orthoViewer](dataType::SegmentIdType labelId) {
                    if (segImage == nullptr || orthoViewer == nullptr) {
                        return;
                    }

                    dataType::SegmentsImageType::IndexType index;
                    if (!utils::findRepresentativeVoxelForLabel(segImage, labelId, index)) {
                        return;
                    }

                    navigateOrthoViewerToIndex(orthoViewer, index);
                });
            if (singleLabelScene) {
                const QPointer<Segment3DViewerDialog> guardedDialog(dialog);
                Segment3DViewerDialog::SingleLabelSessionConfig session;
                session.labels = collect3DNavigationLabels();
                session.requestLabel =
                    [this, guardedDialog](dataType::SegmentIdType labelId,
                                          const Roi &bounds) {
                        if (guardedDialog != nullptr) {
                            requestSingleLabel3D(guardedDialog.data(), labelId, bounds);
                        }
                    };
                session.labelActivated = [this](dataType::SegmentIdType labelId) {
                    selectTableLabel(labelId);
                };
                session.deleteLabel = [this](dataType::SegmentIdType labelId) {
                    return deleteLabelFrom3DViewer(labelId);
                };
                dialog->setSingleLabelSession(std::move(session));
            }
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->presentInFront();
        } else {
            QMessageBox::information(this, "3D View", "No 3D surface could be generated for the selected labels.");
        }
    } catch (const std::exception &e) {
        if (updateDialog != nullptr) {
            updateDialog->rejectPreparedScene(requestedLabel);
        }
        QMessageBox::critical(messageParent, "3D View Error", QString::fromStdString(e.what()));
    } catch (...) {
        if (updateDialog != nullptr) {
            updateDialog->rejectPreparedScene(requestedLabel);
        }
        QMessageBox::critical(messageParent, "3D View Error",
                              "Unknown error while preparing 3D view.");
    }

    updateResultsActionState();
}

// ---- table population -------------------------------------------------------

void SegmentTableDialog::applyMergedLabelChanges(
    const std::map<dataType::SegmentIdType, dataType::SegmentIdType> &newLabelByConsumedLabel,
    const std::map<dataType::SegmentIdType, std::size_t> &voxelCountByConsumedLabel) {
    QElapsedTimer timer;
    timer.start();
    using SegmentId = dataType::SegmentIdType;
    struct LabelRow {
        SegmentId oldLabel = 0;
        int row = -1;
        std::size_t voxelCount = 0;
    };

    std::map<SegmentId, std::vector<LabelRow>> rowsByNewLabel;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto *labelItem = model->item(row, SegmentTableDialog::COL_LABEL);
        if (labelItem == nullptr) {
            continue;
        }
        const SegmentId oldLabel = static_cast<SegmentId>(
            labelItem->data(Qt::UserRole).toULongLong());
        const auto replacement = newLabelByConsumedLabel.find(oldLabel);
        if (replacement != newLabelByConsumedLabel.end()) {
            const auto voxelCount = voxelCountByConsumedLabel.find(oldLabel);
            rowsByNewLabel[replacement->second].push_back(
                {oldLabel, row, voxelCount == voxelCountByConsumedLabel.end() ? 0 : voxelCount->second});
        }
    }

    std::vector<int> rowsToRemove;
    std::size_t updatedLabelCount = 0;
    // Keep the existing feature values of one representative row per result.
    // Detaching the proxy applies sorting and filtering only once after all ID
    // changes and row removals are complete.
    sortModel->setSourceModel(nullptr);
    for (const auto &[newLabel, labelRows] : rowsByNewLabel) {
        const auto retained = std::max_element(
            labelRows.begin(), labelRows.end(),
            [](const LabelRow &first, const LabelRow &second) {
                if (first.voxelCount != second.voxelCount) {
                    return first.voxelCount < second.voxelCount;
                }
                return first.oldLabel < second.oldLabel;
            });
        if (retained == labelRows.end()) {
            continue;
        }

        auto *retainedItem = model->item(retained->row, SegmentTableDialog::COL_LABEL);
        retainedItem->setText(QString::number(newLabel));
        retainedItem->setData(QVariant(static_cast<qlonglong>(newLabel)), Qt::UserRole);
        ++updatedLabelCount;

        QStringList oldLabels;
        for (const LabelRow &labelRow : labelRows) {
            oldLabels << QStringLiteral("%1(%2)").arg(labelRow.oldLabel).arg(labelRow.voxelCount);
            if (labelRow.row != retained->row) {
                rowsToRemove.push_back(labelRow.row);
            }
        }
        SP_LOG_DEBUG(
            "segmentation",
            QStringLiteral("operation=merge_with_neighbor phase=table_label_update old_voxels=[%1] retained=%2 new=%3")
                .arg(oldLabels.join(QStringLiteral(" ")))
                .arg(retained->oldLabel)
                .arg(newLabel));
    }

    const RowRemovalStats removalStats =
        removeModelRowsInDescendingBlocks(model, std::move(rowsToRemove));
    sortModel->setSourceModel(model);
    SP_LOG_DEBUG(
        "segmentation",
        QStringLiteral("operation=merge_with_neighbor phase=table_quick_update "
                       "updated_labels=%1 removed_rows=%2 row_blocks=%3 elapsed_ms=%4")
            .arg(updatedLabelCount)
            .arg(removalStats.rows)
            .arg(removalStats.blocks)
            .arg(static_cast<double>(timer.nsecsElapsed()) / 1000000.0, 0, 'f', 3));
}

void SegmentTableDialog::populateTable(const ComputeResult &result) {
    currentResultFlags = result.flags;
    currentResultIs2D = result.is2D;

    // Avoid re-sorting and resizing the proxy after every cell change during a bulk refresh.
    sortModel->setSourceModel(nullptr);
    updateColumnHeaders(result.flags, result.is2D);
    model->removeRows(0, model->rowCount());
    model->setRowCount(static_cast<int>(result.rows.size()));

    for (int r = 0; r < static_cast<int>(result.rows.size()); ++r) {
        const auto &row = result.rows[r];

        // Label item — centroid stored in UserRole+1/2/3 for navigation.
        auto *labelItem = new QStandardItem(QString::number(row.label));
        labelItem->setData(QVariant(static_cast<qlonglong>(row.label)), Qt::UserRole);
        labelItem->setData(QVariant(row.centroidX), Qt::UserRole + 1);
        labelItem->setData(QVariant(row.centroidY), Qt::UserRole + 2);
        labelItem->setData(QVariant(row.centroidZ), Qt::UserRole + 3);
        labelItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        model->setItem(r, SegmentTableDialog::COL_LABEL,               labelItem);
        model->setItem(r, SegmentTableDialog::COL_VOLUME,              makeNumericItem(row.volume, 0));
        model->setItem(r, SegmentTableDialog::COL_IS_ISOLATED,         makeBooleanItem(row.isIsolated));
        model->setItem(r, SegmentTableDialog::COL_PHYSICAL_SIZE,        makeNumericItem(row.physicalSize, 2));
        model->setItem(r, SegmentTableDialog::COL_PIXELS_ON_BORDER,     makeNumericItem(row.pixelsOnBorder, 0));
        model->setItem(r, SegmentTableDialog::COL_PERIMETER_ON_BORDER,  makeNumericItem(row.perimeterOnBorder, 2));
        // Centroid always populated; column visibility controlled by flags.centroid.
        model->setItem(r, SegmentTableDialog::COL_CX, makeNumericItem(static_cast<double>(row.centroidX), 0));
        model->setItem(r, SegmentTableDialog::COL_CY, makeNumericItem(static_cast<double>(row.centroidY), 0));
        model->setItem(r, SegmentTableDialog::COL_CZ, makeNumericItem(static_cast<double>(row.centroidZ), 0));
        model->setItem(r, SegmentTableDialog::COL_BBOX_W,               makeNumericItem(row.bboxW, 0));
        model->setItem(r, SegmentTableDialog::COL_BBOX_H,               makeNumericItem(row.bboxH, 0));
        model->setItem(r, SegmentTableDialog::COL_BBOX_D,               makeNumericItem(row.bboxD, 0));
        model->setItem(r, SegmentTableDialog::COL_ELONGATION,            makeNumericItem(row.elongation));
        model->setItem(r, SegmentTableDialog::COL_FLATNESS,              makeNumericItem(row.flatness));
        model->setItem(r, SegmentTableDialog::COL_ROUNDNESS,             makeNumericItem(row.roundness));
        model->setItem(r, SegmentTableDialog::COL_EQUIV_SPH_RADIUS,      makeNumericItem(row.equivSphRadius));
        model->setItem(r, SegmentTableDialog::COL_EQUIV_SPH_PERIM,       makeNumericItem(row.equivSphPerimeter));
        model->setItem(r, SegmentTableDialog::COL_EQUIV_ELLIP_D0,        makeNumericItem(row.equivEllipD0));
        model->setItem(r, SegmentTableDialog::COL_EQUIV_ELLIP_D1,        makeNumericItem(row.equivEllipD1));
        model->setItem(r, SegmentTableDialog::COL_EQUIV_ELLIP_D2,        makeNumericItem(row.equivEllipD2));
        model->setItem(r, SegmentTableDialog::COL_PRINCIPAL_MOM0,        makeNumericItem(row.principalMom0));
        model->setItem(r, SegmentTableDialog::COL_PRINCIPAL_MOM1,        makeNumericItem(row.principalMom1));
        model->setItem(r, SegmentTableDialog::COL_PRINCIPAL_MOM2,        makeNumericItem(row.principalMom2));
        model->setItem(r, SegmentTableDialog::COL_PERIMETER,             makeNumericItem(row.perimeter, 1));
        model->setItem(r, SegmentTableDialog::COL_OBBOX_W,               makeNumericItem(row.obboxW));
        model->setItem(r, SegmentTableDialog::COL_OBBOX_H,               makeNumericItem(row.obboxH));
        model->setItem(r, SegmentTableDialog::COL_OBBOX_D,               makeNumericItem(row.obboxD));
        model->setItem(r, SegmentTableDialog::COL_OBBOX_VOLUME,          makeNumericItem(row.obboxVolume));
    }

    applyColumnColoring();
    sortModel->setSourceModel(model);
    updateColumnVisibility(result.flags, result.is2D);
    tableView->resizeColumnsToContents();
}

// ---- color coding -----------------------------------------------------------

void SegmentTableDialog::applyColumnColoring() {
    const int nRows = model->rowCount();
    if (nRows == 0) { return; }

    for (int col = SegmentTableDialog::COL_VOLUME; col < SegmentTableDialog::COL_COUNT; ++col) {
        if (col == SegmentTableDialog::COL_IS_ISOLATED) {
            continue;
        }
        double minVal = std::numeric_limits<double>::max();
        double maxVal = std::numeric_limits<double>::lowest();

        for (int row = 0; row < nRows; ++row) {
            const auto *item = model->item(row, col);
            if (!item) { continue; }
            bool ok = false;
            const double v = item->data(Qt::UserRole).toDouble(&ok);
            if (ok && v >= 0.0) {
                minVal = std::min(minVal, v);
                maxVal = std::max(maxVal, v);
            }
        }

        if (minVal >= maxVal) { continue; }
        const double range = maxVal - minVal;

        for (int row = 0; row < nRows; ++row) {
            auto *item = model->item(row, col);
            if (!item) { continue; }
            bool ok = false;
            const double v = item->data(Qt::UserRole).toDouble(&ok);
            if (!ok || v < 0.0) { continue; }
            const QColor bg = colorForNormalizedValue((v - minVal) / range);
            item->setBackground(bg);
            item->setForeground(textColorForBackground(bg));
        }
    }
}

// ---- column visibility ------------------------------------------------------

void SegmentTableDialog::updateColumnHeaders(const FeatureFlags &flags, bool is2D) {
    const bool calibrated = is2D && flags.overridePixelSize;
    const QString unit = physicalUnitLabel(flags.physicalUnit);
    const auto withUnit = [calibrated, &unit](const QString &name, int power = 1) {
        if (!calibrated) {
            return name;
        }
        const QString suffix = power == 1 ? unit : unit + QStringLiteral("²");
        return QStringLiteral("%1 (%2)").arg(name, suffix);
    };

    model->setHeaderData(SegmentTableDialog::COL_VOLUME, Qt::Horizontal,
                         is2D ? QStringLiteral("# Pixels") : QStringLiteral("# Voxels"));
    model->setHeaderData(SegmentTableDialog::COL_PHYSICAL_SIZE, Qt::Horizontal,
                         is2D ? withUnit("Physical Area", 2) : "Physical Size");
    model->setHeaderData(
        SegmentTableDialog::COL_PIXELS_ON_BORDER,
        Qt::Horizontal,
        flags.borderDistancePx == 0
            ? QStringLiteral("Px on Border")
            : QStringLiteral("Px ≤ %1 px from Border").arg(flags.borderDistancePx));
    model->setHeaderData(SegmentTableDialog::COL_PERIMETER_ON_BORDER, Qt::Horizontal,
                         is2D ? withUnit("Perim on Border") : QStringLiteral("Perim on Border"));
    model->setHeaderData(SegmentTableDialog::COL_CX, Qt::Horizontal,
                         is2D ? QStringLiteral("CX (px)") : QStringLiteral("CX"));
    model->setHeaderData(SegmentTableDialog::COL_CY, Qt::Horizontal,
                         is2D ? QStringLiteral("CY (px)") : QStringLiteral("CY"));
    model->setHeaderData(SegmentTableDialog::COL_CZ, Qt::Horizontal, QStringLiteral("CZ"));
    model->setHeaderData(SegmentTableDialog::COL_BBOX_W, Qt::Horizontal,
                         is2D ? QStringLiteral("BBox W (px)") : QStringLiteral("BBox W"));
    model->setHeaderData(SegmentTableDialog::COL_BBOX_H, Qt::Horizontal,
                         is2D ? QStringLiteral("BBox H (px)") : QStringLiteral("BBox H"));
    model->setHeaderData(SegmentTableDialog::COL_BBOX_D, Qt::Horizontal, QStringLiteral("BBox D"));
    model->setHeaderData(SegmentTableDialog::COL_EQUIV_SPH_RADIUS, Qt::Horizontal,
                         is2D ? withUnit("Equiv Circle R") : "Equiv Sph R");
    model->setHeaderData(SegmentTableDialog::COL_EQUIV_SPH_PERIM, Qt::Horizontal,
                         is2D ? withUnit("Equiv Circle Perim") : "Equiv Sph Perim");
    model->setHeaderData(SegmentTableDialog::COL_EQUIV_ELLIP_D0, Qt::Horizontal,
                         is2D ? withUnit("Ellip D0") : "Ellip D0");
    model->setHeaderData(SegmentTableDialog::COL_EQUIV_ELLIP_D1, Qt::Horizontal,
                         is2D ? withUnit("Ellip D1") : "Ellip D1");
    model->setHeaderData(SegmentTableDialog::COL_EQUIV_ELLIP_D2, Qt::Horizontal, "Ellip D2");
    model->setHeaderData(SegmentTableDialog::COL_PRINCIPAL_MOM0, Qt::Horizontal,
                         is2D ? withUnit("PrinMom 0", 2) : "PrinMom 0");
    model->setHeaderData(SegmentTableDialog::COL_PRINCIPAL_MOM1, Qt::Horizontal,
                         is2D ? withUnit("PrinMom 1", 2) : "PrinMom 1");
    model->setHeaderData(SegmentTableDialog::COL_PRINCIPAL_MOM2, Qt::Horizontal, "PrinMom 2");
    model->setHeaderData(SegmentTableDialog::COL_PERIMETER, Qt::Horizontal,
                         is2D ? withUnit("Perimeter") : "Surface Area");
    model->setHeaderData(SegmentTableDialog::COL_OBBOX_W, Qt::Horizontal,
                         is2D ? withUnit("OBBox W") : "OBBox W");
    model->setHeaderData(SegmentTableDialog::COL_OBBOX_H, Qt::Horizontal,
                         is2D ? withUnit("OBBox H") : "OBBox H");
    model->setHeaderData(SegmentTableDialog::COL_OBBOX_D, Qt::Horizontal, "OBBox D");
    model->setHeaderData(SegmentTableDialog::COL_OBBOX_VOLUME, Qt::Horizontal,
                         is2D ? withUnit("OBBox Area", 2) : "OBBox Vol");
}

void SegmentTableDialog::updateColumnVisibility(const FeatureFlags &f, bool is2D) {
    // Hide every non-label column first, then reveal only what was computed.
    for (int col = SegmentTableDialog::COL_VOLUME; col < SegmentTableDialog::COL_COUNT; ++col) {
        tableView->setColumnHidden(col, true);
    }

    if (f.volume)            tableView->setColumnHidden(SegmentTableDialog::COL_VOLUME, false);
    if (f.isIsolated)        tableView->setColumnHidden(SegmentTableDialog::COL_IS_ISOLATED, false);
    if (f.physicalSize)      tableView->setColumnHidden(SegmentTableDialog::COL_PHYSICAL_SIZE, false);
    if (f.pixelsOnBorder)    tableView->setColumnHidden(SegmentTableDialog::COL_PIXELS_ON_BORDER, false);
    if (f.perimeterOnBorder) tableView->setColumnHidden(SegmentTableDialog::COL_PERIMETER_ON_BORDER, false);
    if (f.centroid) {
        tableView->setColumnHidden(SegmentTableDialog::COL_CX, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_CY, false);
        if (!is2D) {
            tableView->setColumnHidden(SegmentTableDialog::COL_CZ, false);
        }
    }
    if (f.bbox) {
        tableView->setColumnHidden(SegmentTableDialog::COL_BBOX_W, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_BBOX_H, false);
        if (!is2D) {
            tableView->setColumnHidden(SegmentTableDialog::COL_BBOX_D, false);
        }
    }
    if (f.elongation)        tableView->setColumnHidden(SegmentTableDialog::COL_ELONGATION, false);
    if (f.flatness)          tableView->setColumnHidden(SegmentTableDialog::COL_FLATNESS, false);
    if (f.roundness)         tableView->setColumnHidden(SegmentTableDialog::COL_ROUNDNESS, false);
    if (f.equivSphRadius)    tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_SPH_RADIUS, false);
    if (f.equivSphPerimeter) tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_SPH_PERIM, false);
    if (f.equivEllipsoid) {
        tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_ELLIP_D0, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_ELLIP_D1, false);
        if (!is2D) {
            tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_ELLIP_D2, false);
        }
    }
    if (f.principalMoments) {
        tableView->setColumnHidden(SegmentTableDialog::COL_PRINCIPAL_MOM0, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_PRINCIPAL_MOM1, false);
        if (!is2D) {
            tableView->setColumnHidden(SegmentTableDialog::COL_PRINCIPAL_MOM2, false);
        }
    }
    if (f.perimeter)    tableView->setColumnHidden(SegmentTableDialog::COL_PERIMETER, false);
    if (f.orientedBBox) {
        tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_W, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_H, false);
        if (!is2D) {
            tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_D, false);
        }
        tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_VOLUME, false);
    }
}

// ---- navigation -------------------------------------------------------------

void SegmentTableDialog::onSelectionChanged(const QModelIndex &current,
                                            const QModelIndex & /*previous*/) {
    if (!current.isValid()) { return; }
    const QModelIndex src = sortModel->mapToSource(current);
    const auto *labelItem = model->item(src.row(), SegmentTableDialog::COL_LABEL);
    if (!labelItem) { return; }
    navigateTo(labelItem->data(Qt::UserRole + 1).toInt(),
               labelItem->data(Qt::UserRole + 2).toInt(),
               labelItem->data(Qt::UserRole + 3).toInt());
}

void SegmentTableDialog::navigateTo(int x, int y, int z) {
    if (orthoViewer == nullptr) { return; }
    if (orthoViewer->xy->isSliceIndexValid(z)) orthoViewer->xy->setSliceIndex(z);
    if (orthoViewer->xz->isSliceIndexValid(y)) orthoViewer->xz->setSliceIndex(y);
    if (orthoViewer->zy->isSliceIndexValid(x)) orthoViewer->zy->setSliceIndex(x);
    orthoViewer->centerViewportsToXYZImageSpace(x, y, z);
}

// ---- CSV export (only visible columns, current sort order) ------------------

QString SegmentTableDialog::suggestedCsvExportPath(const QString &storedDefaultSavePath) const {
    const QString segmentationName =
        currentTableSegmentationSignal != nullptr
            ? currentTableSegmentationSignal->name
            : QString();
    return export_path_utils::suggestedExportPath(
        storedDefaultSavePath,
        {currentTableSegmentationSignal != nullptr
             ? currentTableSegmentationSignal->sourceFilePath
             : QString(),
         graphBase->pWorkingSegments != nullptr
             ? graphBase->pWorkingSegments->sourceFilePath
             : QString(),
         graphBase->lastLoadedSourcePath},
        segmentationName,
        QStringLiteral("Segmentation"),
        QStringLiteral("_features.csv"));
}

void SegmentTableDialog::onExportCsvClicked() {
    QSettings settings;
    const QString defaultSavePath =
        settings.value(QStringLiteral("default_save_dir")).toString();
    const QString path = QFileDialog::getSaveFileName(
        this,
        "Export CSV",
        suggestedCsvExportPath(defaultSavePath),
        "CSV files (*.csv)");
    if (path.isEmpty()) { return; }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed",
                             "Could not open file for writing:\n" + path);
        return;
    }

    QTextStream out(&file);

    // Only export the columns that are currently visible.
    QList<int> visibleCols;
    for (int col = 0; col < SegmentTableDialog::COL_COUNT; ++col) {
        if (!tableView->isColumnHidden(col)) { visibleCols << col; }
    }

    QStringList headers;
    for (int col : visibleCols) {
        const auto *h = model->horizontalHeaderItem(col);
        headers << (h ? h->text() : QString());
    }
    out << headers.join(",") << "\n";

    for (int proxyRow = 0; proxyRow < sortModel->rowCount(); ++proxyRow) {
        QStringList rowData;
        for (int col : visibleCols) {
            rowData << sortModel->data(sortModel->index(proxyRow, col),
                                       Qt::DisplayRole).toString();
        }
        out << rowData.join(",") << "\n";
    }

    if (currentTableSegmentation != nullptr) {
        const auto size = currentTableSegmentation->GetLargestPossibleRegion().GetSize();
        const auto spacing = currentTableSegmentation->GetSpacing();
        const int dimensions = currentResultIs2D ? 2 : 3;
        QStringList sizeParts;
        QStringList spacingParts;
        quint64 elementCount = 1;
        for (int dimension = 0; dimension < dimensions; ++dimension) {
            sizeParts << QString::number(size[dimension]);
            spacingParts << QString::number(spacing[dimension], 'g', 15);
            elementCount *= static_cast<quint64>(size[dimension]);
        }

        out << "\n# metadata\n";
        if (currentResultIs2D && currentResultFlags.overridePixelSize) {
            out << "# pixel_size="
                << QString::number(currentResultFlags.pixelSize, 'g', 15) << " "
                << physicalUnitLabel(currentResultFlags.physicalUnit) << "/px\n";
        } else {
            out << (currentResultIs2D ? "# pixel_spacing=" : "# voxel_spacing=")
                << spacingParts.join("x") << " image-units/"
                << (currentResultIs2D ? "px\n" : "voxel\n");
        }
        out << "# image_dimensions=" << sizeParts.join("x") << " "
            << (currentResultIs2D ? "pixels\n" : "voxels\n");
        out << (currentResultIs2D ? "# total_image_pixels=" : "# total_image_voxels=")
            << elementCount << "\n";
    }

    out.flush();
    const bool exportSucceeded =
        out.status() == QTextStream::Ok && file.commit();
    if (!exportSucceeded) {
        QMessageBox::warning(this, "Export Failed",
                             "Could not write file completely:\n" + path);
        return;
    }

    settings.setValue(QStringLiteral("default_save_dir"),
                      QFileInfo(path).absoluteFilePath());
    statusLabel->setText("Exported to " + path);
}
