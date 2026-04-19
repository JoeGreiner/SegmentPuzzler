#ifndef SEGMENT3DVIEWERDIALOG_H
#define SEGMENT3DVIEWERDIALOG_H

#include <QDialog>
#include <QString>
#include <array>
#include <functional>
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
class QCheckBox;
class QEvent;
class QPushButton;
class QSlider;
class QWidget;
class TaskRunner;
class CutStrokeOverlay;

class Segment3DViewerDialog : public QDialog {
    Q_OBJECT

public:
    using LabelWithColor = std::pair<dataType::SegmentIdType, quint32>;
    using NavigateToLabelHandler = std::function<void(dataType::SegmentIdType)>;

    struct PreparedMesh {
        dataType::SegmentIdType labelId = 0;
        vtkSmartPointer<vtkPolyData> polyData;
        quint32 lutColor = 0xAAAAAA;
        bool useCellScalars = false;
        std::array<double, 3> centerWorld{0.0, 0.0, 0.0};
    };

    struct PreparedScene {
        QString windowTitle;
        dataType::SegmentIdType targetLabelId = 0;
        PreparedMesh combinedMesh;
        bool hasCombinedMesh = false;
        std::vector<PreparedMesh> meshes;
        std::array<double, 3> sceneCenterWorld{0.0, 0.0, 0.0};
        double sceneExtent = 1.0;
        // Stored for deferred background split when the user activates the explode toggle
        vtkSmartPointer<vtkPolyData> splitSourcePolyData;
        std::vector<LabelWithColor> splitLabels;
    };

    static PreparedScene prepareScene(
        dataType::SegmentsImageType::Pointer segImage,
        std::vector<LabelWithColor> labels);
    static PreparedScene prepareScene(
        dataType::SegmentsImageType::Pointer segImage,
        std::vector<LabelWithColor> labels,
        Roi requestedBounds);

    struct CutApplyResult {
        bool mutated = false;
        QString message;
    };

    struct CutSessionConfig {
        TaskRunner *taskRunner = nullptr;
        std::function<CutApplyResult(const Projected3DCutRequest &)> applyCut;
        QString progressText = QStringLiteral("Applying 3D cut...");
    };

    explicit Segment3DViewerDialog(PreparedScene preparedScene,
                                   CutSessionConfig cutSession,
                                   QWidget *parent = nullptr);
    explicit Segment3DViewerDialog(PreparedScene preparedScene,
                                   QWidget *parent = nullptr);

    void setNavigateToLabelHandler(NavigateToLabelHandler handler);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct ActorInfo {
        vtkSmartPointer<vtkActor> actor;
        dataType::SegmentIdType labelId = 0;
        std::array<double, 3> centerWorld{0.0, 0.0, 0.0};
    };

    void stepExplodeSlider(int direction);
    void onExplodeToggled(bool checked);
    void activateExplodeActors(const std::vector<PreparedMesh> &meshes);
    void beginCutDrawing();
    void clearCutStroke();
    void applyProjectedCut();
    void showCutHelp();
    void updateCutUiState();
    Projected3DCutRequest buildProjected3DCutRequest() const;
    bool tryNavigateToPickedLabel(int pickX,
                                  int pickY,
                                  Qt::KeyboardModifiers modifiers,
                                  const char *sourceTag);
    void handleInteractorLeftButtonPress();

    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkActor> m_combinedActor;
    std::vector<ActorInfo> m_explodeActors;
    std::array<double, 3> m_sceneCenterWorld{};
    double m_sceneExtent = 1.0;
    dataType::SegmentIdType m_targetLabelId = 0;
    vtkSmartPointer<vtkPolyData> m_splitSourcePolyData;
    std::vector<LabelWithColor> m_splitLabels;

    QSlider *m_explodeSlider = nullptr;
    QCheckBox *m_explodeToggle = nullptr;
    QPushButton *m_drawCutButton = nullptr;
    QPushButton *m_clearCutButton = nullptr;
    QPushButton *m_applyCutButton = nullptr;
    QWidget *m_controlsWidget = nullptr;
    QVTKOpenGLNativeWidget *m_vtkWidget = nullptr;
    CutStrokeOverlay *m_cutOverlay = nullptr;
    vtkSmartPointer<vtkOrientationMarkerWidget> m_orientationWidget;
    TaskRunner *m_ownedTaskRunner = nullptr;
    TaskRunner *m_taskRunner = nullptr;
    CutSessionConfig m_cutSession;
    NavigateToLabelHandler m_navigateToLabelHandler;
    bool m_cutDrawModeActive = false;
    bool m_cutApplyInFlight = false;
};

#endif // SEGMENT3DVIEWERDIALOG_H
