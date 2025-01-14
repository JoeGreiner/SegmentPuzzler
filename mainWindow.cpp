#include "mainWindow.h"
#include <QApplication>
#include <QDesktopWidget>
#include <QStatusBar>
#include <QScreen>
#include <QDialogButtonBox>
#include "src/utils/utils.h"
#include "src/controllers/SignalControl.h"
#include "src/viewers/OrthoViewer.h"

MainWindow::MainWindow() {

//    FeatureList::nodeFeaturesList.push_back(std::make_unique<NumberOfVoxels>());
//    FeatureList::nodeFeaturesList.push_back(std::make_unique<PCAValues>());
//    FeatureList::nodeFeaturesList.push_back(std::make_unique<PCARatios>());
//    FeatureList::edgeFeaturesList.push_back(std::make_unique<NumberOfVoxels>());

    graphBase = std::make_shared<GraphBase>();

    graphBase->ignoredSegmentLabels = {};
    graphBase->pWorkingSegmentsImage = nullptr;
    graphBase->pEdgesInitialSegmentsImage = nullptr;
    graphBase->colorLookUpEdgesStatus = std::unordered_map<char, std::vector<unsigned char>>();
    graphBase->edgeStatus = std::unordered_map<dataType::MappedEdgeIdType, char>();

    graph = new Graph(graphBase);
    graphBase->pGraph = graph;

    myOrthowindow = new OrthoViewer(graphBase);
    graphBase->pOrthoViewer = myOrthowindow;

//    QString fileName = "/Volumes/Borneo/jg19/segmentation/files/160323_1/watershed4.shlat";
//    QString fileName = "/Volumes/Seagate/ManualCorrection/watershed.uilat";
//    QString fileName2 = "/Volumes/Borneo/jg19/segmentation/files/160323_1/watershed2.shlat";
//    QString fileName_dapi = "/Volumes/Borneo/jg19/segmentation/files/160323_1/160323_1_c1.shlat";
//    QString fileName_cx = "/Volumes/Borneo/jg19/segmentation/files/160323_1/160323_1_c2.shlat";
//    QString fileName_wga = "/Volumes/Borneo/jg19/segmentation/files/160323_1/160323_1_c3.shlat";
//    QString fileName = "/Users/jg19/CLionProjects/SegmentCoupler/testFiles/fine2.shlat";
//    QString fileName = "/Users/jg19/CLionProjects/SegmentCoupler/testFiles/fine.shlat";
//    QString fileName2 = "/Users/jg19/CLionProjects/SegmentCoupler/testFiles/fine2.shlat";
//    QString fileName = "/Volumes/Borneo/ET_SegmentCoupler/1ws_improved.uilat";
//    QString fileName1 = "/Users/joachimgreiner/CLionProjects/segmentcoupler/testFiles/2d_segments.tif";
//    QString fileName2 = "/Users/joachimgreiner/CLionProjects/segmentcoupler/testFiles/2d_image_Probabilities.tiff";
//    QString fileName2 = "/Users/joachimgreiner/CLionProjects/segmentcoupler/testFiles/2d_image_empty.tif";
//    QString fileName2 = "/Users/joachimgreiner/watershed0.nrrd";
//    QString fileName2 = "/Users/joachimgreiner/Downloads/newseg.nrrd";`
//    QString fileName3 = "/Users/joachimgreiner/Downloads/150416_1_c3.nrrd";
//    QString fileName2 = "/Users/joachimgreiner/Documents/wscrop.nrrd";
//    QString fileName3 = "/Users/joachimgreiner/Documents/memcrop.nrrd";
//    QString fileName = "/Volumes/Borneo/jg19/segmentation/files/160323_1/watershedSmall.shlat";
//    QString fileName = "/Users/jg19/Downloads/tomosegmemtv_osx/source/wsTest/watershedBorder.shlat";
//    QString fileName2 = "/Users/jg19/Downloads/tomosegmemtv_osx/source/wsTest/dataOffSet.uilat";
//    QString fileName2 = "/home/greinerj/CLionProjects/segmentcoupler/sampleData/Stack.nrrd";
//    QString fileName2 = "/mnt/work/tmp/seg_stack_2/prediction_combined/2024_11_08_WGA_Rabbit_stack_1_wga_MultiCut/2024_11_08_WGA_Rabbit_stack_1_wga_mc_0.075_pmap_zero.nrrd";
//    QString fileName2 = "/mnt/work/tmp/seg_stack_2/prediction_combined/2024_11_08_WGA_Rabbit_stack_1_wga_ws.nrrd";

    mySignalControl = new SignalControl(graphBase);
//    mySignalControl->addSegmentsGraph(fileName2);
//    mySignalControl->loadMembraneProbability(fileName2);
//    graphBase->ROI_fx = 0;d
//    graphBase->ROI_fy = 0;
//    graphBase->ROI_fz = 0;
//    graphBase->ROI_tx = 255;
//    graphBase->ROI_ty = 255;
//    graphBase->ROI_tz = 1;
//    mySignalControl->runWatershed();
//    mySignalControl->createNewSegmentationVolume();
//    mySignalControl->addSegmentsGraph(fileName2);
//    mySignalControl->addImage(fileName_dapi);
//    mySignalControl->addImage(fileName_wga);
//    mySignalControl->addImage(fileName_cx);
//    mySignalControl->addRefinementWatershed(fileName2);
//    mySignalControl->addImage(fileName2);

//    graph.refineSegmentByPosition(440, 0, 159);

    auto horizontalSplitter = new QSplitter();
    horizontalSplitter->addWidget(mySignalControl);
    horizontalSplitter->addWidget(myOrthowindow);
    horizontalSplitter->setStretchFactor(0, 1);
    horizontalSplitter->setStretchFactor(1, 3);

    setCentralWidget(horizontalSplitter);

    QRect rec =    QGuiApplication::primaryScreen()->geometry();
    unsigned int screenWidth = rec.width();
    unsigned int screenHeight = rec.height();
    printf("width: %d height: %d\n", screenWidth, screenHeight);
    this->resize(static_cast<int>(0.9*screenWidth), static_cast<int>(0.9*screenHeight)); //have to do this
    this->showMaximized();

    sampleDataMenu = menuBar()->addMenu(tr("&Sample Data"));
    loadSampleSegmentationAction = new QAction(tr("&Load Sample Data (100MB)"), this);
    sampleDataMenu->addAction(loadSampleSegmentationAction);
//    connect(loadSampleSegmentationAction, &QAction::triggered, this, &MainWindow::loadSegmentationSample);
    connect(loadSampleSegmentationAction, &QAction::triggered, this, [this]() {
        QMetaObject::invokeMethod(this, "loadSegmentationSample", Qt::QueuedConnection);
    });


    helpMenu = menuBar()->addMenu(tr("&Help"));
    openHotkeysAction = new QAction(tr("&Show HotKeys"), this);
    helpMenu->addAction(openHotkeysAction);
    connect(openHotkeysAction, &QAction::triggered, this, &MainWindow::showHotkeys);
    connect(myOrthowindow, &OrthoViewer::sendStatusMessage, this, &MainWindow::receiveStatusMessage);


}


