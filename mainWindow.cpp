#include "mainWindow.h"
#include <QApplication>
#include <QStatusBar>
#include <QScreen>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QMimeData>
#include <QLineEdit>
#include <QLabel>
#include <QInputDialog>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QWindow>
#include <algorithm>
#include "src/utils/utils.h"
#include "src/utils/systemStats.h"
#include "src/qtUtils/WindowStats.h"
#include "src/controllers/SignalControl.h"
#include "src/viewers/OrthoViewer.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SegmentTableDialog.h"
#include "src/qtUtils/LoggingSettingsDialog.h"
#include "src/utils/AppLogger.h"

MainWindow::~MainWindow() = default;

namespace {

bool debugLayerLayoutEnabled() {
    static const bool enabled = !qgetenv("SEGMENTPUZZLER_DEBUG_LAYER_LAYOUT").isEmpty();
    return enabled;
}

QStringList localFilesFromMimeData(const QMimeData *mimeData) {
    QStringList localFiles;
    if (mimeData == nullptr || !mimeData->hasUrls()) {
        return localFiles;
    }

    for (const QUrl &url : mimeData->urls()) {
        const QString fileName = url.toLocalFile();
        if (!fileName.isEmpty()) {
            localFiles.push_back(fileName);
        }
    }

    return localFiles;
}

void showWindowWithinAvailableScreen(QMainWindow *window) {
    if (window == nullptr) {
        return;
    }

    window->show();
    QTimer::singleShot(0, window, [window]() {
        QScreen *screen = nullptr;
        if (window->windowHandle() != nullptr) {
            screen = window->windowHandle()->screen();
        }
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }

        if (screen == nullptr) {
            return;
        }

        const QRect availableGeometry = screen->availableGeometry();
        const QRect frameGeometry = window->frameGeometry();
        const QRect contentGeometry = window->geometry();

        const int leftFrameMargin = contentGeometry.left() - frameGeometry.left();
        const int topFrameMargin = contentGeometry.top() - frameGeometry.top();
        const int rightFrameMargin = frameGeometry.right() - contentGeometry.right();
        const int bottomFrameMargin = frameGeometry.bottom() - contentGeometry.bottom();

        QRect targetGeometry = availableGeometry.adjusted(leftFrameMargin,
                                                          topFrameMargin,
                                                          -rightFrameMargin,
                                                          -bottomFrameMargin);
        if (targetGeometry.width() < 1 || targetGeometry.height() < 1) {
            targetGeometry = availableGeometry;
        }

        window->setGeometry(targetGeometry);
    });
}

void scheduleInitialSidebarSizes(QSplitter *splitter, SignalControl *sidebar) {
    if (splitter == nullptr || sidebar == nullptr) {
        return;
    }

    QPointer<QSplitter> guardedSplitter = splitter;
    QPointer<SignalControl> guardedSidebar = sidebar;
    QTimer::singleShot(0, splitter, [guardedSplitter, guardedSidebar]() {
        if (guardedSplitter == nullptr || guardedSidebar == nullptr) {
            return;
        }

        const int availableWidth = std::max(0, guardedSplitter->width() - guardedSplitter->handleWidth());
        if (availableWidth <= 0) {
            return;
        }

        int sidebarMargin = guardedSplitter->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, guardedSplitter);
        if (sidebarMargin < 0) {
            sidebarMargin = 8;
        }

        const int requestedLeftWidth = guardedSidebar->preferredSidebarWidthHint() + sidebarMargin;
        const int maxLeftWidth = std::max(1, static_cast<int>(availableWidth * 0.45));
        const int leftWidth = std::min(requestedLeftWidth, maxLeftWidth);
        guardedSplitter->setSizes({leftWidth, std::max(0, availableWidth - leftWidth)});

        if (debugLayerLayoutEnabled()) {
            const QList<int> sizes = guardedSplitter->sizes();
            std::cout << QStringLiteral(
                             "[LayerSidebarSizing] splitterWidth=%1 handleWidth=%2 available=%3 requestedLeft=%4 "
                             "chosenLeft=%5 actualSizes=%6,%7 sidebarWidth=%8 sidebarPreferred=%9")
                             .arg(guardedSplitter->width())
                             .arg(guardedSplitter->handleWidth())
                             .arg(availableWidth)
                             .arg(requestedLeftWidth)
                             .arg(leftWidth)
                             .arg(sizes.value(0, -1))
                             .arg(sizes.value(1, -1))
                             .arg(guardedSidebar->width())
                             .arg(guardedSidebar->preferredSidebarWidthHint())
                             .toStdString()
                      << std::endl;
        }
    });
}

} // namespace

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

    graph = std::make_unique<Graph>(graphBase);
    graphBase->pGraph = graph.get();

    taskRunner = std::make_unique<TaskRunner>(this, this);
    myOrthowindow = new OrthoViewer(graphBase, taskRunner.get());

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

    mySignalControl = new SignalControl(graphBase, myOrthowindow, taskRunner.get());
