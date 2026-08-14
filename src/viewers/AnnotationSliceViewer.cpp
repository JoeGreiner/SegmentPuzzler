#include <QPainter>
#include <QDateTime>
#include <QtWidgets>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <Qt>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
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
#include "src/utils/AppLogger.h"
#include "OrthoViewer.h"
#include "src/qtUtils/TaskRunner.h"

namespace {

constexpr double kKeyboardZoomFactor = 1.25;

int brushWidthForNumberKey(int key) {
    // Index 0 represents key 0; keys 1-9 follow their numeric index.
    static constexpr std::array<int, 10> brushWidths{101, 1, 3, 5, 9, 15, 21, 31, 51, 75};
    if (key < Qt::Key_0 || key > Qt::Key_9) {
        return 0;
    }
    return brushWidths[static_cast<std::size_t>(key - Qt::Key_0)];
}

void drawVoxelBrushStroke(QPainter &painter,
                          const QPoint &start,
                          const QPoint &end,
                          const QColor &color,
                          int brushWidth) {
    if (brushWidth == 1 || brushWidth == 3 || brushWidth == 5) {
        painter.setPen(QPen(color, 1));
        const int radius = brushWidth / 2;
        for (int yOffset = -radius; yOffset <= radius; ++yOffset) {
            for (int xOffset = -radius; xOffset <= radius; ++xOffset) {
                if (std::abs(xOffset) + std::abs(yOffset) > radius) {
                    continue;
                }
                const QPoint offset(xOffset, yOffset);
                if (start == end) {
                    painter.drawPoint(start + offset);
                } else {
                    painter.drawLine(start + offset, end + offset);
                }
            }
        }
        return;
    }

    painter.setPen(QPen(color, brushWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF voxelCenterOffset(0.5, 0.5);
    const QPointF centeredStart = QPointF(start) + voxelCenterOffset;
    const QPointF centeredEnd = QPointF(end) + voxelCenterOffset;
    if (start == end) {
        painter.drawPoint(centeredStart);
    } else {
        painter.drawLine(centeredStart, centeredEnd);
    }
}

struct Prepared3DSplitView {
    std::shared_ptr<segment_puzzler::SeededWatershedSplitSession> session;
    Segment3DViewerDialog::PreparedScene scene;
    Graph::WorkingSegmentResolution workingResolution;
};

bool hasIdentityDirection(dataType::SegmentsImageType::Pointer image, double epsilon = 1e-6) {
    if (image == nullptr) {
        return true;
    }

    const auto direction = image->GetDirection();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const double expected = row == col ? 1.0 : 0.0;
            if (std::abs(direction[row][col] - expected) > epsilon) {
                return false;
            }
        }
    }
    return true;
}

bool haveMatchingImageRegions(dataType::SegmentsImageType::Pointer left,
                              dataType::SegmentsImageType::Pointer right) {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    const auto leftRegion = left->GetLargestPossibleRegion();
    const auto rightRegion = right->GetLargestPossibleRegion();
    return leftRegion.GetIndex() == rightRegion.GetIndex() &&
           leftRegion.GetSize() == rightRegion.GetSize();
}

bool toolWorksWithoutWorkingSegments(SliceViewer::ToolMode tool) {
    return tool == SliceViewer::ToolMode::None ||
           tool == SliceViewer::ToolMode::Ctrl ||
           tool == SliceViewer::ToolMode::Delete ||
           tool == SliceViewer::ToolMode::SelectColor ||
           tool == SliceViewer::ToolMode::Fill ||
           tool == SliceViewer::ToolMode::Open;
}

std::optional<SliceViewer::ToolMode> transientToolModeForKey(int key) {
    switch (key) {
        case Qt::Key_Control:
            return SliceViewer::ToolMode::Ctrl;
        case Qt::Key_S:
            return SliceViewer::ToolMode::Transfer;
        case Qt::Key_P:
            return SliceViewer::ToolMode::Refine;
        case Qt::Key_D:
            return SliceViewer::ToolMode::Delete;
        case Qt::Key_X:
            return SliceViewer::ToolMode::Split;
        case Qt::Key_C:
            return SliceViewer::ToolMode::Cut;
        case Qt::Key_Q:
            return SliceViewer::ToolMode::SelectColor;
        case Qt::Key_F:
            return SliceViewer::ToolMode::Fill;
        case Qt::Key_G:
            return SliceViewer::ToolMode::Open;
        case Qt::Key_J:
            return SliceViewer::ToolMode::Dilate;
        case Qt::Key_K:
            return SliceViewer::ToolMode::Erode;
        case Qt::Key_H:
            return SliceViewer::ToolMode::Insert;
        case Qt::Key_W:
            return SliceViewer::ToolMode::View3DSplit;
        case Qt::Key_M:
            return SliceViewer::ToolMode::View3D;
        default:
            return std::nullopt;
    }
}

bool isOneShotViewerCommandKey(int key) {
    return key == Qt::Key_U
           || key == Qt::Key_N;
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
    SP_LOG_DEBUG_CHANGED("viewer.render", key, message);
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

void navigateOrthoViewerToIndex(OrthoViewer *orthoViewer,
                                const dataType::SegmentsImageType::IndexType &index) {
    if (orthoViewer == nullptr) {
        return;
    }

    if (orthoViewer->xy->isSliceIndexValid(index[2])) orthoViewer->xy->setSliceIndex(index[2]);
    if (orthoViewer->xz->isSliceIndexValid(index[1])) orthoViewer->xz->setSliceIndex(index[1]);
    if (orthoViewer->zy->isSliceIndexValid(index[0])) orthoViewer->zy->setSliceIndex(index[0]);
    orthoViewer->centerViewportsToXYZImageSpace(index[0], index[1], index[2]);
}

void navigateOrthoViewerToLabel(OrthoViewer *orthoViewer,
                                dataType::SegmentsImageType::Pointer segImage,
                                dataType::SegmentIdType labelId) {
    if (orthoViewer == nullptr || segImage == nullptr) {
        return;
    }

    dataType::SegmentsImageType::IndexType index;
    if (!utils::findRepresentativeVoxelForLabel(segImage, labelId, index)) {
        return;
    }

    navigateOrthoViewerToIndex(orthoViewer, index);
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

void AnnotationSliceViewer::setDeleteSelectedSegmentationLabelHandler(
    DeleteSelectedSegmentationLabelHandler handler)
{
    deleteSelectedSegmentationLabelHandler = std::move(handler);
}


AnnotationSliceViewer::AnnotationSliceViewer(std::shared_ptr<GraphBase> graphBaseIn,
                                             TaskRunner *taskRunnerIn,
                                             QWidget *parent,
                                             bool)
    : SliceViewer(graphBaseIn, taskRunnerIn, parent) {
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Constructing AnnotationSliceViewer"));
    }

    paintModeIsActive = false;
    paintBoundaryModeIsActive = false;

    pThresholdedBoundaries = nullptr;

    ROISelectionModeIsActive = false;
    ROISelectionRubberBand = nullptr;

    labelOfClickedSegmentation = 0;

    rightClicked = false;

    scribbling = false;

    this->setMouseTracking(true);
    refreshBrushCursor();

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
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Annotation paintEvent rect=%1,%2 %3x%4")
                         .arg(event->rect().topLeft().x())
                         .arg(event->rect().topLeft().y())
                         .arg(event->rect().width())
                         .arg(event->rect().height()));
    }
    const qint64 startedAtMs = verbose ? QDateTime::currentMSecsSinceEpoch() : 0;

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const QRect targetRect = rect();
    const QRect sourceRect = backGroundImage.rect();

