#include <QPainter>
#include <QDebug>
#include <QHash>
#include <QtWidgets>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <Qt>
#include <algorithm>
#include <itkLabelGeometryImageFilter.h>
#include <itkRegionOfInterestImageFilter.h>
#include <itkBinaryBallStructuringElement.h>
#include <itkBinaryDilateImageFilter.h>
#include <itkBinaryErodeImageFilter.h>
#include <itkBinaryMorphologicalClosingImageFilter.h>
#include <itkBinaryMorphologicalOpeningImageFilter.h>
#include <itkBinaryThresholdImageFunction.h>
#include <itkFloodFilledImageFunctionConditionalIterator.h>
#include "AnnotationSliceViewer.h"
#include "Segment3DViewerDialog.h"
#include "itkImageRegionIteratorWithIndex.h"
#include <unordered_set>
#include <unordered_map>
#include "src/utils/utils.h"
#include "OrthoViewer.h"
#include "src/qtUtils/TaskRunner.h"

namespace {

bool toolWorksWithoutWorkingSegments(SliceViewer::ToolMode tool) {
    return tool == SliceViewer::ToolMode::None ||
           tool == SliceViewer::ToolMode::Ctrl ||
           tool == SliceViewer::ToolMode::Delete ||
           tool == SliceViewer::ToolMode::SelectColor ||
           tool == SliceViewer::ToolMode::Fill ||
           tool == SliceViewer::ToolMode::Open;
}

dataType::SegmentsImageType::RegionType paddedLabelRegion(
    dataType::SegmentsImageType::Pointer image,
    dataType::SegmentIdType label,
    int padding) {
    auto [fx, fy, fz, tx, ty, tz] = utils::calculateBoundingBoxForLabel(image, label);
    const auto fullRegion = image->GetLargestPossibleRegion();
    const auto fullIndex = fullRegion.GetIndex();
    const auto fullSize = fullRegion.GetSize();

    dataType::SegmentsImageType::IndexType roiIndex;
    dataType::SegmentsImageType::SizeType roiSize;
    roiIndex[0] = std::max<int>(fullIndex[0], fx - padding);
    roiIndex[1] = std::max<int>(fullIndex[1], fy - padding);
    roiIndex[2] = std::max<int>(fullIndex[2], fz - padding);

    const int maxX = std::min<int>(fullIndex[0] + static_cast<int>(fullSize[0]) - 1, tx + padding);
    const int maxY = std::min<int>(fullIndex[1] + static_cast<int>(fullSize[1]) - 1, ty + padding);
    const int maxZ = std::min<int>(fullIndex[2] + static_cast<int>(fullSize[2]) - 1, tz + padding);
    roiSize[0] = std::max<dataType::SegmentsImageType::SizeType::SizeValueType>(1, maxX - roiIndex[0] + 1);
    roiSize[1] = std::max<dataType::SegmentsImageType::SizeType::SizeValueType>(1, maxY - roiIndex[1] + 1);
    roiSize[2] = std::max<dataType::SegmentsImageType::SizeType::SizeValueType>(1, maxZ - roiIndex[2] + 1);
    return {roiIndex, roiSize};
}

void logAnnotationViewerState(const QString &key, const QString &message) {
    static QHash<QString, QString> lastLogs;
    if (lastLogs.value(key) == message) {
        return;
    }

    qInfo().noquote() << message;
    lastLogs.insert(key, message);
}

QString summarizeAnnotationSignalImageRects(const std::vector<SliceViewerITKSignal *> &signalList) {
    QStringList entries;
    entries.reserve(static_cast<int>(signalList.size()));
    for (auto *signal : signalList) {
        if (signal == nullptr || !signal->getIsActive()) {
            continue;
        }

        const QImage *image = signal->getAddressSliceQImage();
        if (image == nullptr) {
            entries << "null";
            continue;
        }

        entries << QString("%1x%2").arg(image->width()).arg(image->height());
    }

    if (entries.isEmpty()) {
        return "none";
    }

    return entries.join(",");
}

void setLinkedToolMode(std::vector<SliceViewer *> &linkedViewerList, SliceViewer::ToolMode toolMode) {
    for (auto *viewer : linkedViewerList) {
        if (viewer != nullptr) {
            viewer->activeTool = toolMode;
        }
    }
}

void clearMatchingLinkedToolMode(std::vector<SliceViewer *> &linkedViewerList, SliceViewer::ToolMode toolMode) {
    for (auto *viewer : linkedViewerList) {
        if (viewer != nullptr && viewer->activeTool == toolMode) {
            viewer->activeTool = SliceViewer::ToolMode::None;
        }
    }
}

}


AnnotationSliceViewer::AnnotationSliceViewer(std::shared_ptr<GraphBase> graphBaseIn,
                                             TaskRunner *taskRunnerIn,
                                             QWidget *parent,
                                             bool)
    : SliceViewer(graphBaseIn, taskRunnerIn, parent) {
    if (verbose) { std::cout << "AnnotationSliceViewer: Constructor\n"; }

    paintModeIsActive = false;
    paintBoundaryModeIsActive = false;

    pThresholdedBoundaries = nullptr;

    ROISelectionModeIsActive = false;
    ROISelectionRubberBand = nullptr;

    labelOfClickedSegmentation = 0;

    rightClicked = false;

    scribbling = false;

    this->setMouseTracking(true);

}

dataType::SegmentsImageType::Pointer AnnotationSliceViewer::active3DViewSegmentsImage() const {
    if (graphBase == nullptr) {
        return nullptr;
    }
    if (graphBase->useSelectedSegmentationFor3DView) {
        return graphBase->pSelectedSegmentation;
    }
    return graphBase->pWorkingSegmentsImage;
}

itkSignal<dataType::SegmentIdType> *AnnotationSliceViewer::active3DViewSignal() const {
    if (graphBase == nullptr) {
        return nullptr;
    }
    if (graphBase->useSelectedSegmentationFor3DView) {
        return graphBase->pSelectedSegmentationSignal;
    }
    return graphBase->pWorkingSegments;
}

void AnnotationSliceViewer::notifyOrthoViewerInteractionModeChanged() {
    auto *viewer = orthoViewer();
    if (viewer != nullptr) {
        viewer->refreshInteractionModeIndicators();
    }
}

// Keep the common key-driven tool transitions in one small path.
// Paint and ROI mode toggles still notify explicitly in their own methods.
void AnnotationSliceViewer::setLinkedToolModeAndNotify(std::vector<SliceViewer *> &viewerList, ToolMode toolMode) {
    setLinkedToolMode(viewerList, toolMode);
    notifyOrthoViewerInteractionModeChanged();
}


