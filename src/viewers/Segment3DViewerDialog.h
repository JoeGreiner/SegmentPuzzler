#ifndef SEGMENT3DVIEWERDIALOG_H
#define SEGMENT3DVIEWERDIALOG_H

#include <QDialog>
#include <QString>
#include <array>
#include <functional>
#include <map>
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
class CutStrokeOverlay;

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

    struct CutApplyResult {
        bool mutated = false;
        QString message;
    };

    struct CutSessionConfig {
        TaskRunner *taskRunner = nullptr;
        std::function<QString(const Projected3DCutRequest &)> preflightWarning;
        std::function<CutApplyResult(const Projected3DCutRequest &)> applyCut;
        QString progressText = QStringLiteral("Applying 3D cut...");
    };

    struct SingleLabelSessionConfig {
        std::vector<dataType::SegmentIdType> labels;
        RequestLabelHandler requestLabel;
        DeleteLabelHandler deleteLabel;
        LabelActivatedHandler labelActivated;
    };

    explicit Segment3DViewerDialog(PreparedScene preparedScene,
                                   CutSessionConfig cutSession,
                                   QWidget *parent = nullptr,
                                   int launchSliceAxis = -1);
    explicit Segment3DViewerDialog(PreparedScene preparedScene,
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
    };

    void cycleSegmentColors();
    void applyColorCycle(std::vector<SegmentActorInfo> &actors) const;
    void stepExplodeSlider(int direction);
    QHBoxLayout *ensureControlsRow();
    void addOrbitControls(QHBoxLayout *controlsRow);
    void requestAdjacentLabel(int direction);
    void activateOrRequestLabel(dataType::SegmentIdType labelId);
    bool applyPreparedScene(const PreparedScene &preparedScene);
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
    void beginCutDrawing();
    void clearCutStroke();
    void applyProjectedCut();
    void showCutHelp();
    void updateCutUiState();
    Projected3DCutRequest buildProjected3DCutRequest() const;
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
    QPushButton *m_drawCutButton = nullptr;
    QPushButton *m_clearCutButton = nullptr;
    QPushButton *m_applyCutButton = nullptr;
    QWidget *m_controlsWidget = nullptr;
    QVTKOpenGLNativeWidget *m_vtkWidget = nullptr;
    CutStrokeOverlay *m_cutOverlay = nullptr;
    vtkSmartPointer<vtkOrientationMarkerWidget> m_orientationWidget;
    CutSessionConfig m_cutSession;
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
    bool m_cutDrawModeActive = false;
    bool m_cutApplyInFlight = false;
    bool m_singleLabelNavigationBusy = false;
    bool m_deleteModeActive = false;
};

#endif // SEGMENT3DVIEWERDIALOG_H
