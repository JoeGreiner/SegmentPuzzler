#include <src/viewers/fileIO.h>
#include <src/viewers/SliceViewer.h>

#include <QApplication>
#include <QObject>
#include <QIODevice>
#include <QFile>
#include <QTextStream>
#include <QMainWindow>
#include <QFileDialog>

int main(int argc, char *argv[]) {
    QApplication myApp(argc, argv);

    QFile f("../qdarkstyle/style.qss");
    if (!f.exists()) {
        printf("Unable to set stylesheet, file not found\n");
    } else {
        f.open(QFile::ReadOnly | QFile::Text);
        QTextStream ts(&f);
        myApp.setStyleSheet(ts.readAll());
    }

    auto myMainWindow = std::make_unique<QMainWindow>();
    auto graphBase = std::make_shared<GraphBase>();

//     sliceviewer will be owned by myMainWindow and will be deleted when myMainWindow is deleted
    auto* sliceViewer = new SliceViewer(graphBase);
    myMainWindow->setCentralWidget(sliceViewer);
    QString fileName = QFileDialog::getOpenFileName(myMainWindow.get(),
                                                    "Open Images");

    itk::Image<unsigned short, 3>::Pointer pImage;
    try {
        pImage = ITKImageLoader<unsigned short>(fileName);
    } catch (const itk::ExceptionObject &error) {
        std::cout << "Error loading image:" << error.what();
        return -1;
    }

    auto pSignal2 = std::make_unique<itkSignal<unsigned short>>(pImage);
    pSignal2->setLUTCategorical();

    auto pSignal3 = std::make_unique<SliceViewerITKSignal>(pSignal2.get());
    sliceViewer->addSignal(pSignal3.get());

    myMainWindow->show();

    return QApplication::exec();
}