//    mySignalControl->addSegmentsGraph(fileName2);
//    mySignalControl->loadMembraneProbability(fileName2);
//    graphBase->ROI_fx = 0;d
//    graphBase->ROI_fy = 0;
//    graphBase->ROI_fz = 0;
//    graphBase->ROI_tx = 255;
//    graphBase->ROI_ty = 255;
//    graphBase->ROI_tz = 1;
//    mySignalControl->runWatershed();
//    mySignalControl->addSegmentsGraph(fileName2);
//    mySignalControl->addImage(fileName_dapi);
//    mySignalControl->addImage(fileName_wga);
//    mySignalControl->addImage(fileName_cx);
//    mySignalControl->addImage(fileName2);

//    graph.refineWithSelectedRefinementAtPosition(440, 0, 159);

    auto horizontalSplitter = new QSplitter();
    horizontalSplitter->addWidget(mySignalControl);
    horizontalSplitter->addWidget(myOrthowindow);
    horizontalSplitter->setChildrenCollapsible(false);
    horizontalSplitter->setStretchFactor(0, 1);
    horizontalSplitter->setStretchFactor(1, 3);
    connect(mySignalControl, &SignalControl::preferredSidebarWidthChanged, this, [this, horizontalSplitter]() {
        scheduleInitialSidebarSizes(horizontalSplitter, mySignalControl);
    });

    setCentralWidget(horizontalSplitter);

    addDataMenu = menuBar()->addMenu(tr("&Add Data"));
    loadSampleSegmentationAction = new QAction(tr("&Download Sample Dataset (100 MB)"), this);
    mySignalControl->populateAddDataMenu(addDataMenu, loadSampleSegmentationAction);
    connect(loadSampleSegmentationAction, &QAction::triggered, this, [this]() {
        QMetaObject::invokeMethod(this, "loadSegmentationSample", Qt::QueuedConnection);
    });

    boundariesMenu = menuBar()->addMenu(tr("&Boundaries"));
    mySignalControl->populateBoundariesMenu(boundariesMenu);

    refinementsMenu = menuBar()->addMenu(tr("&Refinements"));
    mySignalControl->populateRefinementsMenu(refinementsMenu);

    segmentationsMenu = menuBar()->addMenu(tr("&Segmentations"));
    mySignalControl->populateSegmentationsMenu(segmentationsMenu);
    segmentationsMenu->addSeparator();
    showSegmentTableAction = new QAction(tr("&Segment Feature Table"), this);
    showSegmentTableAction->setShortcut(Qt::Key_F8);
    segmentationsMenu->addAction(showSegmentTableAction);
    connect(showSegmentTableAction, &QAction::triggered, this, [this]() {
        if (myOrthowindow != nullptr) {
            myOrthowindow->flashShortcutLegendKey("f8");
        }
    });
    connect(showSegmentTableAction, &QAction::triggered, this, &MainWindow::showSegmentTable);
    splitWorkingSegment3DCutAction = new QAction(tr("Open Segment in 3D Cut View..."), this);
    segmentationsMenu->addAction(splitWorkingSegment3DCutAction);
    connect(splitWorkingSegment3DCutAction, &QAction::triggered, this, &MainWindow::arm3DWorkingSegmentCut);

    settingsMenu = menuBar()->addMenu(tr("&Settings"));
    QAction *setClosingRadiusAction = new QAction(tr("Set Closing Radius"), this);
    settingsMenu->addAction(setClosingRadiusAction);
    connect(setClosingRadiusAction, &QAction::triggered, this, [this]() {
        const int currentRadius = myOrthowindow != nullptr && myOrthowindow->xy != nullptr
                                      ? myOrthowindow->xy->getClosingRadius()
                                      : 8;
        const int radius = QInputDialog::getInt(
            this,
            tr("Closing Radius"),
            tr("Radius for the fill/close action (F):"),
            currentRadius,
            0,
            100);
        if (myOrthowindow != nullptr) {
            myOrthowindow->setMorphologyClosingRadius(radius);
        }
    });

    QAction *setOpeningRadiusAction = new QAction(tr("Set Opening Radius"), this);
    settingsMenu->addAction(setOpeningRadiusAction);
    connect(setOpeningRadiusAction, &QAction::triggered, this, [this]() {
        const int currentRadius = myOrthowindow != nullptr && myOrthowindow->xy != nullptr
                                      ? myOrthowindow->xy->getOpeningRadius()
                                      : 3;
        const int radius = QInputDialog::getInt(
            this,
            tr("Opening Radius"),
            tr("Radius for the open action (G):"),
            currentRadius,
            0,
            100);
        if (myOrthowindow != nullptr) {
            myOrthowindow->setMorphologyOpeningRadius(radius);
        }
    });

    QAction *setDilationRadiusAction = new QAction(tr("Set Dilation Radius"), this);
    settingsMenu->addAction(setDilationRadiusAction);
    connect(setDilationRadiusAction, &QAction::triggered, this, [this]() {
        const int currentRadius = myOrthowindow != nullptr && myOrthowindow->xy != nullptr
                                      ? myOrthowindow->xy->getDilationRadius()
                                      : 1;
        const int radius = QInputDialog::getInt(
            this,
            tr("Dilation Radius"),
            tr("Radius for the dilate action (J):"),
            currentRadius,
            0,
            100);
        if (myOrthowindow != nullptr) {
            myOrthowindow->setMorphologyDilationRadius(radius);
        }
    });

    QAction *setErosionRadiusAction = new QAction(tr("Set Erosion Radius"), this);
    settingsMenu->addAction(setErosionRadiusAction);
    connect(setErosionRadiusAction, &QAction::triggered, this, [this]() {
        const int currentRadius = myOrthowindow != nullptr && myOrthowindow->xy != nullptr
                                      ? myOrthowindow->xy->getErosionRadius()
                                      : 1;
        const int radius = QInputDialog::getInt(
            this,
            tr("Erosion Radius"),
            tr("Radius for the erode action (K):"),
            currentRadius,
            0,
            100);
        if (myOrthowindow != nullptr) {
            myOrthowindow->setMorphologyErosionRadius(radius);
        }
    });

    settingsMenu->addSeparator();
    QAction *loggingSettingsAction = new QAction(tr("Logging..."), this);
    settingsMenu->addAction(loggingSettingsAction);
    connect(loggingSettingsAction, &QAction::triggered, this, &MainWindow::showLoggingSettings);
    settingsMenu->addSeparator();
    QAction *useSelectedSegmentationFor3DViewsAction =
        new QAction(tr("Use Selected Segmentation For 3D Views/Cuts"), this);
    useSelectedSegmentationFor3DViewsAction->setCheckable(true);
    useSelectedSegmentationFor3DViewsAction->setChecked(graphBase->useSelectedSegmentationFor3DView);
    settingsMenu->addAction(useSelectedSegmentationFor3DViewsAction);
    connect(useSelectedSegmentationFor3DViewsAction, &QAction::toggled, this, [this](bool checked) {
        graphBase->useSelectedSegmentationFor3DView = checked;
        update3DWorkingSegmentCutActionState();
    });


    goToMenu = menuBar()->addMenu(tr("&Go To"));
    QAction *openGoToCoordinatesAction = new QAction(tr("&Go to Coordinates"), this);
    openGoToCoordinatesAction->setShortcut(Qt::Key_F9);
    goToMenu->addAction(openGoToCoordinatesAction);
    connect(openGoToCoordinatesAction, &QAction::triggered, this, [this]() {
        if (myOrthowindow != nullptr) {
            myOrthowindow->flashShortcutLegendKey("f9");
        }
    });
    connect(openGoToCoordinatesAction, &QAction::triggered, this, [this]() {
//        open q box for three integers
        QDialog dialog(this);
        dialog.setWindowTitle("Go to Coordinates");
        QFormLayout form(&dialog);
        form.addRow(new QLabel("X:"));
        QLineEdit xEdit;
        form.addRow(&xEdit);
        form.addRow(new QLabel("Y:"));
        QLineEdit yEdit;
        form.addRow(&yEdit);
        form.addRow(new QLabel("Z:"));
        QLineEdit zEdit;
        form.addRow(&zEdit);

        QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
        form.addRow(&buttonBox);
        QObject::connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(&dialog, &QDialog::accepted, &dialog, &QDialog::deleteLater);

        if (dialog.exec() == QDialog::Accepted) {
            int x = xEdit.text().toInt();
            int y = yEdit.text().toInt();
            int z = zEdit.text().toInt();
            SP_LOG_INFO("viewer.interaction", QStringLiteral("Go to coordinates x=%1 y=%2 z=%3").arg(x).arg(y).arg(z));
            if(myOrthowindow->xy->isSliceIndexValid(z)) {
                myOrthowindow->xy->setSliceIndex(z);
            }
            if (myOrthowindow->xz->isSliceIndexValid(y)) {
                myOrthowindow->xz->setSliceIndex(y);
            }
            if (myOrthowindow->zy->isSliceIndexValid(x)) {
                myOrthowindow->zy->setSliceIndex(x);
            }
        }
    });

    QAction *openGoToLabelAction = new QAction(tr("&Go to Label ID"), this);
    openGoToLabelAction->setShortcut(Qt::Key_F10);
    goToMenu->addAction(openGoToLabelAction);
    connect(openGoToLabelAction, &QAction::triggered, this, [this]() {
        if (myOrthowindow != nullptr) {
            myOrthowindow->flashShortcutLegendKey("f10");
        }
    });
    connect(openGoToLabelAction, &QAction::triggered, this, [this]() {
        if (graphBase->pSelectedSegmentation == nullptr) {
            SP_LOG_WARNING("segmentation", QStringLiteral("Go to label requested without a selected segmentation"));
            return;
        }
        QDialog dialog(this);
        dialog.setWindowTitle("Go to Label ID");
        QFormLayout form(&dialog);
        form.addRow(new QLabel("Label ID:"));
        QLineEdit labelEdit;
        form.addRow(&labelEdit);

        QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
        form.addRow(&buttonBox);
        QObject::connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(&dialog, &QDialog::accepted, &dialog, &QDialog::deleteLater);

        if (dialog.exec() == QDialog::Accepted) {
            int label = labelEdit.text().toInt();
            SP_LOG_INFO("viewer.interaction", QStringLiteral("Go to label id=%1").arg(label));
            itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(graphBase->pSelectedSegmentation, graphBase->pSelectedSegmentation->GetLargestPossibleRegion());
            std::vector<itk::Index<3>> indices;
            while (!it.IsAtEnd()) {
                if (it.Get() == label) {
                    auto index = it.GetIndex();
                    indices.push_back(index);
                }
                ++it;
            }

            if (indices.empty()) {
                SP_LOG_WARNING("segmentation", QStringLiteral("Requested label id=%1 was not found in the selected segmentation").arg(label));
                return;
            }

//           calculate center of gravity
            itk::Index<3> index = {0, 0, 0};
            for (auto idx : indices) {
                index[0] += idx[0];
                index[1] += idx[1];
                index[2] += idx[2];
            }
            index[0] /= indices.size();
            index[1] /= indices.size();
            index[2] /= indices.size();
            SP_LOG_DEBUG("segmentation", QStringLiteral("Label id=%1 centroidApprox=%2,%3,%4").arg(label).arg(index[0]).arg(index[1]).arg(index[2]));

//           go to index that is closest
            auto closest_index = indices[0];
            double min_dist = std::numeric_limits<double>::max();
            for (auto idx : indices) {
                double dist = std::sqrt(std::pow(idx[0] - index[0], 2) + std::pow(idx[1] - index[1], 2) + std::pow(idx[2] - index[2], 2));
                if (dist < min_dist) {
                    min_dist = dist;
                    closest_index = idx;
                }
            }

            index = closest_index;

            SP_LOG_INFO("viewer.interaction", QStringLiteral("Navigating to label id=%1 at %2,%3,%4").arg(label).arg(index[0]).arg(index[1]).arg(index[2]));
            if(myOrthowindow->xy->isSliceIndexValid(index[2])) {
                myOrthowindow->xy->setSliceIndex(index[2]);
            }
            if (myOrthowindow->xz->isSliceIndexValid(index[1])) {
                myOrthowindow->xz->setSliceIndex(index[1]);
            }
            if (myOrthowindow->zy->isSliceIndexValid(index[0])) {
                myOrthowindow->zy->setSliceIndex(index[0]);
            }
            myOrthowindow->centerViewportsToXYZImageSpace(index[0], index[1], index[2]);
        }

    }
    );
    helpMenu = menuBar()->addMenu(tr("&Help"));
    openHotkeysAction = new QAction(tr("&Show Hotkeys"), this);
    openHotkeysAction->setShortcut(Qt::Key_F1);
    helpMenu->addAction(openHotkeysAction);
    connect(openHotkeysAction, &QAction::triggered, this, [this]() {
        if (myOrthowindow != nullptr) {
            myOrthowindow->flashShortcutLegendKey("f1");
        }
    });
    connect(openHotkeysAction, &QAction::triggered, this, &MainWindow::showHotkeys);
    connect(myOrthowindow, &OrthoViewer::sendStatusMessage, this, &MainWindow::receiveStatusMessage);
    connect(taskRunner.get(), &TaskRunner::busyChanged, loadSampleSegmentationAction, &QAction::setDisabled);
    connect(taskRunner.get(), &TaskRunner::busyChanged, this, [this]() { update3DWorkingSegmentCutActionState(); });
    connect(segmentationsMenu, &QMenu::aboutToShow, this, &MainWindow::update3DWorkingSegmentCutActionState);
    installInitialFileDropHandling();
    update3DWorkingSegmentCutActionState();

    // Title-bar system stats (CPU + RAM + swap), updated every 500 ms.
    windowStats::setupWindowTitleStatsTimer(this, "SegmentPuzzler");

    showWindowWithinAvailableScreen(this);
    scheduleInitialSidebarSizes(horizontalSplitter, mySignalControl);
}

