#ifndef SEGMENTPUZZLER_SEGMENTTABLEDIALOG_H
#define SEGMENTPUZZLER_SEGMENTTABLEDIALOG_H

#include <QDialog>
#include <QFutureWatcher>
#include <memory>
#include <vector>
#include "src/segment_handling/graphBase.h"
#include "src/file_definitions/dataTypes.h"
#include "src/viewers/Segment3DViewerDialog.h"
#include "src/viewers/itkSignalBase.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressDialog;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItemModel;
class QTableView;
class OrthoViewer;
class QModelIndex;

class SegmentTableDialog : public QDialog {
    Q_OBJECT
public:
    explicit SegmentTableDialog(std::shared_ptr<GraphBase> graphBase,
                                OrthoViewer *orthoViewer,
                                QWidget *parent = nullptr);
    ~SegmentTableDialog() override;

    void setQuickComputeMode();
    void startCompute(dataType::SegmentsImageType::Pointer segImg = nullptr);

    // ---- What to compute (mirrors the setup-page checkboxes) ----
    struct FeatureFlags {
        bool volume            = true;
        bool isIsolated        = true;
        bool physicalSize      = false;
        bool pixelsOnBorder    = false;
        bool perimeterOnBorder = false;
        bool centroid          = true;   // always computed; flag controls column visibility only
        bool bbox              = false;
        bool elongation        = true;
        bool flatness          = true;
        bool roundness         = true;
        bool equivSphRadius    = true;
        bool equivSphPerimeter = false;
        bool equivEllipsoid    = false;
        bool principalMoments  = false;
        bool perimeter         = false;  // requires SetComputePerimeter
        bool orientedBBox      = false;  // requires SetComputeOrientedBoundingBox
    };

    // ---- Per-label result row; -1.0 = not computed ----
    struct SegmentRow {
        dataType::SegmentIdType label = 0;
        // Centroid always computed (needed for navigation even if column is hidden)
        int centroidX = 0, centroidY = 0, centroidZ = 0;

        double volume = -1, physicalSize = -1;
        bool isIsolated = true;
        double pixelsOnBorder = -1, perimeterOnBorder = -1;
        double bboxW = -1, bboxH = -1, bboxD = -1;
        double elongation = -1, flatness = -1, roundness = -1;
        double equivSphRadius = -1, equivSphPerimeter = -1;
        double equivEllipD0 = -1, equivEllipD1 = -1, equivEllipD2 = -1;
        double principalMom0 = -1, principalMom1 = -1, principalMom2 = -1;
        double perimeter = -1;
        double obboxW = -1, obboxH = -1, obboxD = -1, obboxVolume = -1;
    };

    struct ComputeResult {
        std::vector<SegmentRow> rows;
        FeatureFlags flags;
        double elapsedSeconds = 0.0;
    };

    // ---- Column definitions (fixed set; visibility is dynamic) ----
    enum Columns {
        COL_LABEL               = 0,
        COL_VOLUME              = 1,
        COL_IS_ISOLATED         = 2,
        COL_PHYSICAL_SIZE       = 3,
        COL_PIXELS_ON_BORDER    = 4,
        COL_PERIMETER_ON_BORDER = 5,
        COL_CX                  = 6,
        COL_CY                  = 7,
        COL_CZ                  = 8,
        COL_BBOX_W              = 9,
        COL_BBOX_H              = 10,
        COL_BBOX_D              = 11,
        COL_ELONGATION          = 12,
        COL_FLATNESS            = 13,
        COL_ROUNDNESS           = 14,
        COL_EQUIV_SPH_RADIUS    = 15,
        COL_EQUIV_SPH_PERIM     = 16,
        COL_EQUIV_ELLIP_D0      = 17,
        COL_EQUIV_ELLIP_D1      = 18,
        COL_EQUIV_ELLIP_D2      = 19,
        COL_PRINCIPAL_MOM0      = 20,
        COL_PRINCIPAL_MOM1      = 21,
        COL_PRINCIPAL_MOM2      = 22,
        COL_PERIMETER           = 23,
        COL_OBBOX_W             = 24,
        COL_OBBOX_H             = 25,
        COL_OBBOX_D             = 26,
        COL_OBBOX_VOLUME        = 27,
        COL_COUNT               = 28
    };

    // Runs on a worker thread — must not touch QWidgets.
    static ComputeResult computeFeatures(
        dataType::SegmentsImageType::Pointer segImg,
        FeatureFlags flags);

signals:
    void computeFinishedDebug();

private slots:
    void onComputeClicked();
    void onComputeFinished();
    void onBackClicked();
    void onDeleteSelectedClicked();
    void onSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void onExportCsvClicked();
    void onView3DSelectedClicked();
    void onView3DPreparationFinished();

private:
    // ---- Column definitions (fixed set; visibility is dynamic) ----

    QWidget *createSetupPage();
    QWidget *createResultsPage();
    FeatureFlags collectFlags() const;
    void populateTable(const ComputeResult &result);
    void applyColumnColoring();
    void updateColumnVisibility(const FeatureFlags &flags);
    void navigateTo(int x, int y, int z);
    void setAllChecked(bool checked);
    void updateResultsActionState();
    std::vector<std::pair<dataType::SegmentIdType, quint32>> collectSelectedLabelsFor3D() const;

    // ---- Shared state ----
    std::shared_ptr<GraphBase> graphBase;
    OrthoViewer *orthoViewer;
    dataType::SegmentsImageType::Pointer currentTableSegmentation;
    itkSignalBase *currentTableSegmentationSignal = nullptr;
    QStackedWidget *stack = nullptr;

    // ---- Setup page ----
    QCheckBox *cbVolume = nullptr, *cbIsIsolated = nullptr, *cbPhysicalSize = nullptr;
    QCheckBox *cbPixelsOnBorder = nullptr, *cbPerimeterOnBorder = nullptr;
    QCheckBox *cbCentroid = nullptr, *cbBBox = nullptr;
    QCheckBox *cbElongation = nullptr, *cbFlatness = nullptr, *cbRoundness = nullptr;
    QCheckBox *cbEquivSphRadius = nullptr, *cbEquivSphPerimeter = nullptr;
    QCheckBox *cbEquivEllipsoid = nullptr, *cbPrincipalMoments = nullptr;
    QCheckBox *cbPerimeter = nullptr, *cbOrientedBBox = nullptr;
    QPushButton *computeButton = nullptr;

    // ---- Results page ----
    QPushButton *backButton = nullptr;
    QPushButton *recomputeButton = nullptr;
    QPushButton *deleteSelectedButton = nullptr;
    QPushButton *view3DButton = nullptr;
    QPushButton *exportCsvButton = nullptr;
    QLabel *statusLabel = nullptr;
    QLineEdit *searchEdit = nullptr;
    QTableView *tableView = nullptr;
    QStandardItemModel *model = nullptr;
    QSortFilterProxyModel *sortModel = nullptr;

    QFutureWatcher<ComputeResult> *watcher = nullptr;
    QFutureWatcher<Segment3DViewerDialog::PreparedScene> *view3DWatcher = nullptr;
    QProgressDialog *view3DProgressDialog = nullptr;
};

#endif // SEGMENTPUZZLER_SEGMENTTABLEDIALOG_H
