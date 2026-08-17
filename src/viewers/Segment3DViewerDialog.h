#ifndef SEGMENT3DVIEWERDIALOG_H
#define SEGMENT3DVIEWERDIALOG_H

#include <QDialog>
#include <QString>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <utility>
#include <vtkActor.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/Projected3DCut.h"
#include "src/segment_handling/SeededWatershedSplit.h"
#include "src/utils/roi.h"

class vtkOrientationMarkerWidget;
class QVTKOpenGLNativeWidget;
class QEvent;
class QCheckBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QShowEvent;
class QSlider;
class QTimer;
class QWidget;
class TaskRunner;
class StrokeOverlay;

class Segment3DViewerDialog : public QDialog {
    Q_OBJECT

public:
    using LabelWithColor = std::pair<dataType::SegmentIdType, quint32>;
    using NavigateToLabelHandler = std::function<void(dataType::SegmentIdType)>;
    using DeleteLabelHandler = std::function<bool(dataType::SegmentIdType)>;
    using RequestLabelHandler = std::function<void(dataType::SegmentIdType, const Roi &)>;
    using LabelActivatedHandler = std::function<void(dataType::SegmentIdType)>;

    struct CameraOrientation {
        std::array<double, 3> lookDirection{0.0, 0.0, 1.0};
        std::array<double, 3> viewUp{0.0, 1.0, 0.0};
    };

    struct PreparedMesh {
        dataType::SegmentIdType labelId = 0;
        vtkSmartPointer<vtkPolyData> polyData;
        quint32 lutColor = 0xAAAAAA;
        std::array<double, 3> centerWorld{0.0, 0.0, 0.0};
        bool touchesImageBoundary = false;
    };

    struct PreparedScene {
        QString windowTitle;
        dataType::SegmentIdType targetLabelId = 0;
        std::vector<PreparedMesh> meshes;
        std::vector<dataType::SegmentIdType> navigationLabels;
        std::map<dataType::SegmentIdType, Roi> navigationBounds;
        bool navigationCatalogComplete = false;
        std::array<double, 3> sceneCenterWorld{0.0, 0.0, 0.0};
        double sceneExtent = 1.0;
    };

    static PreparedScene prepareScene(
        dataType::SegmentsImageType::Pointer segImage,
        std::vector<LabelWithColor> labels);
    static PreparedScene prepareScene(
        dataType::SegmentsImageType::Pointer segImage,
        std::vector<LabelWithColor> labels,
        Roi requestedBounds);
    static PreparedScene prepareSingleLabelSlideshowScene(
        dataType::SegmentsImageType::Pointer segImage,
        LabelWithColor label);
    static PreparedScene prepareSingleLabelSlideshowScene(
        dataType::SegmentsImageType::Pointer segImage,
        LabelWithColor label,
        const Roi &cachedBounds);
    static PreparedScene prepareAllLabelsScene(
        dataType::SegmentsImageType::Pointer segImage,
        std::vector<quint32> labelColors);
    // Expects triangulated vtkSurfaceNets3D output with BoundaryLabels.
    // Each result shares combinedPolyData's points to avoid duplicate VBOs.
    // Treat points and cells as immutable. GetBounds() covers all shared points;
    // use centerWorld or vtkPolyData::GetCellsBounds() for label-local bounds.
    static std::vector<PreparedMesh> prepareExplodedMeshes(
        vtkPolyData *combinedPolyData,
        const std::vector<LabelWithColor> &labels);
    static std::optional<CameraOrientation> cameraOrientationForSliceAxis(int sliceAxis);

    struct SingleLabelSessionConfig {
        std::vector<dataType::SegmentIdType> labels;
        RequestLabelHandler requestLabel;
        DeleteLabelHandler deleteLabel;
        LabelActivatedHandler labelActivated;
    };

    struct SplitApplyResult {
        bool mutated = false;
        QString message;
    };

    struct SplitSessionConfig {
        TaskRunner *taskRunner = nullptr;
        std::shared_ptr<segment_puzzler::SeededWatershedSplitSession> session;
        std::function<SplitApplyResult(
            const segment_puzzler::SeededWatershedSplitResult &)> applySplit;
        std::function<SplitApplyResult(
            const Projected3DCutResult &)> applyProjectedCut;
        QString progressText = QStringLiteral("Applying seeded split...");
        QString projectedCutProgressText = QStringLiteral("Applying projected cut...");
    };