void MainWindow::installInitialFileDropHandling() {
    // The main app window owns generic file drag-and-drop. We install it once
    // for the initial widget tree here and intentionally leave the watershed
    // workflow window out of this mechanism.
    registerDropTarget(this);
    const auto widgets = findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        registerDropTarget(widget);
    }
}

void MainWindow::registerDropTarget(QWidget *widget) {
    if (widget == nullptr) {
        return;
    }

    widget->setAcceptDrops(true);
    widget->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto *dropLikeEvent = static_cast<QDropEvent *>(event);
        if (!localFilesFromMimeData(dropLikeEvent->mimeData()).isEmpty()) {
            dropLikeEvent->acceptProposedAction();
            return true;
        }
    }

    if (event->type() == QEvent::Drop) {
        auto *dropEvent = static_cast<QDropEvent *>(event);
        const QStringList fileNames = localFilesFromMimeData(dropEvent->mimeData());
        if (fileNames.isEmpty()) {
            return QMainWindow::eventFilter(watched, event);
        }

        // Local-file drops are intentionally consumed at the main-window level.
        // Child widgets inside this window should use this shared path instead
        // of implementing their own file-drop behavior.
        for (const QString &fileName : fileNames) {
            QTimer::singleShot(0, mySignalControl, [this, fileName]() {
                mySignalControl->handleDroppedFile(fileName);
            });
        }
        dropEvent->acceptProposedAction();
        return true;
    }

    return QMainWindow::eventFilter(watched, event);
}


