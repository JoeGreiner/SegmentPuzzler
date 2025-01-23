#ifndef SEGMENTCOUPLER_MAINWINDOW_H
#define SEGMENTCOUPLER_MAINWINDOW_H


#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>

#include "src/controllers/SignalControl.h"
#include "src/viewers/OrthoViewer.h"

class MainWindow : public QMainWindow {
Q_OBJECT


public:
    MainWindow();
    SignalControl* mySignalControl;

public slots:
    void loadSegmentationSample();
    void receiveStatusMessage(const QString& string);

private slots:
    void showHotkeys();

private:
    OrthoViewer *myOrthowindow;
    QMenu *helpMenu;
    QMenu *viewerMenu;
    QMenu *sampleDataMenu;

    QAction *openHotkeysAction;
    QAction *loadSampleSegmentationAction;
    Graph * graph;
    std::shared_ptr<GraphBase> graphBase;
};

#endif //SEGMENTCOUPLER_MAINWINDOW_H