void AnnotationSliceViewer::paintEvent(QPaintEvent *event) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    std::lock_guard<std::mutex> lock(signalListMutex);

    if (verbose) {
        std::cout << "AnnotationViewer: Paintevent triggered: " << event->rect().width()
                  << " x " << event->rect().height() << std::endl;
        printf("x0: %d y0: %d x1: %d y1: %d\n",
               event->rect().topLeft().x(),
               event->rect().topLeft().y(),
               event->rect().width(),
               event->rect().height());
    }
    double tic = utils::tic();

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const QRect targetRect = rect();
    const QRect sourceRect = backGroundImage.rect();

    painter.drawImage(targetRect, backGroundImage, sourceRect);

    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) { std::cout << "AnnotationViewer: Painting new signal" << std::endl; }
            painter.drawImage(targetRect,
                              *(signal->getAddressSliceQImage()),
                              signal->getAddressSliceQImage()->rect());
        }
    }

    painter.drawImage(targetRect, annotationImage, annotationImage.rect());

    // update sliceIndicatorImage in own viewer (draw other indicator for other views in viewer)
    sliceIndicatorImage.fill(QColor(0, 0, 0, 0)); // erase old slice indicator image!
    for (auto *viewer : linkedViewerList) {
        drawOtherViewerSliceIndicator(viewer->getSliceAxis(), viewer->getSliceIndex());
    }
    painter.drawImage(targetRect, sliceIndicatorImage, sliceIndicatorImage.rect());

    const qreal scaleX = getCurrentSliceWidth() > 0 ? static_cast<qreal>(targetRect.width()) / static_cast<qreal>(getCurrentSliceWidth()) : 1.0;
    const qreal scaleY = getCurrentSliceHeight() > 0 ? static_cast<qreal>(targetRect.height()) / static_cast<qreal>(getCurrentSliceHeight()) : 1.0;
    const auto mapPointToDisplay = [scaleX, scaleY](qreal x, qreal y) {
        return QPointF((x + 0.5) * scaleX, (y + 0.5) * scaleY);
    };
    const qreal dotRadius = 5.0;
    int dotAlpha = 255;

    QColor xy_red = QColor(255, 0, 0, dotAlpha);
    QColor xz_green = QColor(0, 255, 0, dotAlpha);
    QColor yz_yellow = QColor(255, 255, 0, dotAlpha);


    switch (sliceAxis) {
        case 0:
            painter.setBrush(QBrush(xy_red));
            painter.drawEllipse(mapPointToDisplay(indexVerticalIndicator, lastMouseY), dotRadius, dotRadius);
            painter.setBrush(QBrush(xz_green));
            painter.drawEllipse(mapPointToDisplay(lastMouseZ, indexHorizontalIndicator), dotRadius, dotRadius);
            break;
        case 1:
            painter.setBrush(QBrush(yz_yellow));
            painter.drawEllipse(mapPointToDisplay(indexVerticalIndicator, lastMouseZ), dotRadius, dotRadius);
            painter.setBrush(QBrush(xy_red));
            painter.drawEllipse(mapPointToDisplay(lastMouseX, indexHorizontalIndicator), dotRadius, dotRadius);
            break;
        case 2:
            painter.setBrush(QBrush(yz_yellow));
            painter.drawEllipse(mapPointToDisplay(indexVerticalIndicator, lastMouseY), dotRadius, dotRadius);
            painter.setBrush(QBrush(xz_green));
            painter.drawEllipse(mapPointToDisplay(lastMouseX, indexHorizontalIndicator), dotRadius, dotRadius);
            break;
        default:
            throw (std::logic_error("SliceAxis not implemented!"));
    }

    const QString planeName = sliceAxis == 0 ? "YZ" : (sliceAxis == 1 ? "XZ" : "XY");
    const QString logKey = QString("AnnotationViewerPaint_%1").arg(planeName);
    const QString message = QString("[AnnotationViewerPaint %1] eventRect=%2,%3 %4x%5 widgetRect=%6,%7 %8x%9 "
                                    "widgetSize=%10x%11 zoom=%12 currentSlice=%13x%14 background=%15x%16 annotation=%17x%18 "
                                    "sliceIndicator=%19x%20 scale=%21x%22 activeSignalImages=%23")
            .arg(planeName)
            .arg(event->rect().x()).arg(event->rect().y()).arg(event->rect().width()).arg(event->rect().height())
            .arg(targetRect.x()).arg(targetRect.y()).arg(targetRect.width()).arg(targetRect.height())
            .arg(width()).arg(height())
            .arg(zoomFactor, 0, 'f', 6)
            .arg(getCurrentSliceWidth()).arg(getCurrentSliceHeight())
            .arg(backGroundImage.width()).arg(backGroundImage.height())
            .arg(annotationImage.width()).arg(annotationImage.height())
            .arg(sliceIndicatorImage.width()).arg(sliceIndicatorImage.height())
            .arg(scaleX, 0, 'f', 6).arg(scaleY, 0, 'f', 6)
            .arg(summarizeAnnotationSignalImageRects(signalList));
    logAnnotationViewerState(logKey, message);


    if (verbose) {    utils::toc(tic, "AnnotationViewer PaintEvent finished: ");}

}


void AnnotationSliceViewer::keyPressEvent(QKeyEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()) {
        std::cout << "Currently Calculating, not accepting more KeyPressEvents!" << std::endl;
        return;
    }
//    std::cout << event->key() << std::endl;
    if (event->key() == Qt::Key_R) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("r");
        }
        if (graphBase->pSelectedSegmentationSignal != nullptr) {
            graphBase->pSelectedSegmentationSignal->checkAndResizeLUT(graphBase->selectedSegmentationMaxSegmentId);
        }

        std::unordered_set<itkSignalBase *> randomizedSignals;
        for (auto *sliceSignal : signalList) {
            if (sliceSignal == nullptr) {
                continue;
            }

            itkSignalBase *signal = sliceSignal->getSignal();
            if (signal == nullptr || !randomizedSignals.insert(signal).second) {
                continue;
            }

            signal->randomizeCategoricalLUT();
        }

        if (graphBase->pWorkingSegments != nullptr && !graphBase->ignoredSegmentLabels.empty()) {
            graphBase->pWorkingSegments->setLUTValueToBlack(graphBase->ignoredSegmentLabels.front());
        }
        for (auto *viewer : linkedViewerList) {
            viewer->recalculateQImages();
        }
    } else if (event->key() == Qt::Key_Plus) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("zoom");
        }
        modifyZoomInAllViewers(2);
    } else if (event->key() == Qt::Key_Minus) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("zoom");
        }
        modifyZoomInAllViewers(0.5);
    } else if (event->key() == Qt::Key_1) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(5);
    } else if (event->key() == Qt::Key_2) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(10);
    } else if (event->key() == Qt::Key_3) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(15);
    } else if (event->key() == Qt::Key_4) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(20);
    } else if (event->key() == Qt::Key_5) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(25);
    } else if (event->key() == Qt::Key_6) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(30);
    } else if (event->key() == Qt::Key_7) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(35);
    } else if (event->key() == Qt::Key_8) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(55);
    } else if (event->key() == Qt::Key_9) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(75);
    } else if (event->key() == Qt::Key_0) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(100);
    } else if (event->key() == Qt::Key_Up) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("slice");
        }
        incrementSliceIndex();
    } else if (event->key() == Qt::Key_Down) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("slice");
        }
        decrementSliceIndex();
    } else if (event->key() == Qt::Key_X) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Split);
    } else if (event->key() == Qt::Key_C) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Cut);
    } else if (event->key() == Qt::Key_Control) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Ctrl);
    } else if (event->key() == Qt::Key_U) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("u");
        }
        this->exportView();
    } else if (event->key() == Qt::Key_V) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("v");
        }
        this->exportVideo();
    } else if (event->key() == Qt::Key_S) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Transfer);
    } else if (event->key() == Qt::Key_P) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Refine);
    } else if (event->key() == Qt::Key_Q) {
        if (isPaintModeActive() || isPaintBoundaryModeActive()) {
            setLinkedToolModeAndNotify(linkedViewerList, ToolMode::SelectColor);
        }
    } else if (event->key() == Qt::Key_D) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Delete);
    } else if (event->key() == Qt::Key_F) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Fill);
    } else if (event->key() == Qt::Key_G) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Open);
    } else if (event->key() == Qt::Key_J) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Dilate);
    } else if (event->key() == Qt::Key_K) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Erode);
    } else if (event->key() == Qt::Key_H) {
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::Insert);
    } else if (event->key() == Qt::Key_E) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("e");
        }
        exportDebugInformation();
//        graphBase->pGraph->printMergeTreeToFile("mergeTree.txt");
//        graphBase->pGraph->printEdgesToFile("edges.txt");
//        graphBase->pGraph->printEdgeIdLookupToFile("edgeIds.txt");
//        graphBase->pGraph->writeInitialEdgesToFile("initialEdges.nrrd");
//    } else if(event->key() == Qt::Key_F) {
//        graphBase->pGraph->printMergeTreeToFile("mergeTree.txt");
    } else if (event->key() == Qt::Key_M) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("m");
        }
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::View3D);
    } else if (event->key() == Qt::Key_N) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("n");
        }
        show3DAllLabelsView();
    }
}