#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QStandardPaths>
#include <QDir>
#include <QElapsedTimer>


#include <QSslSocket>
#include <QLoggingCategory>
#include <QProgressDialog>

#include <functional>

namespace {

constexpr int sampleDownloadCount = 4;
constexpr int sampleDownloadProgressResolution = 1000;
constexpr qint64 sampleDownloadCachedAnimationDurationMs = 150;

using DownloadProgressCallback = std::function<void(const QString&, int, int, qint64, qint64)>;

enum class SampleDownloadResultType {
    Failed,
    Downloaded,
    Cached
};

struct SampleDownloadResult {
    QString filePath;
    SampleDownloadResultType type = SampleDownloadResultType::Failed;
};

void setSampleDownloadDialogState(QProgressDialog *progressDialog,
                                  const QString &actionLabel,
                                  const QString &fileLabel,
                                  int fileIndex,
                                  int totalFiles,
                                  int value) {
    if (progressDialog == nullptr) {
        return;
    }

    progressDialog->setLabelText(QString("%1 (%2/%3): %4")
                                         .arg(actionLabel)
                                         .arg(fileIndex + 1)
                                         .arg(totalFiles)
                                         .arg(fileLabel));
    progressDialog->setRange(0, totalFiles * sampleDownloadProgressResolution);
    progressDialog->setValue(value);
    progressDialog->repaint();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void animateSampleDownloadProgress(QProgressDialog *progressDialog,
                                   const QString &actionLabel,
                                   const QString &fileLabel,
                                   int fileIndex,
                                   int totalFiles,
                                   int startValue,
                                   int endValue,
                                   qint64 durationMs) {
    if (progressDialog == nullptr) {
        return;
    }

    if (endValue <= startValue || durationMs <= 0) {
        setSampleDownloadDialogState(progressDialog, actionLabel, fileLabel, fileIndex, totalFiles, endValue);
        return;
    }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < durationMs) {
        const double progress = static_cast<double>(timer.elapsed()) / static_cast<double>(durationMs);
        const int value = startValue + static_cast<int>((endValue - startValue) * progress);
        setSampleDownloadDialogState(progressDialog, actionLabel, fileLabel, fileIndex, totalFiles, value);
    }

    setSampleDownloadDialogState(progressDialog, actionLabel, fileLabel, fileIndex, totalFiles, endValue);
}

void updateSampleDownloadProgress(QProgressDialog *progressDialog,
                                  const QString &fileLabel,
                                  int fileIndex,
                                  int totalFiles,
                                  qint64 bytesReceived,
                                  qint64 bytesTotal) {
    if (progressDialog == nullptr) {
        return;
    }

    int value = fileIndex * sampleDownloadProgressResolution;
    if (bytesTotal > 0) {
        double progress = static_cast<double>(bytesReceived) / static_cast<double>(bytesTotal);
        if (progress < 0.0) {
            progress = 0.0;
        }
        if (progress > 1.0) {
            progress = 1.0;
        }
        value += static_cast<int>(progress * sampleDownloadProgressResolution);
    }

    setSampleDownloadDialogState(progressDialog, "Downloading sample dataset", fileLabel, fileIndex, totalFiles, value);
}

void markSampleDownloadComplete(QProgressDialog *progressDialog,
                                const QString &fileLabel,
                                int fileIndex,
                                int totalFiles,
                                bool isCached) {
    if (progressDialog == nullptr) {
        return;
    }

    const int startValue = progressDialog->value();
    const int endValue = (fileIndex + 1) * sampleDownloadProgressResolution;

    if (isCached) {
        animateSampleDownloadProgress(progressDialog,
                                      "Using cached sample dataset",
                                      fileLabel,
                                      fileIndex,
                                      totalFiles,
                                      startValue,
                                      endValue,
                                      sampleDownloadCachedAnimationDurationMs);
        return;
    }

    setSampleDownloadDialogState(progressDialog,
                                 "Downloading sample dataset",
                                 fileLabel,
                                 fileIndex,
                                 totalFiles,
                                 endValue);
}

} // namespace