    painter.drawImage(targetRect, backGroundImage, sourceRect);
    drawActiveSignalLayers(painter, targetRect);

    if (!isImageOnlyMode()) {
        painter.drawImage(targetRect, annotationImage, annotationImage.rect());
    }

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

    drawBrushPreview(painter);

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

    if (verbose) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Annotation paintEvent finished in %1 ms")
                         .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
    }

}

void AnnotationSliceViewer::drawBrushPreview(QPainter &painter) const {
    if (!brushPreviewVisible || !rect().contains(brushPreviewPosition)
        || getCurrentSliceWidth() <= 0 || getCurrentSliceHeight() <= 0) {
        return;
    }

    const QPoint slicePoint = slicePixelFromWidgetPoint(brushPreviewPosition);
    const QPointF center = widgetPositionForSlicePixel(slicePoint.x(), slicePoint.y());
    const double scaleX = static_cast<double>(width()) / getCurrentSliceWidth();
    const double scaleY = static_cast<double>(height()) / getCurrentSliceHeight();
    const qreal brushRadiusX = myPenWidth * scaleX / 2.0;
    const qreal brushRadiusY = myPenWidth * scaleY / 2.0;
    constexpr qreal minimumDisplayedBrushRadius = 4.0;
    const qreal largestBrushRadius = std::max(brushRadiusX, brushRadiusY);
    // Enlarge only the preview when it would rasterize as a tiny box. Scaling
    // both axes equally keeps the voxel-spacing aspect ratio intact.
    const qreal previewScale = largestBrushRadius < minimumDisplayedBrushRadius
        ? minimumDisplayedBrushRadius / largestBrushRadius
        : 1.0;
    const qreal displayedBrushRadiusX = brushRadiusX * previewScale;
    const qreal displayedBrushRadiusY = brushRadiusY * previewScale;

    QColor fillColor = cursorColor;
    fillColor.setAlpha(35);
    QColor outlineColor = cursorColor;
    outlineColor.setAlpha(230);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(fillColor);
    painter.setPen(QPen(QColor(0, 0, 0, 190), 2.0));
    painter.drawEllipse(center, displayedBrushRadiusX, displayedBrushRadiusY);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(outlineColor, 1.0));
    painter.drawEllipse(center, displayedBrushRadiusX, displayedBrushRadiusY);

    constexpr qreal targetDotRadius = 2.5;
    if (paintModeIsActive) {
        constexpr qreal paintModeDotRadius = 3.5;
        QConicalGradient paintModeGradient(center, 90.0);
        paintModeGradient.setColorAt(0.0, QColor("#ff304f"));
        paintModeGradient.setColorAt(1.0 / 6.0, QColor("#ffcc33"));
        paintModeGradient.setColorAt(2.0 / 6.0, QColor("#39d353"));
        paintModeGradient.setColorAt(3.0 / 6.0, QColor("#27d7e7"));
        paintModeGradient.setColorAt(4.0 / 6.0, QColor("#3974ff"));
        paintModeGradient.setColorAt(5.0 / 6.0, QColor("#c642f5"));
        paintModeGradient.setColorAt(1.0, QColor("#ff304f"));
        painter.setBrush(paintModeGradient);
        painter.setPen(QPen(QColor(0, 0, 0, 230), 1.0));
        painter.drawEllipse(center, paintModeDotRadius, paintModeDotRadius);
    } else {
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.setPen(QPen(QColor(0, 0, 0, 220), 1.0));
        painter.drawEllipse(center, targetDotRadius, targetDotRadius);
    }
    painter.restore();
}

void AnnotationSliceViewer::refreshBrushCursor() {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    setCursor(Qt::BlankCursor);
    brushPreviewVisible = underMouse();
    if (brushPreviewVisible) {
        brushPreviewPosition = mapFromGlobal(QCursor::pos());
        brushPreviewVisible = rect().contains(brushPreviewPosition);
    }
    update();
}


void AnnotationSliceViewer::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space) {
        if (!event->isAutoRepeat()) {
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setImageOnlyMode(true);
            }
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_A) {
        if (!event->isAutoRepeat()) {
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setOverlayOnlyMode(true);
            }
        }
        event->accept();
        return;
    }
    if (event->isAutoRepeat() && transientToolModeForKey(event->key()).has_value()) {
        event->accept();
        return;
    }
    if (event->isAutoRepeat() && isOneShotViewerCommandKey(event->key())) {
        event->accept();
        return;
    }
    if (taskRunner != nullptr && taskRunner->isBusy()) {
        SP_LOG_WARNING("viewer.interaction", QStringLiteral("Ignoring key press because a background task is still running"));
        return;
    }