#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>
#include <QStandardPaths>
#include <QDir>


#include <QSslSocket>
#include <QLoggingCategory>
#include <QProgressDialog>

QString downloadFile(QString url_to_download, QString outputFilePath) {
    QNetworkAccessManager manager;
    QUrl qurl(url_to_download);
    QNetworkRequest request(qurl);

    if (QFile::exists(outputFilePath)) {
        std::cout << "File already exists: " << outputFilePath.toStdString() << std::endl;
        return outputFilePath;
    }

    int redirectCount = 0;
    const int maxRedirects = 5;

    while (redirectCount < maxRedirects) {
        QNetworkReply *reply = manager.get(request);

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec(); // Wait for the request to complete

        if (reply->error() != QNetworkReply::NoError) {
            std::cout << "Download failed: " << reply->errorString().toStdString() << std::endl;
            reply->deleteLater();
            return "";
        }

        QVariant redirection = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirection.isValid()) {
            QUrl newUrl = redirection.toUrl();
            if (newUrl.isRelative()) {
                newUrl = request.url().resolved(newUrl);
            }
            std::cout << "Following redirect to: " << newUrl.toString().toStdString() << "\n";
            request.setUrl(newUrl);
            reply->deleteLater();
            redirectCount++;
            continue; // Follow the redirect
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        if (data.isEmpty()) {
            std::cout << "No data received after following redirects.\n";
            return "";
        }

        QFile file(outputFilePath);
        if (!file.open(QIODevice::WriteOnly)) {
            std::cout << "Failed to open file: " << outputFilePath.toStdString() << std::endl;
            return "";
        }

        file.write(data);
        file.close();

        std::cout << "File downloaded successfully to: " << outputFilePath.toStdString() << "\n";
        return outputFilePath;
    }

    std::cout << "Too many redirects.\n";
    return "";
};