SampleDownloadResult downloadFile(const QString &url_to_download,
                                  const QString &outputFilePath,
                                  const QString &fileLabel,
                                  int fileIndex,
                                  int totalFiles,
                                  const DownloadProgressCallback &progressCallback) {
    QNetworkAccessManager manager;
    QUrl qurl(url_to_download);
    QNetworkRequest request(qurl);

    if (QFile::exists(outputFilePath)) {
        SP_LOG_INFO("network", QStringLiteral("Sample file already cached at %1").arg(outputFilePath));
        return {outputFilePath, SampleDownloadResultType::Cached};
    }

    int redirectCount = 0;
    const int maxRedirects = 5;

    if (progressCallback) {
        progressCallback(fileLabel, fileIndex, totalFiles, 0, 0);
    }

    while (redirectCount < maxRedirects) {
        QNetworkReply *reply = manager.get(request);

        QObject::connect(reply,
                         &QNetworkReply::downloadProgress,
                         [progressCallback, fileLabel, fileIndex, totalFiles](qint64 bytesReceived, qint64 bytesTotal) {
                             if (progressCallback) {
                                 progressCallback(fileLabel, fileIndex, totalFiles, bytesReceived, bytesTotal);
                             }
                         });

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec(); // Wait for the request to complete

        if (reply->error() != QNetworkReply::NoError) {
            SP_LOG_ERROR("network", QStringLiteral("Sample download failed: %1").arg(reply->errorString()));
            reply->deleteLater();
            return {};
        }

        QVariant redirection = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirection.isValid()) {
            QUrl newUrl = redirection.toUrl();
            if (newUrl.isRelative()) {
                newUrl = request.url().resolved(newUrl);
            }
            SP_LOG_INFO("network", QStringLiteral("Following sample download redirect to %1").arg(newUrl.toString()));
            request.setUrl(newUrl);
            reply->deleteLater();
            redirectCount++;
            if (progressCallback) {
                progressCallback(fileLabel, fileIndex, totalFiles, 0, 0);
            }
            continue; // Follow the redirect
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        if (data.isEmpty()) {
            SP_LOG_WARNING("network", QStringLiteral("Sample download produced no data after redirects"));
            return {};
        }

        QFile file(outputFilePath);
        if (!file.open(QIODevice::WriteOnly)) {
            SP_LOG_ERROR("io", QStringLiteral("Failed to open sample download target %1").arg(outputFilePath));
            return {};
        }

        file.write(data);
        file.close();

        SP_LOG_INFO("network", QStringLiteral("Downloaded sample file to %1").arg(outputFilePath));
        if (progressCallback) {
            progressCallback(fileLabel, fileIndex, totalFiles, 1, 1);
        }
        return {outputFilePath, SampleDownloadResultType::Downloaded};
    }

    SP_LOG_ERROR("network", QStringLiteral("Sample download aborted because it exceeded the redirect limit"));
    return {};
};



