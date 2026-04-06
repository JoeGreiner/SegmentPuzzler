#ifndef SEGMENTCOUPLER_MAINWINDOW_H
#define SEGMENTCOUPLER_MAINWINDOW_H


#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <memory>

#include "src/controllers/SignalControl.h"
#include "src/viewers/OrthoViewer.h"

class TaskRunner;

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

private:
    OrthoViewer *myOrthowindow;
    QMenu *addDataMenu;
    QMenu *boundariesMenu;
    QMenu *refinementsMenu;
    QMenu *segmentationsMenu;
    QMenu *helpMenu;
    QMenu *goToMenu;

    QAction *openHotkeysAction;
    QAction *loadSampleSegmentationAction;
    std::unique_ptr<Graph> graph;
    std::shared_ptr<GraphBase> graphBase;
    std::unique_ptr<TaskRunner> taskRunner;

    void installInitialFileDropHandling();
    void registerDropTarget(QWidget *widget);
};

#endif //SEGMENTCOUPLER_MAINWINDOW_H