std::tuple<QString, QString, QString, QString> downloadFiles() {

    QLoggingCategory::setFilterRules(QStringLiteral("qt.network.ssl.warning=true\n"
                                                    "qt.network.ssl.debug=true\n"));

    std::cout << "QSslSocket supports SSL: "
              << (QSslSocket::supportsSsl() ? "true" : "false") << std::endl;


    std::cout << "QSslSocket library build version: "
              << QSslSocket::sslLibraryBuildVersionString().toStdString()
              << std::endl;

    std::cout << "QSslSocket library runtime version: "
              << QSslSocket::sslLibraryVersionString().toStdString()
              << std::endl;


    QString url = "https://drive.google.com/uc?export=download&id=1FW592Qge47SjoVQupk83LSwkhs-70nh2";
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty()) {
        std::cout << "Failed to retrieve the temporary directory path.\n";
        return std::make_tuple("", "", "", "");
    }


    QDir dir(tempDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            std::cout << "Failed to create the temporary directory.\n";
            return std::make_tuple("", "", "", "");
        }
    }

    QString outputFilePathMC = QDir(tempDir).filePath("Watershed_MC.tif");
    QString outputFilePathWGA = QDir(tempDir).filePath("WGA.nrrd");
    QString outputFilePathWS = QDir(tempDir).filePath("Watershed.nrrd");
    QString outputFilePathBnd = QDir(tempDir).filePath("BoundaryPrediction.tif");

    QString url_segments_mc = "https://drive.google.com/uc?export=download&id=18VtLYTFA0EVa_JLOVoSmXPjDJrW0Ievr";
    QString url_segments_wga = "https://drive.google.com/uc?export=download&id=1pd6ybdzrQFdgpANKweNG7hB7i1diOgxC";
    QString url_segments_ws = "https://drive.google.com/uc?export=download&id=1x_QYEfPRNrTWlwHRUMI9jFKh1xyWw39Y";
    QString url_segments_bnd = "https://drive.google.com/uc?export=download&id=1YMFz84x8_E4OVh74ABn8j2pcpGXKtu8r";

    QString downloadedFilePathMC = downloadFile(url_segments_mc, outputFilePathMC);
    QString downloadedFilePathWGA = downloadFile(url_segments_wga, outputFilePathWGA);
    QString downloadedFilePathWS = downloadFile(url_segments_ws, outputFilePathWS);
    QString downloadedFilePathBnd = downloadFile(url_segments_bnd, outputFilePathBnd);

    return std::make_tuple(downloadedFilePathMC, downloadedFilePathWGA, downloadedFilePathWS, downloadedFilePathBnd);
}



