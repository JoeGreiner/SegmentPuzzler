#ifndef SEGMENTCOUPLER_MAINWINDOW_H
#define SEGMENTCOUPLER_MAINWINDOW_H


#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <memory>

#include "src/controllers/SignalControl.h"
#include "src/viewers/OrthoViewer.h"

class TaskRunner;
class SegmentTableDialog;
class SelectedSegmentationAutosave;
class QCloseEvent;

class MainWindow : public QMainWindow {
Q_OBJECT


public:
    MainWindow();
    ~MainWindow() override;
    SignalControl* mySignalControl;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

public slots:
    void loadSegmentationSample();
    void receiveStatusMessage(const QString& string);

private slots:
    void showHotkeys();
    void showSegmentTable();
    void showLayerRenderOrder();
    void arm3DSegmentSplit();
    void exportDebugInformation();
    void showLoggingSettings();
    void showRecoverySettings();
    void showVoxelSpacingSettings();

private:
    enum class PendingQuit {
        None,
        WindowClose,
        ApplicationQuit
    };

    OrthoViewer *myOrthowindow;
    QMenu *dataMenu;
    QMenu *segmentationMenu;
    QMenu *viewMenu;
    QMenu *settingsMenu;
    QMenu *helpMenu;

    QAction *openHotkeysAction;
    QAction *loadSampleSegmentationAction;
    QAction *showSegmentTableAction = nullptr;
    QAction *splitSegment3DAction = nullptr;
    QAction *voxelSpacingAction = nullptr;
    QAction *recoverySettingsAction = nullptr;
    std::unique_ptr<Graph> graph;
    std::shared_ptr<GraphBase> graphBase;
    std::unique_ptr<TaskRunner> taskRunner;
    std::unique_ptr<SelectedSegmentationAutosave> selectedSegmentationAutosave;
    QPointer<SegmentTableDialog> segmentTableDialog;
    PendingQuit pendingQuit = PendingQuit::None;
    bool discardQuitConfirmed = false;
    bool quitApproved = false;

    void installInitialFileDropHandling();
    void registerDropTarget(QWidget *widget);
    bool confirmOrDeferQuit(PendingQuit requestedQuit);
    void continuePendingQuit();
    void update3DSegmentSplitActionState();
    void exportViewSeries(AnnotationSliceViewer *viewer);
};

#endif //SEGMENTCOUPLER_MAINWINDOW_H
