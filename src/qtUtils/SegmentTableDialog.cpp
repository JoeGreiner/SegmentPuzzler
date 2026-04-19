#include "SegmentTableDialog.h"

#include <cmath>
#include <limits>
#include <unordered_map>

#include <QCheckBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QProgressDialog>
#include <QScrollArea>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <itkContinuousIndex.h>
#include <itkLabelImageToShapeLabelMapFilter.h>

#include "src/segment_handling/Graph.h"
#include "src/viewers/Segment3DViewerDialog.h"
#include "src/viewers/OrthoViewer.h"

// ---- helpers ----------------------------------------------------------------

namespace {

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
    stack->setCurrentIndex(0);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(stack);

    watcher = new QFutureWatcher<ComputeResult>(this);
    connect(watcher, &QFutureWatcher<ComputeResult>::finished,
            this,    &SegmentTableDialog::onComputeFinished);

    view3DWatcher = new QFutureWatcher<Segment3DViewerDialog::PreparedScene>(this);
    connect(view3DWatcher, &QFutureWatcher<Segment3DViewerDialog::PreparedScene>::finished,
            this,          &SegmentTableDialog::onView3DPreparationFinished);
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

    // --- Basic Measurements ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("Basic Measurements", grid);
        cbVolume            = new QCheckBox("Volume (voxels)");                  cbVolume->setChecked(true);
        cbIsIsolated        = new QCheckBox("Is Isolated");                      cbIsIsolated->setChecked(true);
        cbPhysicalSize      = new QCheckBox("Physical Size");                    cbPhysicalSize->setChecked(false);
        cbPixelsOnBorder    = new QCheckBox("Pixels on Border");                 cbPixelsOnBorder->setChecked(false);
        cbPerimeterOnBorder = new QCheckBox("Perimeter on Border (physical)");   cbPerimeterOnBorder->setChecked(false);
        grid->addWidget(cbVolume,            0, 0);
        grid->addWidget(cbIsIsolated,        0, 1);
        grid->addWidget(cbPhysicalSize,      1, 0);
        grid->addWidget(cbPixelsOnBorder,    1, 1);
        grid->addWidget(cbPerimeterOnBorder, 2, 0);
        vContent->addWidget(gb);
    }

    // --- Position & Extent ---
    {
        QGridLayout *grid = nullptr;
        auto *gb = makeGroup("Position & Extent", grid);
        cbCentroid = new QCheckBox("Centroid (CX, CY, CZ in voxels)");      cbCentroid->setChecked(true);
        cbBBox     = new QCheckBox("Axis-Aligned Bounding Box (W, H, D)");   cbBBox->setChecked(false);
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
        cbEquivSphRadius    = new QCheckBox("Equiv. Spherical Radius");             cbEquivSphRadius->setChecked(true);
        cbEquivSphPerimeter = new QCheckBox("Equiv. Spherical Perimeter");          cbEquivSphPerimeter->setChecked(false);
        cbEquivEllipsoid    = new QCheckBox("Equiv. Ellipsoid Diameters (3 axes)"); cbEquivEllipsoid->setChecked(false);
        cbPrincipalMoments  = new QCheckBox("Principal Moments (3 values)");        cbPrincipalMoments->setChecked(false);
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
        cbPerimeter    = new QCheckBox("Surface Area (marching cubes)");   cbPerimeter->setChecked(false);
        cbOrientedBBox = new QCheckBox("Oriented Bounding Box (size + volume)"); cbOrientedBBox->setChecked(false);
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
    auto *btnRow = new QHBoxLayout();
    btnRow->addWidget(selectAllButton);
    btnRow->addWidget(selectNoneButton);
    btnRow->addStretch();
    btnRow->addWidget(computeButton);
    outer->addLayout(btnRow);

    connect(selectAllButton,  &QPushButton::clicked, this, [this]{ setAllChecked(true);  });
    connect(selectNoneButton, &QPushButton::clicked, this, [this]{ setAllChecked(false); });
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

SegmentTableDialog::~SegmentTableDialog() {
    if (watcher != nullptr && watcher->isRunning()) {
        watcher->waitForFinished();
    }
    if (view3DWatcher != nullptr && view3DWatcher->isRunning()) {
        view3DWatcher->waitForFinished();
    }
}

// ---- setup page actions -----------------------------------------------------

SegmentTableDialog::FeatureFlags SegmentTableDialog::collectFlags() const {
    FeatureFlags f;
    f.volume            = cbVolume->isChecked();
    f.isIsolated        = cbIsIsolated->isChecked();
    f.physicalSize      = cbPhysicalSize->isChecked();
    f.pixelsOnBorder    = cbPixelsOnBorder->isChecked();
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

void SegmentTableDialog::setQuickComputeMode() {
    setAllChecked(false);
    cbVolume->setChecked(true);
    cbIsIsolated->setChecked(true);
    cbCentroid->setChecked(true);
}

void SegmentTableDialog::startCompute(dataType::SegmentsImageType::Pointer segImg) {
    if (segImg == nullptr) {
        segImg = graphBase->pSelectedSegmentation;
    }

    std::cout << "[SegmentTableDebug] startCompute"
              << " requestedSegmentation=" << static_cast<const void *>(segImg.GetPointer())
              << " selectedSegmentation=" << static_cast<const void *>(graphBase->pSelectedSegmentation.GetPointer())
              << " selectedSegmentationSignal=" << static_cast<const void *>(graphBase->pSelectedSegmentationSignal)
              << " selectedSegmentationMaxId=" << graphBase->selectedSegmentationMaxSegmentId
              << "\n";

    if (segImg == nullptr) {
        QMessageBox::information(this, "No Segmentation",
                                 "No segmentation is currently selected.\n"
                                 "Load and select a segmentation first.");
        return;
    }

    if (watcher->isRunning() || (view3DWatcher != nullptr && view3DWatcher->isRunning())) {
        return;
    }

    const FeatureFlags flags = collectFlags();
    currentTableSegmentation = segImg;
    currentTableSegmentationSignal = graphBase->pSelectedSegmentationSignal;
    std::cout << "[SegmentTableDebug] featureFlags"
              << " volume=" << flags.volume
              << " isIsolated=" << flags.isIsolated
              << " centroid=" << flags.centroid
              << " elongation=" << flags.elongation
              << " flatness=" << flags.flatness
              << " roundness=" << flags.roundness
              << " bbox=" << flags.bbox
              << " physicalSize=" << flags.physicalSize
              << " pixelsOnBorder=" << flags.pixelsOnBorder
              << " perimeterOnBorder=" << flags.perimeterOnBorder
              << " equivSphRadius=" << flags.equivSphRadius
              << " equivSphPerimeter=" << flags.equivSphPerimeter
              << " equivEllipsoid=" << flags.equivEllipsoid
              << " principalMoments=" << flags.principalMoments
              << " perimeter=" << flags.perimeter
              << " orientedBBox=" << flags.orientedBBox
              << "\n";

    // Switch to results page immediately so the user sees the computing state.
    computeButton->setEnabled(false);
    stack->setCurrentIndex(1);
    backButton->setEnabled(false);
    recomputeButton->setEnabled(false);
    view3DButton->setEnabled(false);
    exportCsvButton->setEnabled(false);
    statusLabel->setText("Computing features…");

    watcher->setFuture(QtConcurrent::run([segImg, flags]() {
        return SegmentTableDialog::computeFeatures(segImg, flags);
    }));
}

void SegmentTableDialog::onComputeClicked() {
    startCompute();
}

void SegmentTableDialog::onBackClicked() {
    if (watcher->isRunning() || (view3DWatcher != nullptr && view3DWatcher->isRunning())) { return; }
    computeButton->setEnabled(true);
    stack->setCurrentIndex(0);
}

void SegmentTableDialog::onDeleteSelectedClicked() {
    const QModelIndexList selected = tableView->selectionModel()->selectedRows();
    if (selected.isEmpty() || watcher->isRunning() || (view3DWatcher != nullptr && view3DWatcher->isRunning())) { return; }

    if (!segmentationSelectionMatchesCurrent(graphBase.get(), currentTableSegmentation, currentTableSegmentationSignal)) {
        std::cout << "[SegmentTableDebug] refusing delete because selected segmentation wiring does not match current table segmentation\n";
        QMessageBox::warning(this, "Delete Disabled",
                             "The selected segmentation state does not match the table image.\n"
                             "Please reopen Inspect Segments from the current selection.");
        return;
    }

    // Collect label IDs and source row indices from the selection.
    QList<int> sourceRows;
    sourceRows.reserve(selected.size());
    for (const QModelIndex &proxyIdx : selected) {
        const QModelIndex src = sortModel->mapToSource(proxyIdx);
        sourceRows << src.row();

        if (graphBase->pSelectedSegmentation != nullptr && graphBase->pGraph != nullptr) {
            const auto *labelItem = model->item(src.row(), SegmentTableDialog::COL_LABEL);
            if (labelItem) {
                const auto label = static_cast<dataType::SegmentIdType>(
                    labelItem->data(Qt::UserRole).toLongLong());
                graphBase->pGraph->deleteSegmentationLabel(label);
            }
        }
    }

    if (orthoViewer != nullptr && graphBase->pSelectedSegmentation != nullptr) {
        orthoViewer->refreshViewers();
    }

    // Remove the rows from the table (descending order so indices stay valid).
    std::sort(sourceRows.begin(), sourceRows.end(), std::greater<int>());
    for (int row : sourceRows) {
        model->removeRow(row);
    }

    statusLabel->setText(QString("%1 labels").arg(model->rowCount()));
    exportCsvButton->setEnabled(model->rowCount() > 0);
    updateResultsActionState();
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
    const bool busy = (watcher != nullptr && watcher->isRunning())
                      || (view3DWatcher != nullptr && view3DWatcher->isRunning());
    const bool hasSelection = tableView != nullptr
                              && tableView->selectionModel() != nullptr
                              && !tableView->selectionModel()->selectedRows().isEmpty();
    const bool selectionMatches = segmentationSelectionMatchesCurrent(
        graphBase.get(), currentTableSegmentation, currentTableSegmentationSignal);

    if (deleteSelectedButton != nullptr) {
        deleteSelectedButton->setEnabled(hasSelection && !busy && selectionMatches && graphBase != nullptr && graphBase->pGraph != nullptr);
    }
    if (view3DButton != nullptr) {
        view3DButton->setEnabled(hasSelection && !busy && selectionMatches);
    }
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

void SegmentTableDialog::onView3DSelectedClicked() {
    if (currentTableSegmentation == nullptr || view3DWatcher == nullptr || watcher == nullptr) {
        return;
    }
    if (watcher->isRunning() || view3DWatcher->isRunning()) {
        return;
    }

    auto labels = collectSelectedLabelsFor3D();
    if (labels.empty()) {
        return;
    }

    if (view3DProgressDialog != nullptr) {
        view3DProgressDialog->close();
        view3DProgressDialog->deleteLater();
        view3DProgressDialog = nullptr;
    }

    view3DProgressDialog = new QProgressDialog(this);
    view3DProgressDialog->setWindowTitle(QStringLiteral("Please Wait"));
    view3DProgressDialog->setLabelText(labels.size() == 1
                                           ? QStringLiteral("Preparing 3D view for selected segment...")
                                           : QStringLiteral("Preparing 3D view for selected segments..."));
    view3DProgressDialog->setWindowModality(Qt::WindowModal);
    view3DProgressDialog->setCancelButton(nullptr);
    view3DProgressDialog->setRange(0, 0);
    view3DProgressDialog->setMinimumDuration(0);
    view3DProgressDialog->setAutoClose(false);
    view3DProgressDialog->setAutoReset(false);
    view3DProgressDialog->setValue(0);
    view3DProgressDialog->show();

    updateResultsActionState();

    const auto segImage = currentTableSegmentation;
    view3DWatcher->setFuture(QtConcurrent::run([segImage, labels = std::move(labels)]() mutable {
        return Segment3DViewerDialog::prepareScene(segImage, std::move(labels));
    }));
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

    using FilterType = itk::LabelImageToShapeLabelMapFilter<dataType::SegmentsImageType>;
    auto filter = FilterType::New();
    filter->SetInput(segImage);
    // Roundness is computed inside the perimeter pass (SetRoundness is called
    // only when ComputePerimeter runs), so enable it whenever either is needed.
    filter->SetComputePerimeter(flags.perimeter || flags.roundness);
    filter->SetComputeOrientedBoundingBox(flags.orientedBBox);
    // ITK processes each LabelObject in parallel via its internal thread pool.
    // SetNumberOfWorkUnits defaults to hardware_concurrency; set it explicitly
    // so the parallelism is visible and reproducible.
    const itk::ThreadIdType nThreads = itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads();
    filter->SetNumberOfWorkUnits(nThreads);
    std::cout << "SegmentTableDialog: running LabelImageToShapeLabelMapFilter "
              << "with " << nThreads << " ITK work units\n";
    filter->Update();

    auto       *labelMap = filter->GetOutput();
    const auto  n        = labelMap->GetNumberOfLabelObjects();
    result.rows.reserve(n);
    const auto isolationByLabel = computeIsolationByLabel(segImage);

    for (unsigned int i = 0; i < n; ++i) {
        const auto *lo = labelMap->GetNthLabelObject(i);
        SegmentRow row;
        row.label = lo->GetLabel();

        // Centroid — always computed; needed for row-click navigation.
        const auto centroid = lo->GetCentroid();
        itk::ContinuousIndex<double, 3> ci;
        [[maybe_unused]] const bool inside =
            segImage->TransformPhysicalPointToContinuousIndex(centroid, ci);
        row.centroidX = static_cast<int>(std::round(ci[0]));
        row.centroidY = static_cast<int>(std::round(ci[1]));
        row.centroidZ = static_cast<int>(std::round(ci[2]));

        if (flags.volume)
            row.volume = static_cast<double>(lo->GetNumberOfPixels());
        if (flags.isIsolated) {
            const auto isolationIt = isolationByLabel.find(row.label);
            row.isIsolated = isolationIt == isolationByLabel.end() || isolationIt->second;
        }
        if (flags.physicalSize)
            row.physicalSize = lo->GetPhysicalSize();
        if (flags.pixelsOnBorder)
            row.pixelsOnBorder = static_cast<double>(lo->GetNumberOfPixelsOnBorder());
        if (flags.perimeterOnBorder)
            row.perimeterOnBorder = lo->GetPerimeterOnBorder();

        if (flags.bbox) {
            const auto bbox = lo->GetBoundingBox();
            row.bboxW = static_cast<double>(bbox.GetSize()[0]);
            row.bboxH = static_cast<double>(bbox.GetSize()[1]);
            row.bboxD = static_cast<double>(bbox.GetSize()[2]);
        }

        if (flags.elongation)        row.elongation       = lo->GetElongation();
        if (flags.flatness)          row.flatness         = lo->GetFlatness();
        if (flags.roundness)         row.roundness        = lo->GetRoundness();
        if (flags.equivSphRadius)    row.equivSphRadius   = lo->GetEquivalentSphericalRadius();
        if (flags.equivSphPerimeter) row.equivSphPerimeter= lo->GetEquivalentSphericalPerimeter();

        if (flags.equivEllipsoid) {
            const auto ed = lo->GetEquivalentEllipsoidDiameter();
            row.equivEllipD0 = ed[0];
            row.equivEllipD1 = ed[1];
            row.equivEllipD2 = ed[2];
        }
        if (flags.principalMoments) {
            const auto pm = lo->GetPrincipalMoments();
            row.principalMom0 = pm[0];
            row.principalMom1 = pm[1];
            row.principalMom2 = pm[2];
        }

        if (flags.perimeter)    row.perimeter    = lo->GetPerimeter();

        if (flags.orientedBBox) {
            const auto sz = lo->GetOrientedBoundingBoxSize();
            row.obboxW      = sz[0];
            row.obboxH      = sz[1];
            row.obboxD      = sz[2];
            row.obboxVolume = sz[0] * sz[1] * sz[2];
        }

        result.rows.push_back(row);
    }

    result.elapsedSeconds = timer.elapsed() / 1000.0;
    return result;
}

// ---- result arrival ---------------------------------------------------------

void SegmentTableDialog::onComputeFinished() {
    const ComputeResult result = watcher->future().result();
    populateTable(result);
    backButton->setEnabled(true);
    computeButton->setEnabled(true);
    recomputeButton->setEnabled(true);
    exportCsvButton->setEnabled(!result.rows.empty());
    statusLabel->setText(
        QString("%1 labels | Computed in %2 s")
            .arg(result.rows.size())
            .arg(result.elapsedSeconds, 0, 'f', 2));
    std::cout << "[SegmentTableDebug] computeFinished"
              << " currentTableSegmentation=" << static_cast<const void *>(currentTableSegmentation.GetPointer())
              << " currentTableSegmentationSignal=" << static_cast<const void *>(currentTableSegmentationSignal)
              << " selectedSegmentation=" << static_cast<const void *>(graphBase->pSelectedSegmentation.GetPointer())
              << " selectedSegmentationSignal=" << static_cast<const void *>(graphBase->pSelectedSegmentationSignal)
              << " rows=" << result.rows.size()
              << " elapsedSeconds=" << result.elapsedSeconds
              << "\n";
    updateResultsActionState();
    emit computeFinishedDebug();
}

void SegmentTableDialog::onView3DPreparationFinished() {
    if (view3DProgressDialog != nullptr) {
        view3DProgressDialog->close();
        view3DProgressDialog->deleteLater();
        view3DProgressDialog = nullptr;
    }

    try {
        auto preparedScene = view3DWatcher->future().result();
        const auto segImage = currentTableSegmentation;
        if (preparedScene.hasCombinedMesh || !preparedScene.meshes.empty()) {
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
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        } else {
            QMessageBox::information(this, "3D View", "No 3D surface could be generated for the selected labels.");
        }
    } catch (const std::exception &e) {
        QMessageBox::critical(this, "3D View Error", QString::fromStdString(e.what()));
    } catch (...) {
        QMessageBox::critical(this, "3D View Error", "Unknown error while preparing 3D view.");
    }

    updateResultsActionState();
}

// ---- table population -------------------------------------------------------

void SegmentTableDialog::populateTable(const ComputeResult &result) {
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
    updateColumnVisibility(result.flags);
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

void SegmentTableDialog::updateColumnVisibility(const FeatureFlags &f) {
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
        tableView->setColumnHidden(SegmentTableDialog::COL_CZ, false);
    }
    if (f.bbox) {
        tableView->setColumnHidden(SegmentTableDialog::COL_BBOX_W, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_BBOX_H, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_BBOX_D, false);
    }
    if (f.elongation)        tableView->setColumnHidden(SegmentTableDialog::COL_ELONGATION, false);
    if (f.flatness)          tableView->setColumnHidden(SegmentTableDialog::COL_FLATNESS, false);
    if (f.roundness)         tableView->setColumnHidden(SegmentTableDialog::COL_ROUNDNESS, false);
    if (f.equivSphRadius)    tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_SPH_RADIUS, false);
    if (f.equivSphPerimeter) tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_SPH_PERIM, false);
    if (f.equivEllipsoid) {
        tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_ELLIP_D0, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_ELLIP_D1, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_EQUIV_ELLIP_D2, false);
    }
    if (f.principalMoments) {
        tableView->setColumnHidden(SegmentTableDialog::COL_PRINCIPAL_MOM0, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_PRINCIPAL_MOM1, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_PRINCIPAL_MOM2, false);
    }
    if (f.perimeter)    tableView->setColumnHidden(SegmentTableDialog::COL_PERIMETER, false);
    if (f.orientedBBox) {
        tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_W, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_H, false);
        tableView->setColumnHidden(SegmentTableDialog::COL_OBBOX_D, false);
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

void SegmentTableDialog::onExportCsvClicked() {
    const QString path = QFileDialog::getSaveFileName(
        this, "Export CSV", QString(), "CSV files (*.csv)");
    if (path.isEmpty()) { return; }

    QFile file(path);
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

    statusLabel->setText("Exported to " + path);
}