void MainWindow::loadSegmentationSample() {
    if (graphBase->pWorkingSegmentsImage != nullptr) {
        QMessageBox msgBox;
        msgBox.setText("Segments were already added, please restart Segmentocupler for a new project.");
        msgBox.exec();
        return;
    }
//
//    QString appDirPath = QCoreApplication::applicationDirPath();
//    QString pathToSampleSegmentation = appDirPath + "/../../sampleData/Stack.nrrd";
//    QString alternativePathToSampleSegmentation = appDirPath + "/sampleData/Stack.nrrd";
//    QString alternativePathToSampleSegmentation_2 = appDirPath + "/../Resources/sampleData/Stack.nrrd";
////    pathToSampleSegmentation = downloadFiles();
//
//
//    std::cout << appDirPath.toStdString() << std::endl;
//    if (utils::check_if_file_exists(pathToSampleSegmentation)){
//        graph->receiveBackgroundIdStrategy("backgroundIsHighestId");
//        mySignalControl->addSegmentsGraph(pathToSampleSegmentation);
//    } else if (utils::check_if_file_exists(alternativePathToSampleSegmentation)){
//        graph->receiveBackgroundIdStrategy("backgroundIsHighestId");
//        mySignalControl->addSegmentsGraph(alternativePathToSampleSegmentation);
//    } else if (utils::check_if_file_exists(alternativePathToSampleSegmentation_2)){
//        graph->receiveBackgroundIdStrategy("backgroundIsHighestId");
//        mySignalControl->addSegmentsGraph(alternativePathToSampleSegmentation_2);
//    } else {
//        std::cout << "Couldn't find sample data at: " << pathToSampleSegmentation.toStdString() << std::endl;
//        std::cout << "Couldn't find sample data at: " << alternativePathToSampleSegmentation.toStdString() << std::endl;
//        std::cout << "Couldn't find sample data at: " << alternativePathToSampleSegmentation_2.toStdString() << std::endl;
//    }

//     message informing of download
//    QMessageBox msgBox;
//    msgBox.setText("Downloading sample data (100MB). This may take a while.");
//    msgBox.exec();

    QProgressDialog progressDialog("Downloading sample data, please wait...",
                                   QString(),
                                   0,
                                   0,
                                   this);
    progressDialog.setWindowModality(Qt::NonModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.setMinimumDuration(0);
    progressDialog.setWindowTitle("Please Wait");
    progressDialog.show();

    QCoreApplication::processEvents();

    QString downloadedFilePathMC, downloadedFilePathWGA, downloadedFilePathWS, downloadedFilePathBnd;
    std::tie(downloadedFilePathMC, downloadedFilePathWGA, downloadedFilePathWS, downloadedFilePathBnd) = downloadFiles();

    progressDialog.close();

    if (utils::check_if_file_exists(downloadedFilePathMC)){
        graph->receiveBackgroundIdStrategy("backgroundIsLowestId");
        mySignalControl->addSegmentsGraph(downloadedFilePathMC);
        mySignalControl->addRefinementWatershed(downloadedFilePathWS);
        mySignalControl->addImage(downloadedFilePathWGA);
        mySignalControl->loadMembraneProbability(downloadedFilePathBnd);
        QTreeWidgetItem *probability_item = mySignalControl->probabilityTreeWidget->topLevelItem(0);
        mySignalControl->setIsActive(probability_item, false);
        mySignalControl->allSignalList[4]->setNorm(0, 100);


    } else {
        std::cout << "Couldn't find sample data at: " << downloadedFilePathMC.toStdString() << std::endl;
    }
}

void MainWindow::showHotkeys() {
    QString hotKeyText = R"(
<html>
<head>
<style>
  body {
    font-family: Arial, sans-serif;
    font-size: 10pt;
  }
  h3 {
    margin-bottom: 0.3em;
  }
  p {
    margin-top: 0;
    margin-bottom: 1em;
  }
</style>
</head>
<body>

<h3>S + Click</h3>
<p>Transfer working [S]egment under cursor to currently active segmentation.</p>

<h3>D + Click</h3>
<p>[D]elete label under cursor in currently active segmentation.</p>

<h3>C + Click</h3>
<p>[C]ut initial label under cursor from working node under the cursor.</p>

<h3>E</h3>
<p>[E]xport debug information.</p>

<h3>X + Click</h3>
<p>Split working node into its initial nodes.</p>

<h3>Q + Click</h3>
<p>(For Paintmode) Set the brush to the color/id of the SEGMENTATION under the cursor.</p>

<h3>Left/Right Click in Paintmode</h3>
<p>Left: Add to color/id. Right: Remove from color/id.</p>

<h3>+/-</h3>
<p>Zoom to/away from cursor.</p>

<h3>Arrow Up/Down</h3>
<p>Increase/Decrease slice index (go up/down in stack).</p>

<h3>V</h3>
<p>Export Image Series for generation of [V]ideos.</p>

<h3>U</h3>
<p>Export Screenshot of current views.</p>

<h3>0-9</h3>
<p>Change brush size (1 smallest, 10 biggest).</p>

<h3>R</h3>
<p>[R]andom color scheme for working Segments.</p>

<h3>P + Click</h3>
<p>Refine by [P]osition of the cursor with currently active refinement watershed.</p>

<h3>CMD + Click</h3>
<p>Set other orthogonal views to slice through the point under the cursor.</p>

</body>
</html>
)";

    QDialog dialog(this);
    dialog.setWindowTitle("HotKeys");
    dialog.resize(600, 400);

    auto *layout = new QVBoxLayout(&dialog);

    auto *label = new QLabel(&dialog);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setText(hotKeyText);

    layout->addWidget(label);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}


void MainWindow::receiveStatusMessage(const QString& string) {
    statusBar()->showMessage(string);
}