//    std::cout << event->key() << std::endl;
    const int requestedBrushWidth = brushWidthForNumberKey(event->key());
    if (requestedBrushWidth > 0) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("brush");
        }
        updatePenWidthInAllViewers(requestedBrushWidth);
    } else if (event->key() == Qt::Key_R) {
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

        for (auto *viewer : linkedViewerList) {
            viewer->recalculateQImages();
        }
        if (paintModeIsActive) {
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->refreshPaintSelectionColor();
            }
        } else if (paintBoundaryModeIsActive && pThresholdedBoundariesSignal != nullptr &&
                   labelOfClickedSegmentation < static_cast<dataType::SegmentIdType>(
                       pThresholdedBoundariesSignal->LUT.size())) {
            const QColor selectedColor = QColor::fromRgb(
                pThresholdedBoundariesSignal->LUT[labelOfClickedSegmentation]);
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setAnnotationSelection(labelOfClickedSegmentation, selectedColor);
            } else {
                setAnnotationSelection(labelOfClickedSegmentation, selectedColor);
            }
        }
    } else if (event->key() == Qt::Key_Plus) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("zoom");
        }
        modifyZoomInAllViewers(kKeyboardZoomFactor);
    } else if (event->key() == Qt::Key_Minus) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("zoom");
        }
        modifyZoomInAllViewers(1.0 / kKeyboardZoomFactor);
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
    } else if (event->key() == Qt::Key_W) {
        if (orthoViewer() != nullptr) {
            orthoViewer()->flashShortcutLegendKey("3dsplit");
        }
        setLinkedToolModeAndNotify(linkedViewerList, ToolMode::View3DSplit);
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
    const QString &progressText,
    int launchSliceAxis)
{
    const auto segImage = active3DViewSegmentsImage();
    if (segImage == nullptr || labels.empty()) {
        return;
    }

    if (taskRunner == nullptr) {
        auto preparedScene = Segment3DViewerDialog::prepareSingleLabelSlideshowScene(
            segImage, labels.front());
        openPrepared3DView(std::move(preparedScene), launchSliceAxis, true);
        return;
    }

    taskRunner->runWithLabel(
        progressText,
        [segImage, label = labels.front()]() {
            return Segment3DViewerDialog::prepareSingleLabelSlideshowScene(segImage, label);
        },
        [this, launchSliceAxis](Segment3DViewerDialog::PreparedScene preparedScene) {
            openPrepared3DView(std::move(preparedScene), launchSliceAxis, true);
        });
}

void AnnotationSliceViewer::requestSingleLabel3D(
    Segment3DViewerDialog *dialog,
    dataType::SegmentsImageType::Pointer segImage,
    dataType::SegmentIdType labelId,
    const Roi &bounds)
{
    if (dialog == nullptr || segImage == nullptr || labelId == 0
        || active3DViewSegmentsImage() != segImage
        || taskRunner == nullptr || taskRunner->isBusy()) {
        if (dialog != nullptr) {
            dialog->rejectPreparedScene(labelId);
        }
        return;
    }

    quint32 color = 0xFFAAAAAA;
    if (const auto *signal = active3DViewSignal(); signal != nullptr
        && labelId < static_cast<dataType::SegmentIdType>(signal->LUT.size())) {
        color = signal->LUT[labelId];
    }

    const QPointer<Segment3DViewerDialog> guardedDialog(dialog);
    const auto committed = std::make_shared<bool>(false);
    taskRunner->runInBackground(
        QStringLiteral("Preparing 3D segment %1...").arg(labelId),
        [segImage, labelId, color, bounds]() {
            return Segment3DViewerDialog::prepareSingleLabelSlideshowScene(
                segImage, {labelId, color}, bounds);
        },
        [guardedDialog, labelId, committed](Segment3DViewerDialog::PreparedScene preparedScene) {
            *committed = true;
            if (guardedDialog == nullptr) {
                return;
            }
            if (!guardedDialog->acceptPreparedScene(std::move(preparedScene))) {
                if (!guardedDialog->rejectPreparedScene(labelId)) {
                    return;
                }
                QMessageBox::information(
                    guardedDialog,
                    QStringLiteral("3D View"),
                    QStringLiteral("No 3D surface could be generated for segment %1.")
                        .arg(labelId));
            }
        },
        [guardedDialog, labelId, committed]() {
            if (!*committed && guardedDialog != nullptr) {
                guardedDialog->rejectPreparedScene(labelId);
            }
        });
}