    explicit Segment3DViewerDialog(PreparedScene preparedScene,
                                   QWidget *parent = nullptr,
                                   int launchSliceAxis = -1);
    explicit Segment3DViewerDialog(PreparedScene preparedScene,
                                   SplitSessionConfig splitSession,
                                   QWidget *parent = nullptr,
                                   int launchSliceAxis = -1);

    void setNavigateToLabelHandler(NavigateToLabelHandler handler);
    void setDeleteLabelHandler(DeleteLabelHandler handler);
    void setSingleLabelSession(SingleLabelSessionConfig session);
    bool acceptPreparedScene(PreparedScene preparedScene);
    bool rejectPreparedScene(dataType::SegmentIdType labelId);
    void presentInFront();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    static PreparedScene prepareScene(
        dataType::SegmentsImageType::Pointer segImage,
        std::vector<LabelWithColor> labels,
        Roi requestedBounds,
        bool allLabelsInImage);

    struct SegmentActorInfo {
        vtkSmartPointer<vtkActor> actor;
        dataType::SegmentIdType labelId = 0;
        std::array<double, 3> centerWorld{0.0, 0.0, 0.0};
        bool touchesImageBoundary = false;
    };

    struct SeedRayHit {
        segment_puzzler::SeededWatershedSplitSession::IndexType index;
        std::array<std::array<double, 3>, 2> rayEndpoints{};
    };

    enum class SplitMethod {
        SeededWatershed,
        ProjectedCut
    };

    void cycleSegmentColors();
    void applyColorCycle(std::vector<SegmentActorInfo> &actors) const;
    void stepExplodeSlider(int direction);
    QHBoxLayout *ensureControlsRow();
    void addOrbitControls(QHBoxLayout *controlsRow);
    void requestAdjacentLabel(int direction);
    void activateOrRequestLabel(dataType::SegmentIdType labelId);
    bool applyPreparedScene(const PreparedScene &preparedScene);
    bool replaceSegmentMeshes(const PreparedScene &preparedScene, bool resetCamera);
    void cachePreparedScene(PreparedScene preparedScene);
    void prefetchAdjacentLabel();
    void prunePreparedSceneCache();
    void setSingleLabelNavigationBusy(bool busy,
                                      dataType::SegmentIdType pendingLabelId = 0);
    void setOrbitEnabled(bool enabled);
    void advanceOrbit();
    bool deleteCurrentLabel();
    bool removeLabelActor(dataType::SegmentIdType labelId);
    void updateSingleLabelNavigationUiState();
    void armSeedPlacement(int seedNumber);
    bool cancelActiveSplitInput();
    void beginSplitLineDrawing();
    bool updateSplitLineSeedPreview();
    void confirmSplitLineSeeds();
    std::optional<SeedRayHit> seedAlongDisplayRay(double displayX, double displayY) const;
    bool placeSeedAt(int pickX, int pickY);
    void showSeedActors(const segment_puzzler::SeededSplitSeedGroups &seeds);
    void resetSplit();
    void updateSeededSplitSmoothing();
    void previewSeededSplitIfReady();
    void previewSeededSplit();
    void applyActiveSplit();
    void applySeededSplit();
    void beginProjectedCutDrawing();
    void previewProjectedCut();
    void applyProjectedCutPreview();
    void discardProjectedCut();
    void exportSeededSplitDebugBundle();
    void setSeededSplitStatus(const QString &text, bool warning = false);
    void updateSeededSplitUiState();
    void restoreSeededSplitFocus();
    Projected3DCutRequest buildProjectedStrokeRequest() const;
    bool tryHandlePickedLabelInteraction(int pickX,
                                         int pickY,
                                         Qt::KeyboardModifiers modifiers,
                                         const char *sourceTag);
    void handleInteractorLeftButtonPress();
    void raiseAndRequestActivation();
    void applyInitialCameraOrientation(int launchSliceAxis);
    void finishInitialRender();

    vtkSmartPointer<vtkRenderer> m_renderer;
    std::vector<SegmentActorInfo> m_segmentActors;
    std::array<double, 3> m_sceneCenterWorld{};
    double m_sceneExtent = 1.0;
    dataType::SegmentIdType m_targetLabelId = 0;