std::tuple<QString, QString, QString, QString> downloadFiles(QProgressDialog *progressDialog) {

    QLoggingCategory::setFilterRules(QStringLiteral("qt.network.ssl.warning=true\n"
                                                    "qt.network.ssl.debug=true\n"));

    SP_LOG_INFO("network", QStringLiteral("QSslSocket supportsSsl=%1 buildVersion=%2 runtimeVersion=%3")
        .arg(QSslSocket::supportsSsl())
        .arg(QSslSocket::sslLibraryBuildVersionString())
        .arg(QSslSocket::sslLibraryVersionString()));

    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty()) {
        SP_LOG_ERROR("io", QStringLiteral("Failed to retrieve the temporary directory path for the sample download"));
        return std::make_tuple("", "", "", "");
    }


    QDir dir(tempDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            SP_LOG_ERROR("io", QStringLiteral("Failed to create the temporary directory %1").arg(tempDir));
            return std::make_tuple("", "", "", "");
        }
    }

    QString outputFilePathMC = QDir(tempDir).filePath("Watershed_MC.tif");
    QString outputFilePathWGA = QDir(tempDir).filePath("WGA.nrrd");
    QString outputFilePathRefinement = QDir(tempDir).filePath("Watershed.nrrd");
    QString outputFilePathBnd = QDir(tempDir).filePath("BoundaryPrediction.tif");

    QString url_segments_mc = "https://drive.google.com/uc?export=download&id=18VtLYTFA0EVa_JLOVoSmXPjDJrW0Ievr";
    QString url_segments_wga = "https://drive.google.com/uc?export=download&id=1pd6ybdzrQFdgpANKweNG7hB7i1diOgxC";
    QString url_segments_ws = "https://drive.google.com/uc?export=download&id=1x_QYEfPRNrTWlwHRUMI9jFKh1xyWw39Y";
    QString url_segments_bnd = "https://drive.google.com/uc?export=download&id=1YMFz84x8_E4OVh74ABn8j2pcpGXKtu8r";

    if (progressDialog != nullptr) {
        progressDialog->setRange(0, sampleDownloadCount * sampleDownloadProgressResolution);
        progressDialog->setValue(0);
    }

    const DownloadProgressCallback progressCallback = [progressDialog](const QString &fileLabel,
                                                                       int fileIndex,
                                                                       int totalFiles,
                                                                       qint64 bytesReceived,
                                                                       qint64 bytesTotal) {
        updateSampleDownloadProgress(progressDialog, fileLabel, fileIndex, totalFiles, bytesReceived, bytesTotal);
    };

    SampleDownloadResult downloadedMC = downloadFile(url_segments_mc,
                                                     outputFilePathMC,
                                                     "Watershed_MC.tif",
                                                     0,
                                                     sampleDownloadCount,
                                                     progressCallback);
    if (!downloadedMC.filePath.isEmpty()) {
        markSampleDownloadComplete(progressDialog,
                                   "Watershed_MC.tif",
                                   0,
                                   sampleDownloadCount,
                                   downloadedMC.type == SampleDownloadResultType::Cached);
    }

    SampleDownloadResult downloadedWGA = downloadFile(url_segments_wga,
                                                      outputFilePathWGA,
                                                      "WGA.nrrd",
                                                      1,
                                                      sampleDownloadCount,
                                                      progressCallback);
    if (!downloadedWGA.filePath.isEmpty()) {
        markSampleDownloadComplete(progressDialog,
                                   "WGA.nrrd",
                                   1,
                                   sampleDownloadCount,
                                   downloadedWGA.type == SampleDownloadResultType::Cached);
    }

    SampleDownloadResult downloadedRefinement = downloadFile(url_segments_ws,
                                                     outputFilePathRefinement,
                                                     "Watershed.nrrd",
                                                     2,
                                                     sampleDownloadCount,
                                                     progressCallback);
    if (!downloadedRefinement.filePath.isEmpty()) {
        markSampleDownloadComplete(progressDialog,
                                   "Watershed.nrrd",
                                   2,
                                   sampleDownloadCount,
                                   downloadedRefinement.type == SampleDownloadResultType::Cached);
    }

    SampleDownloadResult downloadedBnd = downloadFile(url_segments_bnd,
                                                      outputFilePathBnd,
                                                      "BoundaryPrediction.tif",
                                                      3,
                                                      sampleDownloadCount,
                                                      progressCallback);
    if (!downloadedBnd.filePath.isEmpty()) {
        markSampleDownloadComplete(progressDialog,
                                   "BoundaryPrediction.tif",
                                   3,
                                   sampleDownloadCount,
                                   downloadedBnd.type == SampleDownloadResultType::Cached);
    }

    return std::make_tuple(downloadedMC.filePath,
                           downloadedWGA.filePath,
                           downloadedRefinement.filePath,
                           downloadedBnd.filePath);
}