void AnnotationSliceViewer::openPrepared3DView(Segment3DViewerDialog::PreparedScene preparedScene,
                                               int launchSliceAxis,
                                               bool enableSelectedLabelDeletion)
{
    const bool singleLabelScene = preparedScene.targetLabelId != 0
                                  && preparedScene.meshes.size() == 1;
    auto navigationLabels = std::move(preparedScene.navigationLabels);
    const auto navigationImage = active3DViewSegmentsImage();
    auto *linkedOrthoViewer = orthoViewer();

    auto *dialog = new Segment3DViewerDialog(
        std::move(preparedScene), this, launchSliceAxis);
    dialog->setNavigateToLabelHandler(
        [navigationImage, linkedOrthoViewer](dataType::SegmentIdType labelId) {
            navigateOrthoViewerToLabel(linkedOrthoViewer, navigationImage, labelId);
        });
    Segment3DViewerDialog::DeleteLabelHandler deleteLabel;
    if (enableSelectedLabelDeletion
        && graphBase != nullptr
        && navigationImage == graphBase->pSelectedSegmentation
        && deleteSelectedSegmentationLabelHandler) {
        deleteLabel = [this, navigationImage](dataType::SegmentIdType labelId) {
            if (labelId == 0 || !deleteSelectedSegmentationLabelHandler) {
                return false;
            }
            return deleteSelectedSegmentationLabelHandler(navigationImage, labelId);
        };
    }
    if (singleLabelScene && !navigationLabels.empty()) {
        const QPointer<Segment3DViewerDialog> guardedDialog(dialog);
        Segment3DViewerDialog::SingleLabelSessionConfig session;
        session.labels = std::move(navigationLabels);
        session.requestLabel =
            [this, guardedDialog, navigationImage](dataType::SegmentIdType labelId,
                                                   const Roi &bounds) {
                if (guardedDialog != nullptr) {
                    requestSingleLabel3D(
                        guardedDialog.data(), navigationImage, labelId, bounds);
                }
            };
        session.deleteLabel = std::move(deleteLabel);
        dialog->setSingleLabelSession(std::move(session));
    } else {
        dialog->setDeleteLabelHandler(std::move(deleteLabel));
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->presentInFront();
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

    showPrepared3DView(
        {{label, lutColor}},
        "Preparing 3D segment view...",
        sliceAxis);
}

void AnnotationSliceViewer::refreshWorkingGraphPresentationAfterInsertion(
    dataType::SegmentIdType workingLabel)
{
    if (graphBase != nullptr && graphBase->pWorkingSegments != nullptr) {
        graphBase->pWorkingSegments->checkAndResizeLUT(workingLabel);
    }
    if (graphBase != nullptr && graphBase->pEdgesInitialSegmentsITKSignal != nullptr) {
        graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    }
    if (orthoViewer() != nullptr) {
        orthoViewer()->refreshViewers();
    }
}

bool AnnotationSliceViewer::handleWorkingSegmentResolution(
    const Graph::WorkingSegmentResolution &resolution)
{
    using Status = Graph::WorkingSegmentResolution::Status;
    switch (resolution.status) {
    case Status::ReusedExisting:
        return true;
    case Status::Inserted:
        refreshWorkingGraphPresentationAfterInsertion(resolution.workingLabel);
        return true;
    case Status::NeedsInsertion:
        sendStatusMessage(QStringLiteral("The selected segment must be inserted into the working graph first."));
        return false;
    case Status::NoForeground:
        sendStatusMessage(QStringLiteral("3D split: clicked background in the selected segmentation."));
        return false;
    case Status::Failed:
        sendStatusMessage(QStringLiteral("Could not resolve the selected segment in the working graph."));
        return false;
    }

    sendStatusMessage(QStringLiteral("Could not resolve the selected segment in the working graph."));
    return false;
}

bool AnnotationSliceViewer::show3DSplitView(int posX, int posY) {
    if (graphBase == nullptr || graphBase->pGraph == nullptr
        || graphBase->pWorkingSegmentsImage == nullptr
        || graphBase->pSelectedSegmentation == nullptr) {
        sendStatusMessage(QStringLiteral(
            "Load working segments and select a segmentation before using 3D split."));
        return false;
    }
    const auto selectedImage = graphBase->pSelectedSegmentation;
    if (!haveMatchingImageRegions(selectedImage, graphBase->pWorkingSegmentsImage)) {
        sendStatusMessage(QStringLiteral(
            "The selected segmentation and working segments must have matching image regions."));
        return false;
    }
    if (!hasIdentityDirection(selectedImage)) {
        QMessageBox::information(
            this,
            tr("3D Split"),
            tr("3D split currently requires an image with identity direction."));
        return false;
    }

    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    dataType::SegmentsImageType::IndexType selectedIndex{{x, y, z}};
    if (!selectedImage->GetLargestPossibleRegion().IsInside(selectedIndex)) {
        return false;
    }
    const auto selectedLabel = selectedImage->GetPixel(selectedIndex);
    if (selectedLabel == graphBase->pGraph->backgroundId) {
        sendStatusMessage(QStringLiteral("3D split: clicked background."));
        return false;
    }

    quint32 color = 0xFFAAAAAA;
    if (graphBase->pSelectedSegmentationSignal != nullptr
        && selectedLabel < static_cast<dataType::SegmentIdType>(
                               graphBase->pSelectedSegmentationSignal->LUT.size())) {
        color = graphBase->pSelectedSegmentationSignal->LUT[selectedLabel];
    }
    Graph *const graph = graphBase->pGraph;
    const int launchSliceAxis = sliceAxis;
    const auto prepare = [selectedImage, selectedLabel, color, graph, x, y, z]() {
        Prepared3DSplitView prepared;
        prepared.session = std::make_shared<segment_puzzler::SeededWatershedSplitSession>(
            segment_puzzler::prepareSeededWatershedSplit(selectedImage, selectedLabel));
        prepared.scene = Segment3DViewerDialog::prepareScene(
            selectedImage, {{selectedLabel, color}}, prepared.session->sourceRoi);
        prepared.scene.windowTitle = QStringLiteral("3D Split - Segment %1")
                                         .arg(selectedLabel);
        prepared.workingResolution =
            graph->inspectSelectedSegmentationComponentInWorkingGraph(x, y, z);
        return prepared;
    };

    const auto openPrepared =
        [this, selectedImage, selectedLabel, graph, x, y, z, launchSliceAxis](
            Prepared3DSplitView prepared) {
            if (prepared.session == nullptr || prepared.scene.meshes.empty()) {
                QMessageBox::information(
                    this, tr("3D Split"), tr("No 3D surface could be generated."));
                return;
            }
            if (prepared.session->connectedComponentCount != 1) {
                QMessageBox::information(
                    this,
                    tr("3D Split"),
                    tr("The selected segment is disconnected (%1 regions were found).")
                        .arg(prepared.session->connectedComponentCount));
                return;
            }

            SP_LOG_INFO(
                "segmentation",
                QStringLiteral(
                    "operation=seeded_watershed_split phase=prepared label=%1 voxels=%2 "
                    "components=%3 roi=[%4,%5,%6,%7,%8,%9] "
                    "global_offset=[%10,%11,%12] mask_hash=0x%13 "
                    "maximum_distance=%14 mask_ms=%15 distance_ms=%16")
                    .arg(selectedLabel)
                    .arg(prepared.session->voxelCount)
                    .arg(prepared.session->connectedComponentCount)
                    .arg(prepared.session->sourceRoi.minX)
                    .arg(prepared.session->sourceRoi.minY)
                    .arg(prepared.session->sourceRoi.minZ)
                    .arg(prepared.session->sourceRoi.maxX)
                    .arg(prepared.session->sourceRoi.maxY)
                    .arg(prepared.session->sourceRoi.maxZ)
                    .arg(prepared.session->globalOffset[0])
                    .arg(prepared.session->globalOffset[1])
                    .arg(prepared.session->globalOffset[2])
                    .arg(QString::number(
                        static_cast<qulonglong>(prepared.session->maskHash), 16)
                             .rightJustified(16, QLatin1Char('0')))
                    .arg(prepared.session->maximumDistance, 0, 'g', 9)
                    .arg(prepared.session->maskAndConnectivityMs, 0, 'f', 1)
                    .arg(prepared.session->distanceTransformMs, 0, 'f', 1));

            using Status = Graph::WorkingSegmentResolution::Status;
            bool allowInsertion = false;
            if (prepared.workingResolution.status == Status::NeedsInsertion) {
                const auto answer = QMessageBox::question(
                    this,
                    tr("Insert Segment?"),
                    tr("The selected segment does not exactly match a working segment. "
                       "Insert it into the working graph if the split is applied?"),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (answer != QMessageBox::Yes) {
                    return;
                }
                allowInsertion = true;
            } else if (prepared.workingResolution.status != Status::ReusedExisting) {
                handleWorkingSegmentResolution(prepared.workingResolution);
                return;
            }

            const auto session = prepared.session;
            Segment3DViewerDialog::SplitSessionConfig splitSession;
            splitSession.taskRunner = taskRunner;
            splitSession.session = session;
            splitSession.progressText = QStringLiteral(
                "Applying seeded split and transferring results...");
            splitSession.projectedCutProgressText = QStringLiteral(
                "Applying projected cut and transferring results...");
            const auto applyPartition =
                [this, selectedImage, selectedLabel, graph, session,
                 x, y, z, allowInsertion](
                    dataType::SegmentsImageType::Pointer partition) {
                    Segment3DViewerDialog::SplitApplyResult result;
                    if (graphBase == nullptr || graphBase->pGraph != graph
                        || graphBase->pSelectedSegmentation != selectedImage
                        || selectedImage->GetMTime() != session->sourceModifiedTime
                        || selectedImage->GetPixel({x, y, z}) != selectedLabel) {
                        result.message = QStringLiteral(
                            "The selected segmentation changed after the viewer was opened.");
                        return result;
                    }

                    auto resolution =
                        graph->inspectSelectedSegmentationComponentInWorkingGraph(x, y, z);
                    if (resolution.status == Status::NeedsInsertion && allowInsertion) {
                        resolution = graph->ensureSelectedSegmentationComponentInWorkingGraph(
                            x, y, z);
                    }
                    if (resolution.status != Status::ReusedExisting
                        && resolution.status != Status::Inserted) {
                        result.message = resolution.status == Status::NeedsInsertion
                                             ? QStringLiteral(
                                                   "The segment now requires insertion, but it was not approved.")
                                             : QStringLiteral(
                                                   "The segment could not be resolved in the working graph.");
                        return result;
                    }

                    std::vector<dataType::SegmentIdType> workingLabels;
                    if (!graph->splitWorkingNodeByVoxelPartition(
                            resolution.workingLabel,
                            partition,
                            session->globalOffset,
                            &workingLabels)) {
                        result.message = QStringLiteral(
                            "The preview partition no longer matches the working segment.");
                        return result;
                    }
                    const auto selectedLabels =
                        graph->transferWorkingNodesToSegmentation(workingLabels);
                    result.mutated = true;
                    if (selectedLabels.size() != workingLabels.size()) {
                        result.message = QStringLiteral(
                            "The working segment was split, but not all parts could be transferred.");
                        return result;
                    }
                    selectedImage->Modified();
                    return result;
                };
            splitSession.applySplit =
                [selectedLabel, session, applyPartition](
                    const segment_puzzler::SeededWatershedSplitResult &split) {
                    auto result = applyPartition(split.partition);
                    SP_LOG_INFO(
                        "segmentation",
                        QStringLiteral(
                            "operation=seeded_watershed_split status=split source_label=%1 "
                            "source_voxels=%2 part_voxels=%3,%4 part_components=%5,%6 "
                            "allow_disconnected_parts=%7 mutated=%8")
                            .arg(selectedLabel)
                            .arg(session->voxelCount)
                            .arg(split.voxelCounts[0])
                            .arg(split.voxelCounts[1])
                            .arg(split.connectedComponentCounts[0])
                            .arg(split.connectedComponentCounts[1])
                            .arg(split.disconnectedPartsAllowed)
                            .arg(result.mutated));
                    return result;
                };
            splitSession.applyProjectedCut =
                [selectedLabel, applyPartition](const Projected3DCutResult &cut) {
                    auto result = applyPartition(cut.partition);
                    SP_LOG_INFO(
                        "segmentation",
                        QStringLiteral(
                            "operation=projected_3d_cut status=split source_label=%1 "
                            "source_voxels=%2 cut_voxels=%3 parts=%4 compute_ms=%5 "
                            "mutated=%6")
                            .arg(selectedLabel)
                            .arg(cut.profile.targetVoxelCount)
                            .arg(cut.profile.provisionalCutVoxelCount)
                            .arg(cut.componentVoxelCounts.size())
                            .arg(cut.profile.totalMs, 0, 'f', 1)
                            .arg(result.mutated));
                    return result;
                };

            auto *dialog = new Segment3DViewerDialog(
                std::move(prepared.scene), std::move(splitSession), this, launchSliceAxis);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(dialog, &QDialog::finished, this, [this](int result) {
                if (result != QDialog::Accepted) {
                    return;
                }
                if (graphBase != nullptr
                    && graphBase->pEdgesInitialSegmentsITKSignal != nullptr) {
                    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
                }
                if (orthoViewer() != nullptr) {
                    orthoViewer()->refreshViewers();
                }
            });
            dialog->presentInFront();
        };

    SP_LOG_INFO(
        "segmentation",
        QStringLiteral("operation=seeded_watershed_split phase=prepare label=%1")
            .arg(selectedLabel));
    if (taskRunner != nullptr) {
        taskRunner->runWithLabel(
            QStringLiteral("Preparing 3D split tools..."), prepare, openPrepared);
    } else {
        openPrepared(prepare());
    }
    return true;
}

void AnnotationSliceViewer::show3DAllLabelsView() {
    const auto segImage = active3DViewSegmentsImage();
    if (segImage == nullptr) {
        return;
    }

    std::vector<quint32> labelColors;
    if (const auto *activeSignal = active3DViewSignal(); activeSignal != nullptr) {
        labelColors = activeSignal->LUT;
    }

    if (taskRunner == nullptr) {
        auto preparedScene =
            Segment3DViewerDialog::prepareAllLabelsScene(segImage, std::move(labelColors));
        if (!preparedScene.meshes.empty()) {
            openPrepared3DView(std::move(preparedScene), 2, true);
        }
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Preparing 3D view for all segments..."),
        [segImage, labelColors = std::move(labelColors)]() mutable {
            return Segment3DViewerDialog::prepareAllLabelsScene(
                segImage, std::move(labelColors));
        },
        [this](Segment3DViewerDialog::PreparedScene preparedScene) {
            if (!preparedScene.meshes.empty()) {
                openPrepared3DView(std::move(preparedScene), 2, true);
            }
        });
}

void AnnotationSliceViewer::keyReleaseEvent(QKeyEvent *event) {

    if (event->key() == Qt::Key_Space) {
        if (!event->isAutoRepeat()) {
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setImageOnlyMode(false);
            }
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_A) {
        if (!event->isAutoRepeat()) {
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setOverlayOnlyMode(false);
            }
        }
        event->accept();
        return;
    }

//    std::cout << "Release: " << event->key() << "\n";
    const auto releasedToolMode = transientToolModeForKey(event->key());
    if (releasedToolMode.has_value()) {
        if (event->isAutoRepeat()) {
            event->accept();
            return;
        }
        clearMatchingLinkedToolMode(linkedViewerList, releasedToolMode.value());
        notifyOrthoViewerInteractionModeChanged();
        event->accept();
    }
}

void AnnotationSliceViewer::mousePressEvent(QMouseEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()) {
        return;
    }

    brushPreviewPosition = event->pos();
    brushPreviewVisible = rect().contains(brushPreviewPosition);

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
            lastPoint = slicePixelFromWidgetPoint(event->pos());
            scribbling = true;
            rightClicked = false;
            drawPoint(event->pos());
        } else if (event->button() == Qt::RightButton) {
            lastPoint = slicePixelFromWidgetPoint(event->pos());
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
            orthoViewer()->centerViewportOnSlicePixel(orthoViewer()->scrollAreaXY, orthoViewer()->xy, x, y);
            orthoViewer()->centerViewportOnSlicePixel(orthoViewer()->scrollAreaXZ, orthoViewer()->xz, x, z);
        } else if (sliceAxis == 1) {
            orthoViewer()->centerViewportOnSlicePixel(orthoViewer()->scrollAreaXY, orthoViewer()->xy, x, y);
            orthoViewer()->centerViewportOnSlicePixel(orthoViewer()->scrollAreaZY, orthoViewer()->zy, z, y);
        } else if (sliceAxis == 2) {
            orthoViewer()->centerViewportOnSlicePixel(orthoViewer()->scrollAreaXZ, orthoViewer()->xz, x, z);
            orthoViewer()->centerViewportOnSlicePixel(orthoViewer()->scrollAreaZY, orthoViewer()->zy, z, y);
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
    case ToolMode::View3DSplit:
        show3DSplitView(event->pos().x(), event->pos().y());
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

    SP_LOG_INFO("segmentation", QStringLiteral("Transferring a segmentation segment into the working supervoxel graph"));
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    SP_LOG_INFO("segmentation", QStringLiteral("Insert segmentation segment at %1,%2,%3").arg(x).arg(y).arg(z));

    if (taskRunner == nullptr) {
        const auto insertedLabel = graphBase->pGraph->transferSegmentationSegmentToInitialSegment(x, y, z);
        if (insertedLabel.has_value()) {
            refreshWorkingGraphPresentationAfterInsertion(insertedLabel.value());
        }
        return;
    }

    taskRunner->runWithLabel(
        QStringLiteral("Inserting selected segment into working graph..."),
        [this, x, y, z]() { return graphBase->pGraph->transferSegmentationSegmentToInitialSegment(x, y, z); },
        [this](std::optional<dataType::SegmentIdType> insertedLabel) {
            if (insertedLabel.has_value()) {
                refreshWorkingGraphPresentationAfterInsertion(insertedLabel.value());
            }
        });
}

