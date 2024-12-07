#include <QApplication>
#include <QtWidgets>
#include <src/viewers/fileIO.h>
#include <QObject>
#include <src/viewers/OrthoViewer.h>

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

    std::shared_ptr<GraphBase> graphBase = std::make_shared<GraphBase>();


    auto myMainWindow = std::make_unique<QMainWindow>();
    auto* orthoViewer = new OrthoViewer(graphBase);


    graphBase->pOrthoViewer = orthoViewer;

    myMainWindow->setCentralWidget(orthoViewer);
    QString fileName = QFileDialog::getOpenFileName(myMainWindow.get(),
                                                    "Open Images");

    itk::Image<unsigned short, 3>::Pointer pImage = ITKImageLoader<unsigned short>(fileName);

    std::unique_ptr<itkSignal<unsigned short>> pSignal2(new itkSignal<unsigned short>(pImage));
    pSignal2->setLUTCategorical();

    orthoViewer->addSignal(pSignal2.get());

    myMainWindow->show();

    return a.exec();
}
