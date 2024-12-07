
// read in boundary data
// run watershed pipeline:
// * threshold
// * distancemap
// * minima
// * marker controlled watershed


#include <QApplication>
#include <QtWidgets>
#include <src/viewers/fileIO.h>
#include <QObject>
#include <src/viewers/SliceViewer.h>
#include <src/viewers/OrthoViewer.h>




#include <src/controllers/watershedControl.h>
#include "src/controllers/MainWindowWatershedControl.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    QFile f("../qdarkstyle/style.qss");
    if (!f.exists()) {
        printf("Unable to set stylesheet, file not found\n");
    } else {
        f.open(QFile::ReadOnly | QFile::Text);
        QTextStream ts(&f);
        a.setStyleSheet(ts.readAll());
    }

    auto *myMainWindow = new MainWindowWatershedControl();
    myMainWindow->show();
    myMainWindow->myWatershedControl->addBoundariesFromFile("/Users/joachimgreiner/Documents/memcrop.nrrd");

    return a.exec();
}