void AnnotationSliceViewer::showPrepared3DView(
    std::vector<std::pair<dataType::SegmentIdType, quint32>> labels,
    const QString &progressText)
{
    const auto segImage = active3DViewSegmentsImage();
    if (segImage == nullptr || labels.empty()) {
        return;
    }

    if (taskRunner == nullptr) {
        auto preparedScene = Segment3DViewerDialog::prepareScene(segImage, std::move(labels));
        auto *dialog = new Segment3DViewerDialog(std::move(preparedScene), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        return;
    }

    taskRunner->runWithLabel(
        progressText,
        [segImage, labels]() mutable {
            return Segment3DViewerDialog::prepareScene(segImage, std::move(labels));
        },
        [this](Segment3DViewerDialog::PreparedScene preparedScene) {
            auto *dialog = new Segment3DViewerDialog(std::move(preparedScene), this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        });
}

void AnnotationSliceViewer::show3DSegmentView(int posX, int posY) {
    const auto segImage = active3DViewSegmentsImage();
    if (segImage == nullptr) return;

    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    const dataType::SegmentIdType label = segImage->GetPixel({x, y, z});
    if (label == 0) return;

    quint32 lutColor = 0xFFAAAAAA;
    auto *activeSignal = active3DViewSignal();
    if (activeSignal != nullptr &&
        label < static_cast<dataType::SegmentIdType>(activeSignal->LUT.size())) {
        lutColor = activeSignal->LUT[label];
    }

    showPrepared3DView({{label, lutColor}}, "Preparing 3D segment view...");
}

void AnnotationSliceViewer::show3DAllLabelsView() {
    const auto segImage = active3DViewSegmentsImage();
    if (segImage == nullptr) return;

    const auto *buf = segImage->GetBufferPointer();
    const auto &sz = segImage->GetLargestPossibleRegion().GetSize();
    const size_t total = sz[0] * sz[1] * sz[2];
    auto *activeSignal = active3DViewSignal();

    std::unordered_map<dataType::SegmentIdType, quint32> labelColors;
    for (size_t i = 0; i < total; ++i) {
        const dataType::SegmentIdType id = buf[i];
        if (id == 0 || labelColors.count(id)) continue;
        quint32 color = 0xFFAAAAAA;
        if (activeSignal != nullptr &&
            id < static_cast<dataType::SegmentIdType>(activeSignal->LUT.size())) {
            color = activeSignal->LUT[id];
        }
        labelColors[id] = color;
    }

    if (labelColors.empty()) return;

    std::vector<std::pair<dataType::SegmentIdType, quint32>> labels(labelColors.begin(), labelColors.end());
    showPrepared3DView(std::move(labels), "Preparing 3D view for all segments...");
}

void AnnotationSliceViewer::exportDebugInformation() {
    if (graphBase == nullptr || graphBase->pGraph == nullptr ||
        graphBase->pEdgesInitialSegmentsImage == nullptr ||
        graphBase->pWorkingSegmentsImage == nullptr) {
        QMessageBox::information(this,
                                 tr("Debug Export Unavailable"),
                                 tr("Load supervoxels before exporting debug information."));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        tr("Export Debug Information"),
        tr("This can export a lot of debug information into several files. Do you want to continue?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    std::cout << "Exporting Debug Information from AnnotationsliceViewer\n";
    graphBase->pGraph->exportDebugInformation();
}

void AnnotationSliceViewer::keyReleaseEvent(QKeyEvent *event) {

//    std::cout << "Release: " << event->key() << "\n";
    static const std::unordered_map<int, ToolMode> keyToToolMode = {
        {Qt::Key_Control, ToolMode::Ctrl},
        {Qt::Key_S,       ToolMode::Transfer},
        {Qt::Key_P,       ToolMode::Refine},
        {Qt::Key_D,       ToolMode::Delete},
        {Qt::Key_X,       ToolMode::Split},
        {Qt::Key_C,       ToolMode::Cut},
        {Qt::Key_Q,       ToolMode::SelectColor},
        {Qt::Key_F,       ToolMode::Fill},
        {Qt::Key_G,       ToolMode::Open},
        {Qt::Key_J,       ToolMode::Dilate},
        {Qt::Key_K,       ToolMode::Erode},
        {Qt::Key_H,       ToolMode::Insert},
        {Qt::Key_M,       ToolMode::View3D},
    };
    auto it = keyToToolMode.find(event->key());
    if (it != keyToToolMode.end()) {
        clearMatchingLinkedToolMode(linkedViewerList, it->second);
        notifyOrthoViewerInteractionModeChanged();
    }
}


void AnnotationSliceViewer::mousePressEvent(QMouseEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()) {
        return;
    }

    // Middle click is a general panning gesture across tools.
    // The overlay badge describes the primary tool actions, while pan stays available separately.
    if (event->button() == Qt::MiddleButton) {
        old_middle_click_translate_x_pos = event->pos().x();
        old_middle_click_translate_y_pos = event->pos().y();
        return;
    }

    const bool needs3DSource = activeTool == ToolMode::View3D && active3DViewSegmentsImage() != nullptr;
    if (graphBase->pWorkingSegmentsImage == nullptr &&
        !toolWorksWithoutWorkingSegments(activeTool) &&
        !needs3DSource) {
        return;
    }
    switch (activeTool) {
    case ToolMode::None:
        if (ROISelectionModeIsActive) {
            ROISelectionOrigin = event->pos();
            if (ROISelectionRubberBand != nullptr) { delete ROISelectionRubberBand; }
            ROISelectionRubberBand = new QRubberBand(QRubberBand::Line, this);
            ROISelectionRubberBand->setGeometry(QRect(ROISelectionOrigin, QSize(1, 1)));
            ROISelectionRubberBand->show();
        } else if (event->button() == Qt::LeftButton) {
            lastPoint = QPoint(event->pos().x() / zoomFactor, event->pos().y() / zoomFactor);
            scribbling = true;
            rightClicked = false;
            drawPoint(event->pos());
        } else if (event->button() == Qt::RightButton) {
            lastPoint = QPoint(event->pos().x() / zoomFactor, event->pos().y() / zoomFactor);
            rightClicked = true;
            scribbling = true;
            drawPoint(event->pos());
        }
        break;
    case ToolMode::Ctrl: {
        setAllViewersToXYZCoordinates(event->pos().x(), event->pos().y());
        int x, y, z;
        getXYZfromPixmapPos(event->pos().x(), event->pos().y(), x, y, z);
        if (sliceAxis == 0) {
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXY, x, y, zoomFactor);
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXZ, x, z, zoomFactor);
        } else if (sliceAxis == 1) {
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXY, x, y, zoomFactor);
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaZY, z, y, zoomFactor);
        } else if (sliceAxis == 2) {
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXZ, x, z, zoomFactor);
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaZY, z, y, zoomFactor);
        }
        break;
    }
    case ToolMode::Split:
        splitWorkingNodeIntoInitialNodes(event->pos().x(), event->pos().y());
        break;
    case ToolMode::Refine:
        refineSegmentByPosition(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    case ToolMode::Transfer:
        transferWorkingNodeToSegmentation(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->recalculateQImages(); }
        break;
    case ToolMode::Delete:
        deleteConnectedLabelFromSegmentation(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->recalculateQImages(); }
        break;
    case ToolMode::Cut:
        removeInitialSegmentFromWorkingSegmentAtClick(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->recalculateQImages(); }
        break;
    case ToolMode::SelectColor:
        getSegmentationLabelIdAtCursor(event->pos().x(), event->pos().y());
        break;
    case ToolMode::Fill:
        runFillSegmentationLabel(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    case ToolMode::Open:
        runOpenSegmentationLabel(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    case ToolMode::Dilate:
        runDilateSegmentationLabel(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    case ToolMode::Erode:
        runErodeSegmentationLabel(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    case ToolMode::Insert:
        runInsertSegmentationSegmentIntoInitialSegments(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    case ToolMode::View3D:
        show3DSegmentView(event->pos().x(), event->pos().y());
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::None);
        break;
    }

}

void AnnotationSliceViewer::runInsertSegmentationSegmentIntoInitialSegments(int posX, int posY){
    if (graphBase == nullptr || graphBase->pGraph == nullptr || graphBase->pSelectedSegmentation == nullptr) {
        QMessageBox::information(this,
                                 tr("Insert Unavailable"),
                                 tr("Load and select a segmentation before inserting a segment into the supervoxels."));
        return;
    }

    std::cout << "Running InsertSegmentationSegmentIntoInitialSegments\n";
    std::cout << "AnnotationSliceViewer::InsertSegmentationSegmentIntoInitialSegments called" << std::endl;
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Insert segment at position: %d %d %d\n", x, y, z);

    if (taskRunner == nullptr) {
        graphBase->pGraph->transferSegmentationSegmentToInitialSegment(x, y, z);
        if (orthoViewer() != nullptr) {
            orthoViewer()->refreshViewers();
        }
        return;
    }

    taskRunner->run(
        [this, x, y, z]() { graphBase->pGraph->transferSegmentationSegmentToInitialSegment(x, y, z); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::runOpenSegmentationLabel(int posX, int posY){
    std::cout << "Running OpenSegmentationLabel\n";
    taskRunner->run(
        [this, posX, posY]() { openSegmentationLabel(posX, posY); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::runFillSegmentationLabel(int posX, int posY){
    taskRunner->run(
        [this, posX, posY]() { fillSegmentationLabel(posX, posY); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::runDilateSegmentationLabel(int posX, int posY) {
    taskRunner->run(
        [this, posX, posY]() { dilateSegmentationLabel(posX, posY); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::runErodeSegmentationLabel(int posX, int posY) {
    taskRunner->run(
        [this, posX, posY]() { erodeSegmentationLabel(posX, posY); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

// openSegmentationLabel
void AnnotationSliceViewer::openSegmentationLabel(int posX, int posY){
    double tic = utils::tic();
    if (graphBase->pSelectedSegmentation == nullptr) {
        return;
    }
    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;

    double time_first_part = utils::tic("OpenSegmentationLabel first part started: ");
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Open segment at position: %d %d %d\n", x, y, z);


    // calculate labelmap
    // get roi
    // create new empty image
    // fill in 1:1 segment with floodfill
    // open
    // put back in

    dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x,y,z});
    std::cout << "Label at click position: " << labelAtClickPosition << "\n";

    if (labelAtClickPosition == backgroundLabel) {
        std::cout << "Label at click position matches the background label (" << backgroundLabel
                  << "), not refining.\n";
        return void();
    }

    auto [fx, fy, fz, tx, ty, tz] = utils::calculateBoundingBoxForLabel(graphBase->pSelectedSegmentation, labelAtClickPosition);

    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
    ROIExtractionFilterType::Pointer ROIExtractionFilter = ROIExtractionFilterType::New();
    ROIExtractionFilter->SetInput(graphBase->pSelectedSegmentation);

    dataType::SegmentsImageType::IndexType extractedCellIndex;
    extractedCellIndex.at(0) = fx;
    extractedCellIndex.at(1) = fy;
    extractedCellIndex.at(2) = fz;

    dataType::SegmentsImageType::SizeType extracedCellSize;
    extracedCellSize.at(0) = tx - fx + 1;
    extracedCellSize.at(1) = ty - fy + 1;
    extracedCellSize.at(2) = tz - fz + 1;

    dataType::SegmentsImageType::RegionType pExtracedCellROI(extractedCellIndex, extracedCellSize);
    ROIExtractionFilter->SetRegionOfInterest(pExtracedCellROI);

    dataType::SegmentsImageType ::Pointer pExtractedCell = ROIExtractionFilter->GetOutput();
    ROIExtractionFilter->Update();
    utils::toc(time_first_part, "OpenSegmentationLabel first part finished: ");

    double time_second_part = utils::tic("OpenSegmentationLabel second part started: ");
    using StructuringElementType = itk::BinaryBallStructuringElement<dataType::SegmentIdType , dataType::Dimension>;
    StructuringElementType structuringElement;
    structuringElement.SetRadius(openingRadius);
    structuringElement.CreateStructuringElement();

    using BinaryMorphologicalOpeningImageFilterType =
            itk::BinaryMorphologicalOpeningImageFilter<dataType::SegmentsImageType , dataType::SegmentsImageType, StructuringElementType>;
    BinaryMorphologicalOpeningImageFilterType::Pointer openingFilter = BinaryMorphologicalOpeningImageFilterType::New();
    openingFilter->SetInput(pExtractedCell);
    openingFilter->SetKernel(structuringElement);
    openingFilter->SetForegroundValue(labelAtClickPosition);
    openingFilter->Update();
    auto pExtractedCellClosed = openingFilter->GetOutput();

//    std::cout << "Safe border: " << openingFilter->GetSafeBorder() << "\n";
    utils::toc(time_second_part, "OpenSegmentationLabel second part finished: ");
//
////     try the same thing with a fast binary dil and erode
////    this is not faster!
//    auto start_manual_open = utils::tic("Manual Open started: ");
////    BinaryDilateImageFilter
//    using BinaryDilateImageFilterType = itk::BinaryDilateImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType, StructuringElementType>;
//    BinaryDilateImageFilterType::Pointer dilateFilter = BinaryDilateImageFilterType::New();
//    dilateFilter->SetInput(pExtractedCell);
//    dilateFilter->SetKernel(structuringElement);
//    dilateFilter->SetForegroundValue(labelAtClickPosition);
//    dilateFilter->Update();
//
//    using BinaryErodeImageFilterType = itk::BinaryErodeImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType, StructuringElementType>;
//    BinaryErodeImageFilterType::Pointer erodeFilter = BinaryErodeImageFilterType::New();
//    erodeFilter->SetInput(dilateFilter->GetOutput());
//    erodeFilter->SetKernel(structuringElement);
//    erodeFilter->SetForegroundValue(labelAtClickPosition);
//    erodeFilter->Update();
//
//    auto pExtractedCellClosedManual = erodeFilter->GetOutput();
//    utils::toc(start_manual_open, "Manual Open finished: ");






//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCellClosed,
//                                                                  "/home/greinerj/testClosed.nrrd");
//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCell,
//                                                                   "/home/greinerj/test.nrrd");

    // delete old label; this is only needed if more is done than closing
    itk::ImageRegionIterator<dataType::SegmentsImageType> itDelete(graphBase->pSelectedSegmentation, pExtracedCellROI);
    itDelete.GoToBegin();
    while (!itDelete.IsAtEnd()) {
        if(itDelete.Get() == labelAtClickPosition){
            itDelete.Set(backgroundLabel);
        }
        ++itDelete;
    }

    // insert new label, if d
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(pExtractedCellClosed, pExtractedCellClosed->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        if(it.Get() == labelAtClickPosition){
            dataType::SegmentsImageType::IndexType newIndex = it.GetIndex();
            newIndex[0] += fx;
            newIndex[1] += fy;
            newIndex[2] += fz;
            graphBase->pSelectedSegmentation->SetPixel(newIndex, labelAtClickPosition);
        }
        ++it;
    }

    utils::toc(tic, "OpenSegmentationLabel finished: ");
}



void AnnotationSliceViewer::fillSegmentationLabel(int posX, int posY){
    double tic = utils::tic();
    if (graphBase->pSelectedSegmentation == nullptr) {
        return;
    }
    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;

    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Filling holes segments at position: %d %d %d\n", x, y, z);

    // calculate labelmap

    // get roi

    // create new empty image

    // fill in 1:1 segment with floodfill

    // close

    // put back in
    dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x,y,z});
    std::cout << "Label at click position: " << labelAtClickPosition << "\n";

    if (labelAtClickPosition == backgroundLabel) {
        std::cout << "Label at click position matches the background label (" << backgroundLabel
                  << "), not refining.\n";
        return void();
    }


    auto [fx, fy, fz, tx, ty, tz] = utils::calculateBoundingBoxForLabel(graphBase->pSelectedSegmentation, labelAtClickPosition);

    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
    ROIExtractionFilterType::Pointer ROIExtractionFilter = ROIExtractionFilterType::New();
    ROIExtractionFilter->SetInput(graphBase->pSelectedSegmentation);

    dataType::SegmentsImageType::IndexType extractedCellIndex;
    extractedCellIndex.at(0) = fx;
    extractedCellIndex.at(1) = fy;
    extractedCellIndex.at(2) = fz;

    dataType::SegmentsImageType::SizeType extracedCellSize;
    extracedCellSize.at(0) = tx - fx + 1;
    extracedCellSize.at(1) = ty - fy + 1;
    extracedCellSize.at(2) = tz - fz + 1;

    dataType::SegmentsImageType::RegionType pExtracedCellROI(extractedCellIndex, extracedCellSize);
    ROIExtractionFilter->SetRegionOfInterest(pExtracedCellROI);

    dataType::SegmentsImageType ::Pointer pExtractedCell = ROIExtractionFilter->GetOutput();
    ROIExtractionFilter->Update();

    using StructuringElementType = itk::BinaryBallStructuringElement<dataType::SegmentIdType , dataType::Dimension>;
    StructuringElementType structuringElement;
    structuringElement.SetRadius(closingRadius);
    structuringElement.CreateStructuringElement();

    using BinaryMorphologicalClosingImageFilterType =
    itk::BinaryMorphologicalClosingImageFilter<dataType::SegmentsImageType , dataType::SegmentsImageType, StructuringElementType>;
    BinaryMorphologicalClosingImageFilterType::Pointer closingFilter = BinaryMorphologicalClosingImageFilterType::New();
    closingFilter->SetInput(pExtractedCell);
    closingFilter->SetKernel(structuringElement);
    closingFilter->SetForegroundValue(labelAtClickPosition);
    closingFilter->Update();
    std::cout << "Safe border: " << closingFilter->GetSafeBorder() << "\n";

    auto pExtractedCellClosed = closingFilter->GetOutput();

//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCellClosed,
//                                                                  "/home/greinerj/testClosed.nrrd");
//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCell,
//                                                                   "/home/greinerj/test.nrrd");

    // delete old label; this is only needed if more is done than closing
    itk::ImageRegionIterator<dataType::SegmentsImageType> itDelete(graphBase->pSelectedSegmentation, pExtracedCellROI);
    itDelete.GoToBegin();
    while (!itDelete.IsAtEnd()) {
        if(itDelete.Get() == labelAtClickPosition){
            itDelete.Set(backgroundLabel);
        }
        ++itDelete;
    }

    // insert new label, if d
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(pExtractedCellClosed, pExtractedCellClosed->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        if(it.Get() == labelAtClickPosition){
            dataType::SegmentsImageType::IndexType newIndex = it.GetIndex();
            newIndex[0] += fx;
            newIndex[1] += fy;
            newIndex[2] += fz;
            graphBase->pSelectedSegmentation->SetPixel(newIndex, labelAtClickPosition);
        }
        ++it;
    }

    utils::toc(tic, "FillSegmentationLabel finished: ");
}

void AnnotationSliceViewer::dilateSegmentationLabel(int posX, int posY) {
    if (graphBase->pSelectedSegmentation == nullptr) {
        return;
    }

    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    const dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
    if (labelAtClickPosition == backgroundLabel) {
        return;
    }

    const int radius = std::max(0, dilationRadius);
    const auto roi = paddedLabelRegion(graphBase->pSelectedSegmentation, labelAtClickPosition, radius);
    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
    auto roiExtractionFilter = ROIExtractionFilterType::New();
    roiExtractionFilter->SetInput(graphBase->pSelectedSegmentation);
    roiExtractionFilter->SetRegionOfInterest(roi);
    auto extractedCell = roiExtractionFilter->GetOutput();
    roiExtractionFilter->Update();

    using StructuringElementType = itk::BinaryBallStructuringElement<dataType::SegmentIdType, dataType::Dimension>;
    StructuringElementType structuringElement;
    structuringElement.SetRadius(radius);
    structuringElement.CreateStructuringElement();

    using BinaryDilateImageFilterType =
        itk::BinaryDilateImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType, StructuringElementType>;
    auto dilateFilter = BinaryDilateImageFilterType::New();
    dilateFilter->SetInput(extractedCell);
    dilateFilter->SetKernel(structuringElement);
    dilateFilter->SetForegroundValue(labelAtClickPosition);
    dilateFilter->Update();
    auto dilatedCell = dilateFilter->GetOutput();

    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(dilatedCell, dilatedCell->GetLargestPossibleRegion());
    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        if (it.Get() != labelAtClickPosition) {
            continue;
        }

        dataType::SegmentsImageType::IndexType newIndex = it.GetIndex();
        newIndex[0] += roi.GetIndex()[0];
        newIndex[1] += roi.GetIndex()[1];
        newIndex[2] += roi.GetIndex()[2];

        const auto existingLabel = graphBase->pSelectedSegmentation->GetPixel(newIndex);
        if (existingLabel == backgroundLabel || existingLabel == labelAtClickPosition) {
            graphBase->pSelectedSegmentation->SetPixel(newIndex, labelAtClickPosition);
        }
    }
}

void AnnotationSliceViewer::erodeSegmentationLabel(int posX, int posY) {
    if (graphBase->pSelectedSegmentation == nullptr) {
        return;
    }

    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    const dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
    if (labelAtClickPosition == backgroundLabel) {
        return;
    }

    const int radius = std::max(0, erosionRadius);
    const auto roi = paddedLabelRegion(graphBase->pSelectedSegmentation, labelAtClickPosition, radius);
    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
    auto roiExtractionFilter = ROIExtractionFilterType::New();
    roiExtractionFilter->SetInput(graphBase->pSelectedSegmentation);
    roiExtractionFilter->SetRegionOfInterest(roi);
    auto extractedCell = roiExtractionFilter->GetOutput();
    roiExtractionFilter->Update();

    using StructuringElementType = itk::BinaryBallStructuringElement<dataType::SegmentIdType, dataType::Dimension>;
    StructuringElementType structuringElement;
    structuringElement.SetRadius(radius);
    structuringElement.CreateStructuringElement();

    using BinaryErodeImageFilterType =
        itk::BinaryErodeImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType, StructuringElementType>;
    auto erodeFilter = BinaryErodeImageFilterType::New();
    erodeFilter->SetInput(extractedCell);
    erodeFilter->SetKernel(structuringElement);
    erodeFilter->SetForegroundValue(labelAtClickPosition);
    erodeFilter->Update();
    auto erodedCell = erodeFilter->GetOutput();

    itk::ImageRegionIterator<dataType::SegmentsImageType> deleteIt(graphBase->pSelectedSegmentation, roi);
    for (deleteIt.GoToBegin(); !deleteIt.IsAtEnd(); ++deleteIt) {
        if (deleteIt.Get() == labelAtClickPosition) {
            deleteIt.Set(backgroundLabel);
        }
    }

    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(erodedCell, erodedCell->GetLargestPossibleRegion());
    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        if (it.Get() != labelAtClickPosition) {
            continue;
        }

        dataType::SegmentsImageType::IndexType newIndex = it.GetIndex();
        newIndex[0] += roi.GetIndex()[0];
        newIndex[1] += roi.GetIndex()[1];
        newIndex[2] += roi.GetIndex()[2];
        graphBase->pSelectedSegmentation->SetPixel(newIndex, labelAtClickPosition);
    }
}

void AnnotationSliceViewer::setOpeningRadius(int radius) {
    openingRadius = std::max(0, radius);
}

void AnnotationSliceViewer::setClosingRadius(int radius) {
    closingRadius = std::max(0, radius);
}

void AnnotationSliceViewer::setDilationRadius(int radius) {
    dilationRadius = std::max(0, radius);
}

void AnnotationSliceViewer::setErosionRadius(int radius) {
    erosionRadius = std::max(0, radius);
}


void AnnotationSliceViewer::refineSegmentByPosition(int posX, int posY) {
    std::cout << "AnnotationSliceViewer::refineSegmentByPosition called" << std::endl;
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Refining segments at position: %d %d %d\n", x, y, z);

    taskRunner->run(
        [this, x, y, z]() { graphBase->pGraph->refineWithSelectedRefinementAtPosition(x, y, z); },
        [this]() {
            graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::splitWorkingNodeIntoInitialNodes(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    std::cout << "Splitting workingsegment into initial nodes at position: " << x << " " << y << " " << z << "\n";
    graphBase->pGraph->splitWorkingNodeIntoInitialNodes(x, y, z);
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    orthoViewer()->refreshViewers();
}

void AnnotationSliceViewer::removeInitialSegmentFromWorkingSegmentAtClick(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    std::cout << "Removing initialsegment from workingsegment at position: " << x << " " << y << " " << z << "\n";
    graphBase->pGraph->removeInitialNodeFromWorkingNodeAtPosition(x, y, z);

    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    orthoViewer()->refreshViewers();
}

void AnnotationSliceViewer::transferWorkingNodeToSegmentation(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Transfering WorkingNode to segmentation at position: %d %d %d\n", x, y, z);
    graphBase->pGraph->transferWorkingNodeToSegmentation(x, y, z);
}


void AnnotationSliceViewer::deleteConnectedLabelFromSegmentation(int posX, int posY) {
    double tic = utils::tic();
    if (graphBase->pSelectedSegmentation != nullptr && graphBase->pGraph != nullptr) {
        int x, y, z;
        getXYZfromPixmapPos(posX, posY, x, y, z);
        const dataType::SegmentIdType labelAtPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
        graphBase->pGraph->deleteSegmentationLabel(labelAtPosition);
    }
    utils::toc(tic, "DeleteConnectedLabelFromSegmentation finished: ");
}



void AnnotationSliceViewer::mouseMoveEvent(QMouseEvent *event) {
    int x, y, z;
    getXYZfromPixmapPos(event->pos().x(), event->pos().y(), x, y, z);

    QString logMessage = QString("x: %1/%2 y: %3/%4 z:%5/%6 sliceAxis:%7").arg(lastMouseX).arg(getDimX() - 1)
            .arg(lastMouseY).arg(getDimY() - 1)
            .arg(lastMouseZ).arg(getDimZ() - 1)
            .arg(sliceAxis);


    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            logMessage += " " + signal->getName() + ":" + signal->getNumberOfXYZAsString(x, y, z);
        }
    }

//        logMessage.sprintf("x: %01.0d y: %01.0d z: %01.0d", lastMouseY, lastMouseY, lastMouseZ);
    sendStatusMessage(logMessage);



    for (auto *viewer : linkedViewerList) {
        viewer->updateMousePosition(x, y, z);
        viewer->updateFunction();
    }


    if (activeTool == ToolMode::Ctrl) {
        if (event->buttons() == Qt::LeftButton) {
            setAllViewersToXYZCoordinates(event->pos().x(), event->pos().y());
        }
    } else if (scribbling) {
        if (event->buttons() == Qt::LeftButton) {
            drawLineTo(event->pos());
        } else if (event->buttons() == Qt::RightButton) {
            drawLineTo(event->pos());
        }
    } else if (event->buttons() == Qt::MiddleButton) {
        // Pan remains available independently of the active tool.
        // The badge shows tool-specific actions; this branch handles the shared viewport drag gesture.
        int current_x = event->pos().x();
        int current_y = event->pos().y();
        double scaleFactor = 0.4;
        double delta_x = scaleFactor * (current_x - old_middle_click_translate_x_pos);
        double delta_y = scaleFactor * (current_y - old_middle_click_translate_y_pos);

        QScrollAreaNoWheel *currentScrollArea;
        if (sliceAxis == 0) {
            currentScrollArea = orthoViewer()->scrollAreaZY;
        } else if (sliceAxis == 1) { // xz
            currentScrollArea = orthoViewer()->scrollAreaXZ;
        } else if (sliceAxis == 2) {
            currentScrollArea = orthoViewer()->scrollAreaXY;
        } else {
            throw std::logic_error("slice axis not implemented");
        }

        int current_horizontal_value = currentScrollArea->horizontalScrollBar()->value();
        int current_vertical_value = currentScrollArea->verticalScrollBar()->value();
        currentScrollArea->horizontalScrollBar()->setValue(current_horizontal_value + delta_x);
        currentScrollArea->verticalScrollBar()->setValue(current_vertical_value + delta_y);


        old_middle_click_translate_x_pos = event->pos().x();
        old_middle_click_translate_y_pos = event->pos().y();

    } else if(ROISelectionModeIsActive){
        if(event->buttons() == Qt::LeftButton) {
            if(ROISelectionRubberBand != nullptr) {
                ROISelectionRubberBand->setGeometry(QRect(ROISelectionOrigin, event->pos()).normalized());
            }
        }
    }
}

void AnnotationSliceViewer::mouseReleaseEvent(QMouseEvent *event) {
    if ((event->button() == Qt::LeftButton) && scribbling) {
        drawLineTo(event->pos());
        scribbling = false;
        processAnnotationImage(annotationImage);
        annotationImage.fill(QColor(0, 0, 0, 0));
        updateFunction();
    } else if ((event->button() == Qt::RightButton) && scribbling) {
        drawLineTo(event->pos());
        scribbling = false;
        processAnnotationImage(annotationImage);
        annotationImage.fill(QColor(0, 0, 0, 0));
        updateFunction();
    } else if (ROISelectionModeIsActive) {
        if (ROISelectionRubberBand != nullptr) {
            //TODO: think about zoom
            std::cout << "x: " << ROISelectionRubberBand->x() << " y: " << ROISelectionRubberBand->y();
            std::cout << " width: " << ROISelectionRubberBand->width() << " height: "
                      << ROISelectionRubberBand->height() << "\n";
            if (sliceAxis == 0) {
                graphBase->ROI_fz = static_cast<int>((ROISelectionRubberBand->x()) / zoomFactor);
                graphBase->ROI_tz = static_cast<int>((ROISelectionRubberBand->x() + ROISelectionRubberBand->width()) /
                                                     zoomFactor);
                graphBase->ROI_fy = static_cast<int>((ROISelectionRubberBand->y()) / zoomFactor);
                graphBase->ROI_ty = static_cast<int>((ROISelectionRubberBand->y() + ROISelectionRubberBand->height()) /
                                                     zoomFactor);
                graphBase->ROI_fx = 0;
                graphBase->ROI_tx = graphBase->pWorkingSegments->getDimX();
            } else if (sliceAxis == 1) {
                graphBase->ROI_fx = static_cast<int>((ROISelectionRubberBand->x()) / zoomFactor);
                graphBase->ROI_tx = static_cast<int>((ROISelectionRubberBand->x() + ROISelectionRubberBand->width()) /
                                                     zoomFactor);
                graphBase->ROI_fz = static_cast<int>((ROISelectionRubberBand->y()) / zoomFactor);
                graphBase->ROI_tz = static_cast<int>((ROISelectionRubberBand->y() + ROISelectionRubberBand->height()) /
                                                     zoomFactor);
                graphBase->ROI_fy = 0;
                graphBase->ROI_ty = graphBase->pWorkingSegments->getDimY();
            } else if (sliceAxis == 2) {
                graphBase->ROI_fx = static_cast<int>((ROISelectionRubberBand->x()) / zoomFactor);
                graphBase->ROI_tx = static_cast<int>((ROISelectionRubberBand->x() + ROISelectionRubberBand->width()) /
                                                     zoomFactor);
                graphBase->ROI_fy = static_cast<int>((ROISelectionRubberBand->y()) / zoomFactor);
                graphBase->ROI_ty = static_cast<int>((ROISelectionRubberBand->y() + ROISelectionRubberBand->height()) /
                                                     zoomFactor);
                graphBase->ROI_fz = 0;
                graphBase->ROI_tz = graphBase->pWorkingSegments->getDimZ();
            }
            graphBase->ROI_set = true;
//            ROISelectionRubberBand->hide();
        }
    }
}

void AnnotationSliceViewer::toggleROISelectonModeIsActive() {
    if (ROISelectionModeIsActive) {
        turnROISelectonModeInactive();
    } else {
        turnROISelectonModeActive();
    }
}

void AnnotationSliceViewer::turnROISelectonModeInactive() {
    ROISelectionModeIsActive = false;
    if (ROISelectionRubberBand != nullptr) {
        ROISelectionRubberBand->hide();
    }
    notifyOrthoViewerInteractionModeChanged();
}


void AnnotationSliceViewer::turnROISelectonModeActive() {
    ROISelectionModeIsActive = true;
    if (ROISelectionRubberBand != nullptr) {
        ROISelectionRubberBand->show();
    }
    notifyOrthoViewerInteractionModeChanged();
}



void AnnotationSliceViewer::drawPoint(QPoint point) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    point.setX(point.x() / zoomFactor);
    point.setY(point.y() / zoomFactor);
    std::cout << "Drawing point: " << point.x() << " " << point.y() << "\n";

    if ((point.x() < 0) | (point.y() < 0) | (point.x() >= annotationImage.width()) | (point.y() >= annotationImage.height())) {
        return;
    }


    QPainter painter(&annotationImage);
    if (paintModeIsActive | paintBoundaryModeIsActive) {
        myPenColor = rightClicked ? Qt::black : cursorColor;
    } else {
        myPenColor = rightClicked ? Qt::red : Qt::green;
    }
    painter.setPen(QPen(myPenColor, myPenWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawPoint(point);

    // while the point is drawn on the annotationimage as normal, update() works on the scaled picture!
    int topLeftPoint_x = static_cast<int>((point.x() - myPenWidth) * zoomFactor);
    int topLeftPoint_y = static_cast<int>((point.y() - myPenWidth) * zoomFactor);
    int updateRect_width = static_cast<int>((2 * myPenWidth) * zoomFactor);
    int updateRect_height = static_cast<int>((2 * myPenWidth) * zoomFactor);
    update(QRect(topLeftPoint_x, topLeftPoint_y, updateRect_width, updateRect_height));
//    update();
}

void AnnotationSliceViewer::drawLineTo(QPoint endPoint) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());


    endPoint.setX(endPoint.x() / zoomFactor);
    endPoint.setY(endPoint.y() / zoomFactor);
    std::cout << "Drawing line to: " << endPoint.x() << " " << endPoint.y() << "\n";

    if ((endPoint.x() < 0) | (endPoint.y() < 0) | (endPoint.x() >= annotationImage.width()) | (endPoint.y() >= annotationImage.height())) {
        std::cout << "Point outside image\n";
        return;
    }

    QPainter painter(&annotationImage);
    if (paintModeIsActive | paintBoundaryModeIsActive) {
        myPenColor = rightClicked ? Qt::black : cursorColor;
    } else {
        myPenColor = rightClicked ? Qt::red : Qt::green;
    }

    painter.setPen(QPen(myPenColor, myPenWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawLine(lastPoint, endPoint);

    int rad = (myPenWidth / 2) + 2;
    update(QRect(lastPoint, endPoint).normalized()
                   .adjusted(-rad, -rad, +rad, +rad));
    lastPoint = endPoint;
}


void AnnotationSliceViewer::updatePenWidthInAllViewers(int newPenWidth) {
    orthoViewer()->xy->setPenWidth(newPenWidth);
    orthoViewer()->zy->setPenWidth(newPenWidth);
    orthoViewer()->xz->setPenWidth(newPenWidth);
}


void AnnotationSliceViewer::setPenWidth(int newPenWidth) {
    myPenWidth = newPenWidth;
    setUpCustomCursor();
}


void AnnotationSliceViewer::processAnnotationImage(QImage image) {
    const bool canEditEdges = graphBase->pEdgesInitialSegmentsImage != nullptr;
    const bool canEditBoundaries = pThresholdedBoundaries != nullptr;
    const bool canEditSegmentation = graphBase->pSelectedSegmentation != nullptr;
    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;
    if (canEditEdges || canEditBoundaries || canEditSegmentation) {
        //TODO: Separate function into smaller parts, make sliceblabla general

        int bytesPerPixel = 4;
        unsigned char *bits = image.bits(); // pointer to the first image data
        std::vector<unsigned char> annotationImageRed(image.width() * image.height(), 0);
        std::vector<unsigned char> annotationImageGreen(image.width() * image.height(), 0);
        std::vector<unsigned char> annotationImageBlue(image.width() * image.height(), 0);
        std::vector<unsigned char> annotationImageAlpha(image.width() * image.height(), 0);

        int x = 0;
        int y = 0;

        int offset_1;
        int offset_2;
        int offset_3;
        int vecIndex;
        for (y = 0; y < image.height(); y++) {
            offset_1 = y * image.width();
            offset_2 = offset_1 * bytesPerPixel;
            for (x = 0; x < image.width(); x++) {
                offset_3 = x * bytesPerPixel + offset_2;
                vecIndex = x + offset_1;
                annotationImageRed[vecIndex] = bits[offset_3];
                annotationImageGreen[vecIndex] = bits[1 + offset_3];
                annotationImageBlue[vecIndex] = bits[2 + offset_3];
                annotationImageAlpha[vecIndex] = bits[3 + offset_3];
            }
        }


        if (!paintModeIsActive && !paintBoundaryModeIsActive) { // edge merging/unmerging modus
            if (canEditEdges) {
                std::set<unsigned int> annotatedEdgeNumIdsToMerge;
                std::set<unsigned int> annotatedEdgeNumIdsToUnmerge;

                for (y = 0; y < image.height(); y++) {
                    for (x = 0; x < image.width(); x++) {
                        int worldX, worldY, worldZ;
                        if (annotationImageRed[x + image.width() * y] == 255) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            int edgeNumId = graphBase->pEdgesInitialSegmentsImage->GetPixel({worldX, worldY, worldZ});;
                            if (edgeNumId != 0) {
                                //std::cout << "Unmerge: EdgeNumId: " << edgeNumId << " at position: " << worldX << " " << worldY << " " << worldZ << "\n";
                                annotatedEdgeNumIdsToUnmerge.insert(edgeNumId);
                            }
                        } else if (annotationImageGreen[x + image.width() * y] == 255) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            int edgeNumId = graphBase->pEdgesInitialSegmentsImage->GetPixel({worldX, worldY, worldZ});;
                            if (edgeNumId != 0) {
                                //std::cout << "Merge: EdgeNumId: " << edgeNumId << " at position: " << worldX << " " << worldY << " " << worldZ << "\n";
                                annotatedEdgeNumIdsToMerge.insert(edgeNumId);
                            }
                        }
                    }
                }

                if (!annotatedEdgeNumIdsToMerge.empty()) {
                    graphBase->pGraph->mergeEdges(annotatedEdgeNumIdsToMerge);
                }
                for (auto &annotatedEdgeId : annotatedEdgeNumIdsToMerge) {
                    graphBase->edgeStatus[annotatedEdgeId] = 2;
                }

                if (!annotatedEdgeNumIdsToUnmerge.empty()) {
                    graphBase->pGraph->unmergeEdges(annotatedEdgeNumIdsToUnmerge);
                }
                for (auto &annotatedEdgeId : annotatedEdgeNumIdsToUnmerge) {
                    graphBase->edgeStatus[annotatedEdgeId] = -2;
                }

                // Todo: Update LUT calculation for Edges
                // only update if changes were done
                if (!annotatedEdgeNumIdsToMerge.empty() || !annotatedEdgeNumIdsToUnmerge.empty()) {
                    if (!annotatedEdgeNumIdsToMerge.empty()) {
                        graphBase->pEdgesInitialSegmentsITKSignal->updateLUTEdge(annotatedEdgeNumIdsToMerge);
                    }
                    if (!annotatedEdgeNumIdsToUnmerge.empty()) {
                        graphBase->pEdgesInitialSegmentsITKSignal->updateLUTEdge(annotatedEdgeNumIdsToUnmerge);
                    }

                    for (auto *viewer : linkedViewerList) {
                        viewer->recalculateQImages();
                    }
                }
            }
        } else { // edit the segmentation modus
            if (labelOfClickedSegmentation != backgroundLabel) {
                for (y = 0; y < image.height(); y++) {
                    for (x = 0; x < image.width(); x++) {
                        int worldX, worldY, worldZ;
                        unsigned char r, g, b, a;
                        r = annotationImageRed[x + image.width() * y];
                        g = annotationImageGreen[x + image.width() * y];
                        b = annotationImageBlue[x + image.width() * y];
                        a = annotationImageAlpha[x + image.width() * y];

                        // insert color
                        if (QColor(r, g, b, a) == (cursorColor)) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            if (paintModeIsActive && canEditSegmentation) {
                                graphBase->pSelectedSegmentation->SetPixel({worldX, worldY, worldZ},
                                                                           labelOfClickedSegmentation);
                            } else {
                                if (canEditBoundaries) {
                                    pThresholdedBoundaries->SetPixel({worldX, worldY, worldZ},
                                                          labelOfClickedSegmentation);
                                }
                            }
                        } else if (QColor(r, g, b, a) == Qt::black) { // delete stuff
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            if (paintModeIsActive && canEditSegmentation) {
                                if (graphBase->pSelectedSegmentation->GetPixel({worldX, worldY, worldZ}) ==
                                    labelOfClickedSegmentation) {
                                    graphBase->pSelectedSegmentation->SetPixel({worldX, worldY, worldZ}, backgroundLabel);
                                }
                            } else {
                                if (canEditBoundaries) {
                                    pThresholdedBoundaries->SetPixel({worldX, worldY, worldZ}, 0);
                                }
                            }
                        }
                    }
                }
            }
            for (auto *viewer : linkedViewerList) {
                viewer->recalculateQImages();
            }
        }
    }
}


void AnnotationSliceViewer::resetQImages() {
    if (getCurrentSliceWidth() <= 0 || getCurrentSliceHeight() <= 0) {
        qWarning() << "AnnotationSliceViewer::resetQImages() called with invalid dimensions!";
        return;
    }
    backGroundImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                             static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    backGroundImage.fill(Qt::black);

    annotationImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                             static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    annotationImage.fill(QColor(0, 0, 0, 0));
    sliceIndicatorImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                                 static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    sliceIndicatorImage.fill(QColor(0, 0, 0, 0));
    setPixmap(QPixmap::fromImage(annotationImage));
    syncViewerSizeToImage();

    const QString planeName = sliceAxis == 0 ? "YZ" : (sliceAxis == 1 ? "XZ" : "XY");
    const QString logKey = QString("AnnotationViewerReset_%1").arg(planeName);
    const QString message = QString("[AnnotationViewerReset %1] zoom=%2 currentSlice=%3x%4 background=%5x%6 annotation=%7x%8 sliceIndicator=%9x%10 widgetSize=%11x%12")
            .arg(planeName)
            .arg(zoomFactor, 0, 'f', 6)
            .arg(getCurrentSliceWidth()).arg(getCurrentSliceHeight())
            .arg(backGroundImage.width()).arg(backGroundImage.height())
            .arg(annotationImage.width()).arg(annotationImage.height())
            .arg(sliceIndicatorImage.width()).arg(sliceIndicatorImage.height())
            .arg(width()).arg(height());
    logAnnotationViewerState(logKey, message);
}


void AnnotationSliceViewer::updateFunction() {
    //    bool veryVerbose = false; // this function is not the bottleneck, takes approx 1e-5 secs
//    bool veryVerbose = true;
//    double t=0;
//    if (veryVerbose) { t = utils::tic("PaintStart"); }
    update();
//    if (veryVerboxse) { utils::toc(t, "PaintEnd"); }
}

void AnnotationSliceViewer::togglePaintMode() {
    paintModeIsActive = !paintModeIsActive;
    notifyOrthoViewerInteractionModeChanged();
}

void AnnotationSliceViewer::togglePaintBoundaryMode() {
    paintBoundaryModeIsActive = !paintBoundaryModeIsActive;
    notifyOrthoViewerInteractionModeChanged();
}

void AnnotationSliceViewer::setPaintId(dataType::SegmentIdType){
    if (graphBase == nullptr || graphBase->pSelectedSegmentationSignal == nullptr ||
        labelOfClickedSegmentation >= static_cast<dataType::SegmentIdType>(graphBase->pSelectedSegmentationSignal->LUT.size())) {
        return;
    }

    quint32 colorOfClickedSegment = graphBase->pSelectedSegmentationSignal->LUT[labelOfClickedSegmentation];
    unsigned char red, green, blue;
    red = (unsigned char) (colorOfClickedSegment >> 16);
    green = (unsigned char) (colorOfClickedSegment >> 8);
    blue = (unsigned char) (colorOfClickedSegment >> 0);
    cursorColor = QColor(red, green, blue);
    setUpCustomCursor();
}

void AnnotationSliceViewer::getSegmentationLabelIdAtCursor(int x, int y) {
    if (paintModeIsActive) {
        if (graphBase->pSelectedSegmentation != nullptr && graphBase->pSelectedSegmentationSignal != nullptr) {
            int xWorld, yWorld, zWorld;
            getXYZfromPixmapPos(x, y, xWorld, yWorld, zWorld);
            labelOfClickedSegmentation = graphBase->pSelectedSegmentation->GetPixel({xWorld, yWorld, zWorld});
            if (labelOfClickedSegmentation >=
                static_cast<dataType::SegmentIdType>(graphBase->pSelectedSegmentationSignal->LUT.size())) {
                return;
            }
            quint32 colorOfClickedSegment = graphBase->pSelectedSegmentationSignal->LUT[labelOfClickedSegmentation];
            unsigned char red, green, blue;
            red = (unsigned char) (colorOfClickedSegment >> 16);
            green = (unsigned char) (colorOfClickedSegment >> 8);
            blue = (unsigned char) (colorOfClickedSegment >> 0);
//        std::cout << labelOfClickedSegmentation << " " << (int)alpha << " " << (int)red << " " << (int)green << " " << (int)blue << "\n";
            cursorColor = QColor(red, green, blue);
            setUpCustomCursor();
        }
    } else if (paintBoundaryModeIsActive){
        if(pThresholdedBoundaries != nullptr && pThresholdedBoundariesSignal != nullptr) {
            int xWorld, yWorld, zWorld;
            getXYZfromPixmapPos(x, y, xWorld, yWorld, zWorld);
            labelOfClickedSegmentation = pThresholdedBoundaries->GetPixel({xWorld, yWorld, zWorld});
            if (labelOfClickedSegmentation >=
                static_cast<dataType::SegmentIdType>(pThresholdedBoundariesSignal->LUT.size())) {
                return;
            }
            quint32 colorOfClickedSegment = pThresholdedBoundariesSignal->LUT[labelOfClickedSegmentation];
            unsigned char red, green, blue;
            red = (unsigned char) (colorOfClickedSegment >> 16);
            green = (unsigned char) (colorOfClickedSegment >> 8);
            blue = (unsigned char) (colorOfClickedSegment >> 0);
//        std::cout << labelOfClickedSegmentation << " " << (int)alpha << " " << (int)red << " " << (int)green << " " << (int)blue << "\n";
            cursorColor = QColor(red, green, blue);
            setUpCustomCursor();
        }
    }
}

AnnotationSliceViewer::~AnnotationSliceViewer() {
    if (ROISelectionRubberBand != nullptr) {
        delete ROISelectionRubberBand;
    }

}