    QSlider *m_explodeSlider = nullptr;
    QHBoxLayout *m_controlsRow = nullptr;
    QPushButton *m_previousLabelButton = nullptr;
    QPushButton *m_nextLabelButton = nullptr;
    QLabel *m_navigationLabel = nullptr;
    QCheckBox *m_orbitCheckBox = nullptr;
    QDoubleSpinBox *m_orbitSpeedSpinBox = nullptr;
    QTimer *m_orbitTimer = nullptr;
    std::array<QPushButton *, 2> m_seedButtons{nullptr, nullptr};
    QPushButton *m_resetSplitButton = nullptr;
    QPushButton *m_splitLineButton = nullptr;
    QPushButton *m_confirmLineSeedsButton = nullptr;
    QSlider *m_seedDistanceSlider = nullptr;
    QLabel *m_seedDistanceLabel = nullptr;
    QSlider *m_lineSamplingSlider = nullptr;
    QLabel *m_lineSamplingLabel = nullptr;
    QCheckBox *m_connectSeedsCheckBox = nullptr;
    QCheckBox *m_compactWatershedCheckBox = nullptr;
    QCheckBox *m_allowDisconnectedCheckBox = nullptr;
    QSlider *m_smoothingSlider = nullptr;
    QLabel *m_smoothingLabel = nullptr;
    QTimer *m_smoothingUpdateTimer = nullptr;
    QPushButton *m_applySplitButton = nullptr;
    QPushButton *m_projectedCutButton = nullptr;
    QLabel *m_seedStatusLabel = nullptr;
    QWidget *m_controlsWidget = nullptr;
    QVTKOpenGLNativeWidget *m_vtkWidget = nullptr;
    StrokeOverlay *m_strokeOverlay = nullptr;
    vtkSmartPointer<vtkRenderer> m_seedRenderer;
    vtkSmartPointer<vtkOrientationMarkerWidget> m_orientationWidget;
    SplitSessionConfig m_seededSplitSession;
    PreparedScene m_originalSeededSplitScene;
    std::optional<segment_puzzler::SeededWatershedSplitResult> m_seededSplitPreview;
    std::optional<Projected3DCutResult> m_projectedCutPreview;
    std::optional<segment_puzzler::SeededWatershedSplitResult> m_lastSeededSplitResult;
    std::optional<segment_puzzler::SeededSplitSeedGroups> m_lastSeededSplitSeeds;
    segment_puzzler::SeededSplitSeedGroups m_seedIndices;
    segment_puzzler::SeededSplitSeedGroups m_pendingLineSeeds;
    std::array<std::vector<vtkSmartPointer<vtkActor>>, 2> m_seedActors;
    NavigateToLabelHandler m_navigateToLabelHandler;
    DeleteLabelHandler m_deleteLabelHandler;
    RequestLabelHandler m_requestLabelHandler;
    LabelActivatedHandler m_labelActivatedHandler;
    std::vector<dataType::SegmentIdType> m_navigationLabels;
    std::map<dataType::SegmentIdType, Roi> m_navigationBounds;
    std::map<dataType::SegmentIdType, PreparedScene> m_preparedSceneCache;
    std::set<dataType::SegmentIdType> m_unavailableSceneLabels;
    dataType::SegmentIdType m_pendingSceneLabel = 0;
    dataType::SegmentIdType m_activateWhenReadyLabel = 0;
    quint32 m_colorCycleSeed = 0;
    int m_launchSliceAxis = -1;
    bool m_initialFrameScheduled = false;
    bool m_initialFrameRendered = false;
    int m_activeSeed = -1;
    SplitMethod m_activeSplitMethod = SplitMethod::SeededWatershed;
    bool m_seededSplitBusy = false;
    bool m_seededSplitSmoothingPending = false;
    bool m_splitLineDrawModeActive = false;
    bool m_projectedCutDrawModeActive = false;
    bool m_havePendingLineSeeds = false;
    bool m_pendingLineSeedsValid = false;
    bool m_singleLabelNavigationBusy = false;
    bool m_deleteModeActive = false;
};

#endif // SEGMENT3DVIEWERDIALOG_H
