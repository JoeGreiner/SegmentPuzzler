#ifndef SEGMENT3DVIEWERDIALOG_H
#define SEGMENT3DVIEWERDIALOG_H

#include <QDialog>
#include <QString>
#include <array>
#include <vector>
#include <utility>
#include <vtkActor.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include "src/file_definitions/dataTypes.h"

class vtkOrientationMarkerWidget;
class QVTKOpenGLNativeWidget;
class QCheckBox;
class QSlider;
class QWidget;
class TaskRunner;

class Segment3DViewerDialog : public QDialog {
    Q_OBJECT

public:
    using LabelWithColor = std::pair<dataType::SegmentIdType, quint32>;

    struct PreparedMesh {
        dataType::SegmentIdType labelId = 0;
        vtkSmartPointer<vtkPolyData> polyData;
        quint32 lutColor = 0xAAAAAA;
        bool useCellScalars = false;
        std::array<double, 3> centerWorld{0.0, 0.0, 0.0};
    };

    struct PreparedScene {
        QString windowTitle;
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

    explicit Segment3DViewerDialog(PreparedScene preparedScene,
                                   QWidget *parent = nullptr);

private:
    struct ActorInfo {
        vtkSmartPointer<vtkActor> actor;
        std::array<double, 3> centerWorld{0.0, 0.0, 0.0};
    };

    void stepExplodeSlider(int direction);
    void onExplodeToggled(bool checked);
    void activateExplodeActors(const std::vector<PreparedMesh> &meshes);

    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkActor> m_combinedActor;
    std::vector<ActorInfo> m_explodeActors;
    std::array<double, 3> m_sceneCenterWorld{};
    double m_sceneExtent = 1.0;
    vtkSmartPointer<vtkPolyData> m_splitSourcePolyData;
    std::vector<LabelWithColor> m_splitLabels;

    QSlider *m_explodeSlider = nullptr;
    QCheckBox *m_explodeToggle = nullptr;
    QWidget *m_controlsWidget = nullptr;
    QVTKOpenGLNativeWidget *m_vtkWidget = nullptr;
    vtkSmartPointer<vtkOrientationMarkerWidget> m_orientationWidget;
    TaskRunner *m_taskRunner = nullptr;
};

#endif // SEGMENT3DVIEWERDIALOG_H
