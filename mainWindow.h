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

class MainWindow : public QMainWindow {
Q_OBJECT


public:
    MainWindow();
    ~MainWindow() override;
    SignalControl* mySignalControl;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

public slots:
    void loadSegmentationSample();
    void receiveStatusMessage(const QString& string);

private slots:
    void showHotkeys();
    void showSegmentTable();
    void arm3DWorkingSegmentCut();

private:
    OrthoViewer *myOrthowindow;
    QMenu *addDataMenu;
    QMenu *boundariesMenu;
    QMenu *refinementsMenu;
    QMenu *segmentationsMenu;
    QMenu *settingsMenu;
    QMenu *helpMenu;
    QMenu *goToMenu;

    QAction *openHotkeysAction;
    QAction *loadSampleSegmentationAction;
    QAction *showSegmentTableAction = nullptr;
    QAction *splitWorkingSegment3DCutAction = nullptr;
    std::unique_ptr<Graph> graph;
    std::shared_ptr<GraphBase> graphBase;
    std::unique_ptr<TaskRunner> taskRunner;
    QPointer<SegmentTableDialog> segmentTableDialog;

    void installInitialFileDropHandling();
    void registerDropTarget(QWidget *widget);
    void update3DWorkingSegmentCutActionState();
};

#endif //SEGMENTCOUPLER_MAINWINDOW_H