void AnnotationSliceViewer::runOpenSegmentationLabel(int posX, int posY){
    SP_LOG_INFO("segmentation", QStringLiteral("Opening the clicked segmentation label"));
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
void AnnotationSliceViewer::openSegmentationLabel(int posX, int posY) {
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (graphBase->pSelectedSegmentation == nullptr) {
        return;
    }
    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;

    const qint64 firstPartStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    SP_LOG_DEBUG("viewer.interaction",
                 QStringLiteral("Opening segmentation label at (%1, %2, %3)")
                     .arg(x)
                     .arg(y)
                     .arg(z));


    // calculate labelmap
    // get roi
    // create new empty image
    // fill in 1:1 segment with floodfill
    // open
    // put back in

    dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
    SP_LOG_DEBUG("segmentation",
                 QStringLiteral("Open segmentation requested for label %1")
                     .arg(labelAtClickPosition));

    if (labelAtClickPosition == backgroundLabel) {
        SP_LOG_WARNING("segmentation",
                       QStringLiteral("Skipping segmentation open because clicked label matches background (%1)")
                           .arg(backgroundLabel));
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
    SP_LOG_DEBUG("segmentation",
                 QStringLiteral("Prepared open ROI for label %1 in %2 ms")
                     .arg(labelAtClickPosition)
                     .arg(QDateTime::currentMSecsSinceEpoch() - firstPartStartedAtMs));

    const qint64 secondPartStartedAtMs = QDateTime::currentMSecsSinceEpoch();
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

    SP_LOG_DEBUG("segmentation",
                 QStringLiteral("Applied morphological opening for label %1 in %2 ms")
                     .arg(labelAtClickPosition)
                     .arg(QDateTime::currentMSecsSinceEpoch() - secondPartStartedAtMs));
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

    SP_LOG_INFO("segmentation",
                QStringLiteral("Opened segmentation label %1 in %2 ms")
                    .arg(labelAtClickPosition)
                    .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
}



void AnnotationSliceViewer::fillSegmentationLabel(int posX, int posY) {
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (graphBase->pSelectedSegmentation == nullptr) {
        return;
    }
    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;

    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    SP_LOG_DEBUG("viewer.interaction",
                 QStringLiteral("Filling segmentation label at (%1, %2, %3)")
                     .arg(x)
                     .arg(y)
                     .arg(z));

    // calculate labelmap

    // get roi

    // create new empty image

    // fill in 1:1 segment with floodfill

    // close

    // put back in
    dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
    SP_LOG_DEBUG("segmentation",
                 QStringLiteral("Fill segmentation requested for label %1")
                     .arg(labelAtClickPosition));

    if (labelAtClickPosition == backgroundLabel) {
        SP_LOG_WARNING("segmentation",
                       QStringLiteral("Skipping segmentation fill because clicked label matches background (%1)")
                           .arg(backgroundLabel));
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
    SP_LOG_DEBUG("segmentation",
                 QStringLiteral("Applied morphological closing for label %1 (safeBorder=%2)")
                     .arg(labelAtClickPosition)
                     .arg(closingFilter->GetSafeBorder()));

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

    SP_LOG_INFO("segmentation",
                QStringLiteral("Filled segmentation label %1 in %2 ms")
                    .arg(labelAtClickPosition)
                    .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
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
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    SP_LOG_INFO("segmentation",
                QStringLiteral("Refining segmentation at (%1, %2, %3)")
                    .arg(x)
                    .arg(y)
                    .arg(z));

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
    SP_LOG_INFO("segmentation",
                QStringLiteral("Splitting working segment into initial nodes at (%1, %2, %3)")
                    .arg(x)
                    .arg(y)
                    .arg(z));
    graphBase->pGraph->splitWorkingNodeIntoInitialNodes(x, y, z);
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    orthoViewer()->refreshViewers();
}

void AnnotationSliceViewer::removeInitialSegmentFromWorkingSegmentAtClick(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    SP_LOG_INFO("segmentation",
                QStringLiteral("Removing initial segment from working segment at (%1, %2, %3)")
                    .arg(x)
                    .arg(y)
                    .arg(z));
    graphBase->pGraph->removeInitialNodeFromWorkingNodeAtPosition(x, y, z);

    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    orthoViewer()->refreshViewers();
}

void AnnotationSliceViewer::transferWorkingNodeToSegmentation(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    SP_LOG_INFO("segmentation",
                QStringLiteral("Transferring working node to segmentation at (%1, %2, %3)")
                    .arg(x)
                    .arg(y)
                    .arg(z));
    graphBase->pGraph->transferWorkingNodeToSegmentation(x, y, z);
}


void AnnotationSliceViewer::deleteConnectedLabelFromSegmentation(int posX, int posY) {
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (graphBase->pSelectedSegmentation != nullptr
        && deleteSelectedSegmentationLabelHandler) {
        int x, y, z;
        getXYZfromPixmapPos(posX, posY, x, y, z);
        const dataType::SegmentIdType labelAtPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
        SP_LOG_INFO("segmentation",
                    QStringLiteral("Deleting connected segmentation label %1 at (%2, %3, %4)")
                        .arg(labelAtPosition)
                        .arg(x)
                        .arg(y)
                        .arg(z));
        deleteSelectedSegmentationLabelHandler(
            graphBase->pSelectedSegmentation, labelAtPosition);
    }
    SP_LOG_DEBUG("segmentation",
                 QStringLiteral("Finished deleting connected segmentation label in %1 ms")
                     .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
}



void AnnotationSliceViewer::mouseMoveEvent(QMouseEvent *event) {
    brushPreviewPosition = event->pos();
    brushPreviewVisible = rect().contains(brushPreviewPosition);

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

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void AnnotationSliceViewer::enterEvent(QEnterEvent *event) {
#else
void AnnotationSliceViewer::enterEvent(QEvent *event) {
#endif
    SliceViewer::enterEvent(event);
    brushPreviewPosition = mapFromGlobal(QCursor::pos());
    brushPreviewVisible = rect().contains(brushPreviewPosition);
    update();
}

void AnnotationSliceViewer::leaveEvent(QEvent *event) {
    SliceViewer::leaveEvent(event);
    brushPreviewVisible = false;
    update();
}

void AnnotationSliceViewer::mouseReleaseEvent(QMouseEvent *event) {
    if (scribbling && taskRunner != nullptr && taskRunner->isBusy()) {
        scribbling = false;
        annotationImage.fill(QColor(0, 0, 0, 0));
        update();
        return;
    }

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
            SP_LOG_DEBUG("viewer.interaction",
                         QStringLiteral("ROI rubber band x=%1 y=%2 width=%3 height=%4")
                             .arg(ROISelectionRubberBand->x())
                             .arg(ROISelectionRubberBand->y())
                             .arg(ROISelectionRubberBand->width())
                             .arg(ROISelectionRubberBand->height()));
            const QRect sliceBounds = slicePixelBoundsFromWidgetRect(ROISelectionRubberBand->geometry());
            if (!sliceBounds.isValid()) {
                return;
            }
            if (sliceAxis == 0) {
                graphBase->ROI_fz = sliceBounds.left();
                graphBase->ROI_tz = sliceBounds.right();
                graphBase->ROI_fy = sliceBounds.top();
                graphBase->ROI_ty = sliceBounds.bottom();
                graphBase->ROI_fx = 0;
                graphBase->ROI_tx = graphBase->pWorkingSegments->getDimX() - 1;
            } else if (sliceAxis == 1) {
                graphBase->ROI_fx = sliceBounds.left();
                graphBase->ROI_tx = sliceBounds.right();
                graphBase->ROI_fz = sliceBounds.top();
                graphBase->ROI_tz = sliceBounds.bottom();
                graphBase->ROI_fy = 0;
                graphBase->ROI_ty = graphBase->pWorkingSegments->getDimY() - 1;
            } else if (sliceAxis == 2) {
                graphBase->ROI_fx = sliceBounds.left();
                graphBase->ROI_tx = sliceBounds.right();
                graphBase->ROI_fy = sliceBounds.top();
                graphBase->ROI_ty = sliceBounds.bottom();
                graphBase->ROI_fz = 0;
                graphBase->ROI_tz = graphBase->pWorkingSegments->getDimZ() - 1;
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

    point = slicePixelFromWidgetPoint(point);
    SP_LOG_DEBUG("viewer.interaction",
                 QStringLiteral("Drawing point at (%1, %2)")
                     .arg(point.x())
                     .arg(point.y()));

    if ((point.x() < 0) | (point.y() < 0) | (point.x() >= annotationImage.width()) | (point.y() >= annotationImage.height())) {
        return;
    }


    QPainter painter(&annotationImage);
    if (paintModeIsActive | paintBoundaryModeIsActive) {
        myPenColor = rightClicked ? Qt::black : cursorColor;
    } else {
        myPenColor = rightClicked ? Qt::red : Qt::green;
    }
    drawVoxelBrushStroke(painter, point, point, myPenColor, myPenWidth);

    // while the point is drawn on the annotationimage as normal, update() works on the scaled picture!
    const QRect sourceUpdateRect = QRect(point, QSize(1, 1))
        .adjusted(-myPenWidth, -myPenWidth, myPenWidth, myPenWidth);
    update(widgetRectForSlicePixelBounds(sourceUpdateRect));
//    update();
}

void AnnotationSliceViewer::drawLineTo(QPoint endPoint) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());


    endPoint = slicePixelFromWidgetPoint(endPoint);
    SP_LOG_DEBUG("viewer.interaction",
                 QStringLiteral("Drawing line to (%1, %2)")
                     .arg(endPoint.x())
                     .arg(endPoint.y()));

    if ((endPoint.x() < 0) | (endPoint.y() < 0) | (endPoint.x() >= annotationImage.width()) | (endPoint.y() >= annotationImage.height())) {
        SP_LOG_WARNING("viewer.interaction", QStringLiteral("Ignoring drawLineTo point outside image bounds"));
        return;
    }

    QPainter painter(&annotationImage);
    if (paintModeIsActive | paintBoundaryModeIsActive) {
        myPenColor = rightClicked ? Qt::black : cursorColor;
    } else {
        myPenColor = rightClicked ? Qt::red : Qt::green;
    }

    drawVoxelBrushStroke(painter, lastPoint, endPoint, myPenColor, myPenWidth);

    int rad = (myPenWidth / 2) + 2;
    const QRect sourceUpdateRect = QRect(lastPoint, endPoint).normalized()
        .adjusted(-rad, -rad, +rad, +rad);
    update(widgetRectForSlicePixelBounds(sourceUpdateRect));
    lastPoint = endPoint;
}


void AnnotationSliceViewer::updatePenWidthInAllViewers(int newPenWidth) {
    orthoViewer()->xy->setPenWidth(newPenWidth);
    orthoViewer()->zy->setPenWidth(newPenWidth);
    orthoViewer()->xz->setPenWidth(newPenWidth);
}


void AnnotationSliceViewer::setPenWidth(int newPenWidth) {
    myPenWidth = newPenWidth;
    refreshBrushCursor();
}


void AnnotationSliceViewer::processAnnotationImage(const QImage &image) {
    const bool canEditEdges = graphBase->pEdgesInitialSegmentsImage != nullptr;
    const bool canEditBoundaries = pThresholdedBoundaries != nullptr;
    const bool canEditSegmentation = graphBase->pSelectedSegmentation != nullptr;
    const dataType::SegmentIdType backgroundLabel = graphBase->pGraph->backgroundId;
    if (canEditEdges || canEditBoundaries || canEditSegmentation) {
        //TODO: Separate function into smaller parts, make sliceblabla general

        Q_ASSERT(image.format() == QImage::Format_RGBA8888);
        constexpr int bytesPerPixel = 4;

        if (!paintModeIsActive && !paintBoundaryModeIsActive) { // edge merging/unmerging modus
            if (canEditEdges) {
                std::set<unsigned int> annotatedEdgeNumIdsToMerge;
                std::set<unsigned int> annotatedEdgeNumIdsToUnmerge;

                for (int y = 0; y < image.height(); ++y) {
                    const auto *scanLine = image.constScanLine(y);
                    for (int x = 0; x < image.width(); ++x) {
                        const int pixelOffset = x * bytesPerPixel;
                        int worldX, worldY, worldZ;
                        if (scanLine[pixelOffset] == 255) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            int edgeNumId = graphBase->pEdgesInitialSegmentsImage->GetPixel({worldX, worldY, worldZ});;
                            if (edgeNumId != 0) {
                                //std::cout << "Unmerge: EdgeNumId: " << edgeNumId << " at position: " << worldX << " " << worldY << " " << worldZ << "\n";
                                annotatedEdgeNumIdsToUnmerge.insert(edgeNumId);
                            }
                        } else if (scanLine[pixelOffset + 1] == 255) {
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
            for (int y = 0; y < image.height(); ++y) {
                const auto *scanLine = image.constScanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    if (scanLine[x * bytesPerPixel + 3] == 0) {
                        continue;
                    }

                    int worldX, worldY, worldZ;
                    getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                    if (paintModeIsActive && canEditSegmentation) {
                        if (rightClicked) {
                            graphBase->pSelectedSegmentation->SetPixel(
                                {worldX, worldY, worldZ}, backgroundLabel);
                        } else if (labelOfClickedSegmentation != backgroundLabel) {
                            graphBase->pSelectedSegmentation->SetPixel(
                                {worldX, worldY, worldZ}, labelOfClickedSegmentation);
                        }
                    } else if (canEditBoundaries) {
                        pThresholdedBoundaries->SetPixel(
                            {worldX, worldY, worldZ},
                            rightClicked ? 0 : labelOfClickedSegmentation);
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
        SP_LOG_WARNING("viewer.render", QStringLiteral("Annotation resetQImages called with invalid dimensions"));
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
    if (!paintModeIsActive && !paintBoundaryModeIsActive) {
        cursorColor = Qt::white;
    }
    refreshBrushCursor();
    notifyOrthoViewerInteractionModeChanged();
}

void AnnotationSliceViewer::togglePaintBoundaryMode() {
    paintBoundaryModeIsActive = !paintBoundaryModeIsActive;
    refreshBrushCursor();
    notifyOrthoViewerInteractionModeChanged();
}

void AnnotationSliceViewer::setAnnotationSelection(dataType::SegmentIdType label,
                                                   const QColor &color) {
    labelOfClickedSegmentation = label;
    cursorColor = color;
    refreshBrushCursor();
}

void AnnotationSliceViewer::getSegmentationLabelIdAtCursor(int x, int y) {
    if (paintModeIsActive) {
        if (graphBase->pSelectedSegmentation != nullptr && graphBase->pSelectedSegmentationSignal != nullptr) {
            int xWorld, yWorld, zWorld;
            getXYZfromPixmapPos(x, y, xWorld, yWorld, zWorld);
            const auto selectedLabel = graphBase->pSelectedSegmentation->GetPixel({xWorld, yWorld, zWorld});
            if (selectedLabel >=
                static_cast<dataType::SegmentIdType>(graphBase->pSelectedSegmentationSignal->LUT.size())) {
                return;
            }
            const QColor selectedColor = QColor::fromRgb(
                graphBase->pSelectedSegmentationSignal->LUT[selectedLabel]);
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setAnnotationSelection(selectedLabel, selectedColor);
            } else {
                setAnnotationSelection(selectedLabel, selectedColor);
            }
        }
    } else if (paintBoundaryModeIsActive){
        if(pThresholdedBoundaries != nullptr && pThresholdedBoundariesSignal != nullptr) {
            int xWorld, yWorld, zWorld;
            getXYZfromPixmapPos(x, y, xWorld, yWorld, zWorld);
            const auto selectedLabel = pThresholdedBoundaries->GetPixel({xWorld, yWorld, zWorld});
            if (selectedLabel >=
                static_cast<dataType::SegmentIdType>(pThresholdedBoundariesSignal->LUT.size())) {
                return;
            }
            const QColor selectedColor = QColor::fromRgb(
                pThresholdedBoundariesSignal->LUT[selectedLabel]);
            if (auto *viewer = orthoViewer(); viewer != nullptr) {
                viewer->setAnnotationSelection(selectedLabel, selectedColor);
            } else {
                setAnnotationSelection(selectedLabel, selectedColor);
            }
        }
    }
}

void AnnotationSliceViewer::setVoxelSpacing(const voxel_geometry::VoxelSpacing &spacing) {
    SliceViewer::setVoxelSpacing(spacing);
    if (ROISelectionRubberBand != nullptr) {
        ROISelectionRubberBand->hide();
    }
}

AnnotationSliceViewer::~AnnotationSliceViewer() {
    if (ROISelectionRubberBand != nullptr) {
        delete ROISelectionRubberBand;
    }

}
