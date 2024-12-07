#ifndef SEGMENTCOUPLER_MAINWINDOWWATERSHEDCONTROL_H
#define SEGMENTCOUPLER_MAINWINDOWWATERSHEDCONTROL_H


#include <QMainWindow>
#include <QMenuBar>
#include <src/viewers/OrthoViewer.h>
#include <QMessageBox>
#include <src/controllers/watershedControl.h>
#include "SignalControl.h"


class MainWindowWatershedControl : public QMainWindow {
    Q_OBJECT
public:
    MainWindowWatershedControl();
    WatershedControl* myWatershedControl;

    void setLinkedSignalControl(SignalControl* linkedSignalControlIn);

public slots:
    void receiveStatusMessage(QString string);
    void closeFromExternalSignal();

private:
    OrthoViewer *myOrthowindow;
    SignalControl* linkedSignalControl;

};



#endif //SEGMENTCOUPLER_MAINWINDOWWATERSHEDCONTROL_H