void MainWindow::loadSegmentationSample() {
    if (mySignalControl->hasWorkingSegments()) {
        QMessageBox msgBox;
        msgBox.setText("Supervoxels are already loaded, please restart SegmentPuzzler for a new project.");
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

    QProgressDialog progressDialog("Downloading sample dataset, please wait...",
                                   QString(),
                                   0,
                                   0,
                                   this);
    progressDialog.setWindowModality(Qt::NonModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.setMinimumDuration(0);
    progressDialog.setWindowTitle("Downloading Sample Dataset");
    progressDialog.setAutoClose(false);
    progressDialog.setAutoReset(false);
    progressDialog.setRange(0, sampleDownloadCount * sampleDownloadProgressResolution);
    progressDialog.setValue(0);
    progressDialog.show();

    QCoreApplication::processEvents();

    QString downloadedFilePathMC, downloadedFilePathWGA, downloadedFilePathRefinement, downloadedFilePathBnd;
    std::tie(downloadedFilePathMC, downloadedFilePathWGA, downloadedFilePathRefinement, downloadedFilePathBnd) = downloadFiles(&progressDialog);

    progressDialog.close();

    if (utils::check_if_file_exists(downloadedFilePathMC)){
        graph->setBackgroundIdStrategy("backgroundIsLowestId");
        mySignalControl->addSegmentsGraphAsync(
            downloadedFilePathMC,
            [this, downloadedFilePathRefinement, downloadedFilePathWGA, downloadedFilePathBnd](
                SignalControl::LoadResult segmentsIndex) {
                if (!segmentsIndex) {
                    return;
                }
                mySignalControl->loadRefinementAsync(
                    downloadedFilePathRefinement,
                    "",
                    [this, downloadedFilePathWGA, downloadedFilePathBnd](SignalControl::LoadResult refinementIndex) {
                        if (!refinementIndex) {
                            return;
                        }
                        mySignalControl->addImageAsync(
                            downloadedFilePathWGA,
                            "",
                            [this, downloadedFilePathBnd](SignalControl::LoadResult imageIndex) {
                                if (!imageIndex) {
                                    return;
                                }
                                mySignalControl->loadMembraneProbabilityAsync(
                                    downloadedFilePathBnd,
                                    "",
                                    SignalControl::BoundaryLoadMode::BoundaryOnly,
                                    SignalControl::FloatBoundaryConversionMode::CastValues,
                                    [this, imageIndex](SignalControl::LoadResult boundaryIndex) {
                                        if (!boundaryIndex) {
                                            return;
                                        }
                                        QTreeWidgetItem *probabilityItem =
                                            mySignalControl->probabilityTreeWidget->topLevelItem(0);
                                        if (probabilityItem != nullptr) {
                                            mySignalControl->setIsActive(probabilityItem, false);
                                        }
                                        if (*imageIndex < mySignalControl->allSignalList.size()) {
                                            mySignalControl->setSignalNormAndRefresh(*imageIndex, 0, 100);
                                        }
                                    });
                            });
                    });
            });


    } else {
        SP_LOG_ERROR("io", QStringLiteral("Downloaded sample segments file is missing at %1").arg(downloadedFilePathMC));
    }
}

void MainWindow::showSegmentTable() {
    if (!segmentTableDialog) {
        segmentTableDialog = new SegmentTableDialog(graphBase, myOrthowindow, this);
        segmentTableDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    segmentTableDialog->show();
    segmentTableDialog->raise();
    segmentTableDialog->activateWindow();
}

void MainWindow::showLoggingSettings() {
    SP_LOG_INFO("app", QStringLiteral("Opening logging settings"));
    LoggingSettingsDialog dialog(this);
    dialog.exec();
}

void MainWindow::showHotkeys() {
    QString hotKeyText = R"(
<html>
<head>
<style>
  body {
    font-family: Arial, sans-serif;
  }
  .bold_header {
    font-weight: bold;
    font-size: 10pt ;
    margin-top: 0pt;
    margin-bottom: 0pt;
  }
  p {
    font-size: 10pt;
    margin-top: 0pt;
    margin-bottom: 1em;
  }
</style>
</head>
<body>

<p class="bold_header">S + Click</p>
<p>Transfer the working [S]upervoxel under the cursor to the selected segmentation.</p>

<p class="bold_header">D + Click</p>
<p>[D]elete the label under the cursor in the selected segmentation.</p>

<p class="bold_header">C + Click</p>
<p>[C]ut the initial label under the cursor from the current working supervoxel.</p>

<p class="bold_header">F + Click</p>
<p>[F]ill holes in the segmentation label with the initial label under the cursor.</p>

<p class="bold_header">G + Click</p>
<p>Run an [O]pening operation on the segmentation label under the cursor.</p>

<p class="bold_header">H + Click</p>
<p>Insert Segment from Segmentation into initial nodes.</p>

<p class="bold_header">E</p>
<p>[E]xport debug information.</p>

<p class="bold_header">X + Click</p>
<p>Split working node into its initial nodes.</p>

<p class="bold_header">Q + Click</p>
<p>In paint mode, set the brush to the label ID of the segmentation under the cursor.</p>

<p class="bold_header">Left/Right Click in Paint Mode</p>
<p>Left: add to the current label ID. Right: remove from the current label ID.</p>

<p class="bold_header">+/-</p>
<p>Zoom to/away from cursor.</p>

<p class="bold_header">Arrow Up/Down</p>
<p>Increase/Decrease slice index (go up/down in stack).</p>

<p class="bold_header">V</p>
<p>Export Image Series for generation of [V]ideos.</p>

<p class="bold_header">U</p>
<p>Export Screenshot of current views.</p>

<p class="bold_header">0-9</p>
<p>Change brush size (1 smallest, 10 biggest).</p>

<p class="bold_header">R</p>
<p>Apply a [R]andom color scheme to the working supervoxels.</p>

<p class="bold_header">P + Click</p>
<p>Refine by [P]osition of the cursor with the selected refinement.</p>

<p class="bold_header">J + Click</p>
<p>Dilate the clicked segmentation label by one step.</p>

<p class="bold_header">K + Click</p>
<p>Erode the clicked segmentation label by one step.</p>

<p class="bold_header">CMD + Click</p>
<p>Set other orthogonal views to slice through the point under the cursor.</p>

<p class="bold_header">F7</p>
<p>Run the connected component split filter with the selected target and connectivity.</p>

<p class="bold_header">F8</p>
<p>Open the Segment Feature Table: shape features for all labels in the selected segmentation, sortable and color-coded. Click a row to navigate to that label.</p>

<p class="bold_header">T</p>
<p>Hold T and click a segment to open the cut-enabled 3D view. A selected segmentation is always required because applying a cut automatically transfers every resulting part into it as a separate label. When "Use Selected Segmentation For 3D Views/Cuts" is enabled, the clicked component from the selected segmentation is reused if it already matches a working segment, or inserted into the working graph first. When disabled, the clicked working segment opens directly.</p>

<p class="bold_header">F9</p>
<p>Jump to explicit X, Y, Z coordinates.</p>

<p class="bold_header">F10</p>
<p>Jump to a label ID in the selected segmentation.</p>

<p class="bold_header">M</p>
<p>Hold [M] and click a segment to open its 3D surface mesh view.</p>

<p class="bold_header">N</p>
<p>Open a 3D surface view of all segments at once.</p>

<p class="bold_header">Segmentations -> Open Segment in 3D Cut View...</p>
<p>Same as holding T. In the 3D dialog, orient the segment, press Draw Cut, paint the cut stroke, then Apply. All resulting cut parts are transferred automatically as separate labels into the selected segmentation. Press ? or F1 in that dialog for the step-by-step helper.</p>

</body>
</html>
)";

    QDialog dialog(this);
    dialog.setWindowTitle("Hotkeys");
    dialog.resize(800, 400);

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

void MainWindow::arm3DWorkingSegmentCut() {
    update3DWorkingSegmentCutActionState();
    if (splitWorkingSegment3DCutAction == nullptr || !splitWorkingSegment3DCutAction->isEnabled()) {
        return;
    }

    if (myOrthowindow != nullptr) {
        myOrthowindow->setAnnotationToolMode(SliceViewer::ToolMode::View3DCut);
        myOrthowindow->flashShortcutLegendKey("3dcut");
    }
    if (graphBase != nullptr && graphBase->useSelectedSegmentationFor3DView) {
        receiveStatusMessage(QStringLiteral(
            "Click a segment in the selected segmentation to prepare it in the working graph and open the 3D cut view; "
            "applying the cut transfers all resulting parts back as separate labels."));
    } else {
        receiveStatusMessage(QStringLiteral(
            "Click a working segment to open the 3D cut view; applying the cut transfers all resulting parts "
            "as separate labels into the selected segmentation."));
    }
}

void MainWindow::update3DWorkingSegmentCutActionState() {
    if (splitWorkingSegment3DCutAction == nullptr || mySignalControl == nullptr || taskRunner == nullptr) {
        return;
    }
    const bool usesSelectedSegmentation =
        graphBase != nullptr && graphBase->useSelectedSegmentationFor3DView;
    const bool selectedSegmentationAvailable =
        graphBase != nullptr && graphBase->pSelectedSegmentation != nullptr;
    splitWorkingSegment3DCutAction->setEnabled(
        mySignalControl->hasWorkingSegments() && selectedSegmentationAvailable && !taskRunner->isBusy());

    const QString sourceDescription = usesSelectedSegmentation
        ? tr("Open a segment from the selected segmentation in the 3D cut view; it is inserted into the working graph if needed. Applying the cut transfers every resulting part back as a separate label.")
        : tr("Open a working segment in the 3D cut view. A selected segmentation is required because applying the cut transfers every resulting part into it as a separate label.");
    splitWorkingSegment3DCutAction->setStatusTip(sourceDescription);
    splitWorkingSegment3DCutAction->setToolTip(sourceDescription);
}
