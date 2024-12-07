
#include <QApplication>
#include <QtWidgets>
#include <src/viewers/fileIO.h>
#include <QObject>
#include <src/viewers/OrthoViewer.h>
#include <itkExtractImageFilter.h>

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

    auto *myMainWindow = new QMainWindow();

    auto* orthoViewer = new OrthoViewer();
    GraphBase::pOrthoViewer = orthoViewer;
    myMainWindow->setCentralWidget(orthoViewer);
    QString fileName = QFileDialog::getOpenFileName(myMainWindow,
                                                    "Open Images");

    using ImageType =  itk::Image<short, 3>;
    ImageType::Pointer pImage = ITKImageLoader<short>(fileName);

    std::unique_ptr<itkSignal<short>> pSignal2(new itkSignal<short>(pImage));
    pSignal2->setLUTCategorical();

    orthoViewer->addSignal(pSignal2.get());

    myMainWindow->show();

    // steps:
    // draw rectangle to define new region
    // crop image
    // create new window where you can paint border regions in it
    // run watershed loop stuff
    // merge segments in new window
    // select the merged segment you want to transfer into the existing watershed

    // probably have to rewrite graphbase to be an instance of a class instead of static
    // then multiple graphs can easily coexist
    // dirty hack would be to create another static graphbase for watershed only

    int fx = 0, fy=0, fz=0, tx = 512, ty=512, tz =128;




    //TODO: check for largest possible region
    constexpr unsigned int Dimension = 3;
    using RegionType = itk::ImageRegion<Dimension>;
    RegionType::IndexType regionIndex{fx, fy, fz};
    RegionType::SizeType regionSize;
    regionSize.at(0) = tx-fx;
    regionSize.at(1) = ty-fy;
    regionSize.at(2) = tz-fz;
    RegionType croppedRegion(regionIndex, regionSize);





    using FilterType = itk::ExtractImageFilter<ImageType, ImageType>;
    FilterType::Pointer filter = FilterType::New();
    filter->SetExtractionRegion(croppedRegion);
    filter->SetInput(pImage);
    filter->SetDirectionCollapseToIdentity();
    filter->Update();

    itk::Image<short, 3>::Pointer pCroppedImage = filter->GetOutput();



    auto *myMainWindow2 = new QMainWindow();

    auto* orthoViewer2 = new OrthoViewer();
    myMainWindow2->setCentralWidget(orthoViewer2);

    std::unique_ptr<itkSignal<short>> pSignal3(new itkSignal<short>(pCroppedImage));
    pSignal3->setLUTCategorical();

    orthoViewer2->addSignal(pSignal3.get());

    myMainWindow2->show();


    return a.exec();
}
