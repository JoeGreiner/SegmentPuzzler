#include "OrthoViewer.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "ROIExtractionSliceViewer.h"
#include "src/qtUtils/TaskRunner.h"
#include <Qt>
#include <QtWidgets>
#include "src/segment_handling/graphBase.h"
#include <QDebug>
#include <QHash>
#include <QPainter>
#include "SliceViewer.h"
#include "SliceViewerCoordinateMapping.h"
#include "SliceViewerITKSignal.h"
#include "SliceViewerZoomPolicy.h"
#include "src/utils/AppLogger.h"
#include "src/utils/utils.h"
#include <itkImageRegionConstIteratorWithIndex.h>
#include <QDir>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <mutex>

namespace {

constexpr int kSliceIndicatorLineAlpha = 96;
constexpr double kSliceIndicatorDisplayPenWidth = 1.0;
constexpr double kSliceIndicatorCrossingGapSize = 12.0;

QString planeNameForSliceAxis(int sliceAxis) {
    switch (sliceAxis) {
        case 0:
            return "YZ";
        case 1:
            return "XZ";
        case 2:
            return "XY";
        default:
            return "Unknown";
    }
}

void logSliceViewerState(const QString &key, const QString &message) {
    SP_LOG_DEBUG_CHANGED("viewer.render", key, message);
}

int boundedWidgetExtent(double extent) {
    if (!std::isfinite(extent) || extent <= 1.0) {
        return 1;
    }
    return static_cast<int>(std::lround(std::min(extent, static_cast<double>(QWIDGETSIZE_MAX))));
}

QColor sliceIndicatorColor(int sliceAxis) {
    switch (sliceAxis) {
        case 0: return {255, 255, 0, kSliceIndicatorLineAlpha};
        case 1: return {0, 255, 0, kSliceIndicatorLineAlpha};
        case 2: return {255, 0, 0, kSliceIndicatorLineAlpha};
        default: throw std::out_of_range("Slice axis must be 0, 1, or 2");
    }
}

QString summarizeActiveSignalImageRects(const std::vector<SliceViewerITKSignal *> &signalList) {
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

bool isLabelLayer(const SliceViewerITKSignal *sliceSignal) {
    if (sliceSignal == nullptr || sliceSignal->getSignal() == nullptr) {
        return false;
    }
    const itkSignalBase *signal = sliceSignal->getSignal();
    return signal->usesCategoricalColors() || signal->usesEdgeStatusColors();
}

} // namespace


SliceViewer::SliceViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent, bool verbose)
    : SliceViewer(graphBaseIn, nullptr, parent, verbose) {
}

SliceViewer::SliceViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn, QWidget *parent, bool verbose)
    : verbose{verbose} {
    setParent(parent);
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Constructing SliceViewer"));
    }
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    setFocusPolicy(Qt::WheelFocus);

    graphBase = graphBaseIn;
    taskRunner = taskRunnerIn;

    linkedSliderSet = false;
    imageOnlyMode = false;
    overlayOnlyMode = false;

    dimX = 1;
    dimY = 1;
    dimZ = 1;

    numberSignals = 0;

    indexHorizontalIndicator = 0;
    indexVerticalIndicator = 0;
    lastMouseX = 0;
    lastMouseY = 0;
    lastMouseZ = 0;

    // set default values
    sliceIndex = 0;
    predictedSliceIndex = 0;
    sliceAxis = 2;

    zoomFactor = 1;
    linkedOrthoViewer = nullptr;

    //setup custom cursor
    cursorColor = Qt::white;
    myPenWidth = 3;
    myPenColor = Qt::red;
    refreshBrushCursor();

    resetQImages();
//    show();
}


bool SliceViewer::isSliceIndexValid(int proposedSliceIndex) {
    return proposedSliceIndex >= 0 &&
           static_cast<unsigned long>(proposedSliceIndex) <
               slice_geometry::sliceLimit(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

void SliceViewer::setAllViewersToXYZCoordinates(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    for (auto *viewer : linkedViewerList) {
        viewer->setSliceIndexWithOutUpdating(getSliceIndexFromXYZ(viewer->getSliceAxis(), x, y, z));
    }
    for (auto *viewer : linkedViewerList) {
        viewer->setSliceIndex(getSliceIndexFromXYZ(viewer->getSliceAxis(), x, y, z));
    }
    if (verbose) {
        SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Setting linked viewers to pixmap coordinates x=%1 y=%2").arg(posX).arg(posY));
    }
}

void SliceViewer::updateLastMouseXYZAfterSliceInOrDecrement() {
    switch (sliceAxis) {
        case 0:
            lastMouseX = sliceIndex;
            break;
        case 1:
            lastMouseY = sliceIndex;
            break;
        case 2:
            lastMouseZ = sliceIndex;
            break;
        default:
            SP_LOG_ERROR("viewer.interaction", QStringLiteral("Encountered invalid sliceAxis=%1").arg(sliceAxis));
            throw std::logic_error("SliceAxis not implemented!");
    }
}

void SliceViewer::incrementSliceIndex() {
    if (isSliceIndexValid(sliceIndex + 1)) {

        // TODO: Reenable preparing slice if bug fixed
//        predictedSliceIndex = sliceIndex + 2;
        setSliceIndex(sliceIndex + 1);
//        prepareSliceIndex(predictedSliceIndex);
    }
}


void SliceViewer::decrementSliceIndex() {
    if (isSliceIndexValid(sliceIndex - 1)) {
        // TODO: Reenable preparing slice if bug fixed
//        predictedSliceIndex = sliceIndex - 2;
        setSliceIndex(sliceIndex - 1);
//        prepareSliceIndex(predictedSliceIndex);
    }
}

// this is useful if you want to redraw multiple viewers at once and have them draw the correct slice indicator
void SliceViewer::setSliceIndexWithOutUpdating(int proposedSliceIndex) {
    if (isSliceIndexValid(proposedSliceIndex)) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Setting slice index without redraw to %1").arg(proposedSliceIndex));
        }
        sliceIndex = proposedSliceIndex;
    }
}

void SliceViewer::setSliceIndex(int proposedSliceIndex) {
    if (isSliceIndexValid(proposedSliceIndex)) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Setting slice index to %1").arg(proposedSliceIndex));
        }
        sliceIndex = proposedSliceIndex;
        updateLastMouseXYZAfterSliceInOrDecrement();
        QString logMessage = QString("sx: %1/%2 y: %3/%4 z:%5/%6 sliceAxis:%7").arg(lastMouseX).arg(getDimX() - 1)
                .arg(lastMouseY).arg(getDimY() - 1)
                .arg(lastMouseZ).arg(getDimZ() - 1)
                .arg(sliceAxis);
//        logMessage.sprintf("x: %01.0d y: %01.0d z: %01.0d", lastMouseY, lastMouseY, lastMouseZ);
        sendStatusMessage(logMessage);

// Protect access to signalList with a mutex
//        std::lock_guard<std::mutex> lock(signalListMutex);
//#pragma omp parallel for schedule(dynamic) default(none) shared(proposedSliceIndex)
        for (long long i = 0; i < static_cast<long long>(signalList.size()); ++i) { // this loops update the signals attached to this view
            if (signalList.at(i)->getIsActive()) {
                signalList.at(i)->setSliceIndex(proposedSliceIndex);
            }
        }
        updateFunction();
        for (auto &viewer : linkedViewerList) {
            if (viewer->getSliceAxis() != sliceAxis) {
                viewer->scheduleSliceIndicatorRepaint(sliceAxis, proposedSliceIndex);
            }
        }

        if (linkedSliderSet) {
            linkedSlider->blockSignals(
                    true); // the slider should not generate a signal to calculate the new index again
            linkedSlider->setValue(static_cast<int>(proposedSliceIndex));
            linkedSlider->blockSignals(false);
        }
        emit sliceIndexChanged(sliceAxis, proposedSliceIndex);
    }
}


void SliceViewer::updateFunction() {
//    check that this is called from the main thread!!
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());


//    bool veryVerbose = false; // this function is not the bottleneck, takes approx 1e-5 secs
//    double t=0;
//    if (veryVerbose) { t = utils::tic("PaintStart"); }
    update();
//    if (veryVerbose) { utils::toc(t, "PaintEnd"); }
}

int SliceViewer::getSliceAxis() {
    return sliceAxis;
}

int SliceViewer::getSliceIndex() {
    return sliceIndex;
}


void SliceViewer::prepareSliceIndex(int proposedSliceIndex) {
    if (isSliceIndexValid(proposedSliceIndex)) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.render", QStringLiteral("Preparing predicted slice index=%1").arg(proposedSliceIndex));
        }
        for (auto &signal : signalList) {
            if (signal->getIsActive()) {
                signal->prepareNextSliceIndexAsync(proposedSliceIndex);
            }
        }
    }
}


void SliceViewer::wheelEvent(QWheelEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()
        && !taskRunner->allowsReadOnlyInteraction()) {
        event->ignore();
        return;
    }
    int angleDelta = event->angleDelta().y();
    if (angleDelta > 0) {
        incrementSliceIndex();
    } else {
        decrementSliceIndex();
    }
}


bool SliceViewer::hasDimensionMisMatch(int dimXIn, int dimYIn, int dimZIn) {
    return ((dimXIn != dimX) || (dimYIn != dimY) || (dimZIn != dimZ));
}

void SliceViewer::addSignal(SliceViewerITKSignal *signal) {
    if (verbose) {
        SP_LOG_DEBUG("viewer.render", QStringLiteral("Adding signal to SliceViewer"));
    }
    std::lock_guard<std::mutex> lock(signalListMutex);
    auto insertionPoint = signalList.end();
    if (isLabelLayer(signal)) {
        insertionPoint = std::find_if(
            signalList.begin(),
            signalList.end(),
            [](const SliceViewerITKSignal *existingSignal) {
                return !isLabelLayer(existingSignal);
            });
    }
    signalList.insert(insertionPoint, signal);
    int newDimX, newDimY, newDimZ;
    newDimX = signal->getDimX();
    newDimY = signal->getDimY();
    newDimZ = signal->getDimZ();
    if (verbose) {
        SP_LOG_DEBUG("viewer.render", QStringLiteral("SliceViewer dimensions=%1x%2x%3").arg(newDimX).arg(newDimY).arg(newDimZ));
    }
    if (numberSignals == 0) {
        dimX = newDimX;
        dimY = newDimY;
        dimZ = newDimZ;
        resetQImages();
    } else {
        if (hasDimensionMisMatch(newDimX, newDimY, newDimZ)) {
            throw std::logic_error("Loaded image has a different dimension!");
        }
    }
    signal->setSliceIndex(sliceIndex);
    signal->setSliceAxis(sliceAxis);
    numberSignals++;
    recalculateQImages();
    updateFunction();
}

void SliceViewer::removeSignal(itkSignalBase *signal) {
    std::lock_guard<std::mutex> lock(signalListMutex);
    for (auto it = signalList.begin(); it != signalList.end(); ++it) {
        if ((*it)->getSignal() == signal) {
            delete *it;
            signalList.erase(it);
            --numberSignals;
            return;
        }
    }
}

int SliceViewer::getCurrentSliceWidth() const {
    return slice_geometry::sliceWidth(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

int SliceViewer::getCurrentSliceHeight() const {
    return slice_geometry::sliceHeight(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

void SliceViewer::drawActiveSignalLayers(QPainter &painter, const QRect &targetRect) {
    painter.save();

    for (auto *signal : signalList) {
        if (signal == nullptr || !signal->getIsActive()) {
            continue;
        }

        itkSignalBase *sourceSignal = signal->getSignal();
        if (sourceSignal == nullptr) {
            continue;
        }
        const auto layerRole = sourceSignal->getLayerRole();
        if ((imageOnlyMode && layerRole != itkSignalBase::LayerRole::SourceImage) ||
            (!imageOnlyMode && overlayOnlyMode && layerRole == itkSignalBase::LayerRole::SourceImage)) {
            continue;
        }

        QImage *sliceImage = signal->getAddressSliceQImage();
        if (sliceImage == nullptr) {
            SP_LOG_WARNING("viewer.render",
                           QStringLiteral("SliceViewer encountered an active signal without a slice image"));
            continue;
        }

        painter.setCompositionMode(
            sourceSignal->getBlendMode() == itkSignalBase::BlendMode::Additive
                ? QPainter::CompositionMode_Plus
                : QPainter::CompositionMode_SourceOver);
        if (verbose) {
            SP_LOG_DEBUG("viewer.render", QStringLiteral("Painting active SliceViewer signal"));
        }
        painter.drawImage(targetRect, *sliceImage, sliceImage->rect());
    }

    painter.restore();
}

void SliceViewer::setImageOnlyMode(bool enabled) {
    if (imageOnlyMode == enabled) {
        return;
    }
    imageOnlyMode = enabled;
    update();
}

void SliceViewer::setOverlayOnlyMode(bool enabled) {
    if (overlayOnlyMode == enabled) {
        return;
    }
    overlayOnlyMode = enabled;
    update();
}

void SliceViewer::paintEvent(QPaintEvent *event) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    if (verbose) {
        SP_LOG_DEBUG("viewer.render", QStringLiteral("SliceViewer paintEvent triggered"));
    }
    const qint64 paintStartedAtMs = verbose ? QDateTime::currentMSecsSinceEpoch() : 0;

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const QRect targetRect = rect();
    const QRect eventRect = event != nullptr ? event->rect() : QRect();
    if (backGroundImage.isNull()) {
        SP_LOG_WARNING("viewer.render", QStringLiteral("SliceViewer paint skipped because backGroundImage is not initialized"));
    }
    if (sliceIndicatorImage.isNull()) {
        SP_LOG_WARNING("viewer.render", QStringLiteral("SliceViewer paint skipped because sliceIndicatorImage is not initialized"));
    }
    painter.drawImage(targetRect, backGroundImage, backGroundImage.rect());
    drawActiveSignalLayers(painter, targetRect);
    if (!isImageOnlyMode() && !isOverlayOnlyMode()) {
        painter.drawImage(targetRect, sliceIndicatorImage, sliceIndicatorImage.rect());
    }

    const QString planeName = planeNameForSliceAxis(sliceAxis);
    const QString logKey = QString("SliceViewerPaint_%1").arg(planeName);
    const QString message = QString("[SliceViewerPaint %1] eventRect=%2,%3 %4x%5 widgetRect=%6,%7 %8x%9 "
                                    "widgetSize=%10x%11 fixedSize=%12x%13 zoom=%14 currentSlice=%15x%16 "
                                    "background=%17x%18 sliceIndicator=%19x%20 activeSignalImages=%21")
            .arg(planeName)
            .arg(eventRect.x()).arg(eventRect.y()).arg(eventRect.width()).arg(eventRect.height())
            .arg(targetRect.x()).arg(targetRect.y()).arg(targetRect.width()).arg(targetRect.height())
            .arg(width()).arg(height())
            .arg(minimumWidth()).arg(minimumHeight())
            .arg(zoomFactor, 0, 'f', 6)
            .arg(getCurrentSliceWidth()).arg(getCurrentSliceHeight())
            .arg(backGroundImage.width()).arg(backGroundImage.height())
            .arg(sliceIndicatorImage.width()).arg(sliceIndicatorImage.height())
            .arg(summarizeActiveSignalImageRects(signalList));
    logSliceViewerState(logKey, message);

    if (verbose) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("SliceViewer paintEvent finished in %1 ms")
                         .arg(QDateTime::currentMSecsSinceEpoch() - paintStartedAtMs));
    }

}

void SliceViewer::setSliceAxis(int proposedSliceAxis) {
    if (proposedSliceAxis <= 2 && proposedSliceAxis >= 0) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.interaction", QStringLiteral("Setting SliceViewer axis to %1").arg(proposedSliceAxis));
        }
        sliceAxis = proposedSliceAxis;
        for (auto &signal : signalList) {
            signal->setSliceAxis(sliceAxis);
        }
    } else {
        throw std::logic_error("sliceAxis not implemented!");
    }
    resetQImages();
    updateFunction();
}

void SliceViewer::resetQImages() {
    if (getCurrentSliceWidth() <= 0 || getCurrentSliceHeight() <= 0) {
        SP_LOG_WARNING("viewer.render",
                       QStringLiteral("SliceViewer::resetQImages() called with invalid dimensions %1x%2")
                           .arg(getCurrentSliceWidth())
                           .arg(getCurrentSliceHeight()));
        return;
    }
    backGroundImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                             static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    backGroundImage.fill(Qt::black);
    sliceIndicatorImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                                 static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    sliceIndicatorImage.fill(QColor(0, 0, 0, 0));
    setPixmap(QPixmap::fromImage(backGroundImage));
    syncViewerSizeToImage();

    const QString planeName = planeNameForSliceAxis(sliceAxis);
    const QString logKey = QString("SliceViewerReset_%1").arg(planeName);
    const QString message = QString("[SliceViewerReset %1] zoom=%2 currentSlice=%3x%4 background=%5x%6 sliceIndicator=%7x%8 widgetSize=%9x%10")
            .arg(planeName)
            .arg(zoomFactor, 0, 'f', 6)
            .arg(getCurrentSliceWidth()).arg(getCurrentSliceHeight())
            .arg(backGroundImage.width()).arg(backGroundImage.height())
            .arg(sliceIndicatorImage.width()).arg(sliceIndicatorImage.height())
            .arg(width()).arg(height());
    logSliceViewerState(logKey, message);
}


void SliceViewer::recalculateQImages() {
    if (verbose) {
        SP_LOG_DEBUG("viewer.render", QStringLiteral("Recalculating SliceViewer images"));
    }
    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) {
                SP_LOG_DEBUG("viewer.render", QStringLiteral("Recalculating active SliceViewer signal"));
            }
            signal->calculateSliceQImages();
            // TODO: Put back in if bugs are fixed
//            signal->prepareNextSliceIndexAsync(predictedSliceIndex);
        }
    }
    if (verbose) {
        SP_LOG_DEBUG("viewer.render", QStringLiteral("Finished recalculating SliceViewer images"));
    }
    updateFunction();
}

void SliceViewer::drawOtherViewerSliceIndicator(int otherSliceAxis, int otherSliceIndex) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (verbose) {
        SP_LOG_DEBUG("viewer.render", QStringLiteral("Drawing slice indicator axis=%1 index=%2").arg(otherSliceAxis).arg(otherSliceIndex));
    }
    if (sliceIndicatorImage.isNull()) {
        SP_LOG_WARNING("viewer.render", QStringLiteral("SliceViewer cannot draw the slice indicator because the image is not initialized"));
    }

    QPainter painter(&sliceIndicatorImage);

    const auto axes = voxel_geometry::planeAxes(static_cast<unsigned int>(sliceAxis));
    const bool verticalLine = otherSliceAxis == static_cast<int>(axes.horizontal);
    if (!verticalLine && otherSliceAxis != static_cast<int>(axes.vertical)) {
        return;
    }

    painter.setPen(QPen(sliceIndicatorColor(otherSliceAxis),
                        sliceIndicatorSourcePenWidth(verticalLine),
                        Qt::SolidLine,
                        Qt::RoundCap,
                        Qt::RoundJoin));
    if (verticalLine) {
        painter.drawLine(otherSliceIndex, 0, otherSliceIndex, getCurrentSliceHeight());
        indexVerticalIndicator = otherSliceIndex;
    } else {
        painter.drawLine(0, otherSliceIndex, getCurrentSliceWidth(), otherSliceIndex);
        indexHorizontalIndicator = otherSliceIndex;
    }
}

int SliceViewer::sliceIndicatorSourcePenWidth(bool verticalLine) const {
    const int sourceExtent = verticalLine ? getCurrentSliceWidth() : getCurrentSliceHeight();
    const int targetExtent = verticalLine ? width() : height();
    if (sourceExtent <= 0 || targetExtent <= 0) {
        return 1;
    }
    const double displayScale = static_cast<double>(targetExtent) / sourceExtent;
    return std::max(1, static_cast<int>(std::ceil(kSliceIndicatorDisplayPenWidth / displayScale)));
}

void SliceViewer::clearSliceIndicatorCrossingGap() {
    if (sliceIndicatorImage.isNull() || width() <= 0 || height() <= 0) {
        return;
    }

    const double scaleX = static_cast<double>(width()) / sliceIndicatorImage.width();
    const double scaleY = static_cast<double>(height()) / sliceIndicatorImage.height();
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0) {
        return;
    }

    const double sourceGapWidth = std::max(1.0, kSliceIndicatorCrossingGapSize / scaleX);
    const double sourceGapHeight = std::max(1.0, kSliceIndicatorCrossingGapSize / scaleY);
    const QRectF requestedGap(indexVerticalIndicator - sourceGapWidth / 2.0,
                              indexHorizontalIndicator - sourceGapHeight / 2.0,
                              sourceGapWidth,
                              sourceGapHeight);
    const QRectF visibleGap = requestedGap.intersected(QRectF(sliceIndicatorImage.rect()));
    if (visibleGap.isEmpty()) {
        return;
    }

    QPainter painter(&sliceIndicatorImage);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.fillRect(visibleGap, Qt::transparent);
}

QRect SliceViewer::sliceIndicatorRepaintRect(int otherSliceAxis, int otherSliceIndex) const {
    const auto axes = voxel_geometry::planeAxes(static_cast<unsigned int>(sliceAxis));
    const bool verticalLine = otherSliceAxis == static_cast<int>(axes.horizontal);
    if (!verticalLine && otherSliceAxis != static_cast<int>(axes.vertical)) {
        return {};
    }

    const int sourceExtent = verticalLine ? getCurrentSliceWidth() : getCurrentSliceHeight();
    const int targetExtent = verticalLine ? width() : height();
    const double displayScale = sourceExtent > 0
        ? static_cast<double>(targetExtent) / sourceExtent
        : 1.0;
    const int displayedPenWidth = static_cast<int>(std::ceil(
        sliceIndicatorSourcePenWidth(verticalLine) * displayScale));
    // Cover the 5 px indicator dot, its outline, and rasterization at fractional positions.
    const int repaintMargin = std::max(7, (displayedPenWidth + 1) / 2 + 2);

    if (verticalLine) {
        const int x = qRound(widgetPositionForSlicePixel(otherSliceIndex, 0).x());
        return QRect(x - repaintMargin, 0, 2 * repaintMargin + 1, height()).intersected(rect());
    }
    const int y = qRound(widgetPositionForSlicePixel(0, otherSliceIndex).y());
    return QRect(0, y - repaintMargin, width(), 2 * repaintMargin + 1).intersected(rect());
}

void SliceViewer::scheduleSliceIndicatorRepaint(int otherSliceAxis, int newSliceIndex) {
    const auto axes = voxel_geometry::planeAxes(static_cast<unsigned int>(sliceAxis));
    int *displayedIndex = nullptr;
    if (otherSliceAxis == static_cast<int>(axes.horizontal)) {
        displayedIndex = &indexVerticalIndicator;
    } else if (otherSliceAxis == static_cast<int>(axes.vertical)) {
        displayedIndex = &indexHorizontalIndicator;
    }
    if (displayedIndex == nullptr) {
        return;
    }

    update(sliceIndicatorRepaintRect(otherSliceAxis, *displayedIndex));
    if (*displayedIndex != newSliceIndex) {
        update(sliceIndicatorRepaintRect(otherSliceAxis, newSliceIndex));
        *displayedIndex = newSliceIndex;
    }
}

void SliceViewer::updateMousePosition(int mouseX, int mouseY, int mouseZ) {
    lastMouseX = mouseX;
    lastMouseY = mouseY;
    lastMouseZ = mouseZ;
}


void SliceViewer::setLinkedSlider(QSlider *linkedSliderIn) {
    linkedSliderSet = true;
    linkedSlider = linkedSliderIn;
}

QString SliceViewer::ViewSeriesExportSpec::fileName(int sliceIndex) const {
    return QStringLiteral("%1_%2.png").arg(filePrefix).arg(sliceIndex);
}

std::optional<SliceViewer::ViewSeriesExportSpec> SliceViewer::viewSeriesExportSpec() const {
    switch (sliceAxis) {
        case 0:
            return ViewSeriesExportSpec{QStringLiteral("YZ"), QStringLiteral("ZY"), dimX};
        case 1:
            return ViewSeriesExportSpec{QStringLiteral("XZ"), QStringLiteral("XZ"), dimY};
        case 2:
            return ViewSeriesExportSpec{QStringLiteral("XY"), QStringLiteral("XY"), dimZ};
        default:
            return std::nullopt;
    }
}

QString SliceViewer::exportDirectoryName() {
    return QStringLiteral("imgExport");
}


void SliceViewer::exportView() {
    const auto spec = viewSeriesExportSpec();
    if (!spec.has_value()) {
        throw std::logic_error("SliceAxis not implemented!");
    }
    exportCurrentImageToFile(spec->filePrefix + QStringLiteral(".png"));
}


void SliceViewer::exportViewSeries() {
    const auto spec = viewSeriesExportSpec();
    if (!spec.has_value()) {
        throw std::logic_error("SliceAxis not implemented!");
    }
    const int originalSliceIndex = sliceIndex;
    for (int i = 0; i < spec->sliceCount; ++i) {
        setSliceIndex(i);
        exportCurrentImageToFile(spec->fileName(i));
    }
    setSliceIndex(originalSliceIndex);
}

void SliceViewer::exportCurrentImageToFile(const QString &fileName) {
    const QString directoryName = exportDirectoryName();
    QDir().mkpath(directoryName);
    const QString filePath = QDir(directoryName).filePath(fileName);

    SP_LOG_INFO("io", QStringLiteral("Saving current view to %1").arg(filePath));
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        SP_LOG_WARNING("io",
                       QStringLiteral("Unable to open %1 for writing: %2")
                               .arg(filePath, file.errorString()));
        return;
    }

    QPixmap newPixmap = QPixmap::fromImage(backGroundImage);

    QPainter painter(&newPixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(0, 0, backGroundImage);
    drawActiveSignalLayers(painter, backGroundImage.rect());

    if (!newPixmap.save(&file, "PNG")) {
        SP_LOG_WARNING("io", QStringLiteral("Unable to save PNG to %1").arg(filePath));
    }
}

int SliceViewer::getDimX() const {
    return dimX;
}

int SliceViewer::getDimY() const {
    return dimY;
}

int SliceViewer::getDimZ() const {
    return dimZ;
}

bool SliceViewer::hasSignals() const {
    return numberSignals > 0;
}

void SliceViewer::addLinkedViewers(SliceViewer * viewer) {
    linkedViewerList.push_back(viewer);
}

void SliceViewer::setLinkedViewers(std::vector<SliceViewer *> viewerList) {
    linkedViewerList = viewerList;
}

void SliceViewer::setOrthoViewer(OrthoViewer *orthoViewerIn) {
    linkedOrthoViewer = orthoViewerIn;
}

std::vector<SliceViewer *> SliceViewer::getLinkedViewers() {
    return linkedViewerList;
}

void SliceViewer::setZoom(double zoom) {
    if (zoom > 0) {
        modifyZoom(zoom / zoomFactor);
    }
}

void SliceViewer::setVoxelSpacing(const voxel_geometry::VoxelSpacing &spacing) {
    if (!voxel_geometry::isValid(spacing)) {
        throw std::invalid_argument("Voxel spacing values must be finite and greater than zero");
    }
    voxelSpacing = spacing;
    syncViewerSizeToImage();
    refreshBrushCursor();
    update();
}

voxel_geometry::PlaneScale SliceViewer::getPlaneScale() const {
    return voxel_geometry::planeScale(voxelSpacing, static_cast<unsigned int>(sliceAxis));
}

QPoint SliceViewer::slicePixelFromWidgetPoint(const QPoint &point) const {
    return {
        slice_viewer_geometry::sourcePixelForPaintedPixel(
            point.x(), getCurrentSliceWidth(), width()),
        slice_viewer_geometry::sourcePixelForPaintedPixel(
            point.y(), getCurrentSliceHeight(), height())
    };
}

QRect SliceViewer::slicePixelBoundsFromWidgetRect(const QRect &rect) const {
    if (!rect.isValid() || getCurrentSliceWidth() <= 0 || getCurrentSliceHeight() <= 0) {
        return {};
    }
    const QRect normalized = rect.normalized();
    const QPoint first = slicePixelFromWidgetPoint(normalized.topLeft());
    const QPoint last = slicePixelFromWidgetPoint(normalized.bottomRight());
    return QRect(first, last).normalized();
}

QPointF SliceViewer::widgetPositionForSlicePixel(double sliceX, double sliceY) const {
    return {
        slice_viewer_geometry::paintedPositionForSourcePixelCenter(
            sliceX, getCurrentSliceWidth(), width()),
        slice_viewer_geometry::paintedPositionForSourcePixelCenter(
            sliceY, getCurrentSliceHeight(), height())
    };
}

QRect SliceViewer::widgetRectForSlicePixelBounds(const QRect &rect) const {
    if (!rect.isValid() || getCurrentSliceWidth() <= 0 || getCurrentSliceHeight() <= 0) {
        return {};
    }
    const QRect normalized = rect.normalized();
    const int left = slice_viewer_geometry::paintedBoundaryForSourceBoundary(
        normalized.left(), getCurrentSliceWidth(), width());
    const int top = slice_viewer_geometry::paintedBoundaryForSourceBoundary(
        normalized.top(), getCurrentSliceHeight(), height());
    const int right = slice_viewer_geometry::paintedBoundaryForSourceBoundary(
        normalized.right() + 1, getCurrentSliceWidth(), width());
    const int bottom = slice_viewer_geometry::paintedBoundaryForSourceBoundary(
        normalized.bottom() + 1, getCurrentSliceHeight(), height());
    return QRect(QPoint(left, top), QPoint(std::max(left, right - 1), std::max(top, bottom - 1)));
}

void SliceViewer::modifyZoomInAllViewers(double factor) {
    auto *viewer = orthoViewer();
    if (viewer == nullptr || viewer->xy == nullptr || viewer->xz == nullptr || viewer->zy == nullptr) {
        return;
    }

    const double currentZoom = zoomFactor;
    if (currentZoom <= 0.0) {
        return;
    }

    const int maxSliceExtent = viewer->maximumSliceExtent();
    const auto zoomLimits = slice_viewer_zoom::limitsForMaximumSliceExtent(maxSliceExtent);
    const double proposedZoom = currentZoom * factor;
    const double clampedZoom = std::clamp(proposedZoom, zoomLimits.minimum, zoomLimits.maximum);
    if (std::abs(clampedZoom - proposedZoom) > 1e-9) {
        const QString message = QString("[SliceViewerZoomClamp] current=%1 proposed=%2 minimum=%3 maximum=%4 applied=%5 maxSliceExtent=%6")
                .arg(currentZoom, 0, 'f', 6)
                .arg(proposedZoom, 0, 'f', 6)
                .arg(zoomLimits.minimum, 0, 'f', 6)
                .arg(zoomLimits.maximum, 0, 'f', 6)
                .arg(clampedZoom, 0, 'f', 6)
                .arg(maxSliceExtent);
        logSliceViewerState("SliceViewerZoomClamp", message);
    }
    if (std::abs(clampedZoom - currentZoom) > 1e-9) {
        factor = clampedZoom / currentZoom;
        if (std::abs(factor - 1.0) < 1e-9) {
            return;
        }
    } else {
        return;
    }

    viewer->zy->modifyZoom(factor);
    viewer->xz->modifyZoom(factor);
    viewer->xy->modifyZoom(factor);
    viewer->refreshZoomLayout();
}

void SliceViewer::modifyZoom(double factor) {
    const double oldZoom = zoomFactor;
    zoomFactor *= factor;
    syncViewerSizeToImage();

    const QString planeName = planeNameForSliceAxis(sliceAxis);
    const QString logKey = QString("SliceViewerZoom_%1").arg(planeName);
    const QString message = QString("[SliceViewerZoom %1] factor=%2 oldZoom=%3 newZoom=%4 currentSlice=%5x%6 widgetSize=%7x%8")
            .arg(planeName)
            .arg(factor, 0, 'f', 6)
            .arg(oldZoom, 0, 'f', 6)
            .arg(zoomFactor, 0, 'f', 6)
            .arg(getCurrentSliceWidth()).arg(getCurrentSliceHeight())
            .arg(width()).arg(height());
    logSliceViewerState(logKey, message);

    // update only xy view, it linked sliders will update other views
    if (sliceAxis == 2) {
        auto *viewer = orthoViewer();
        auto rect = viewer->scrollAreaXY->viewport()->rect();
        int horizontal_before = viewer->scrollAreaXY->horizontalScrollBar()->value();
        int horizontal_before_max = viewer->scrollAreaXY->horizontalScrollBar()->maximum();
        int verical_before = viewer->scrollAreaXY->verticalScrollBar()->value();
        int verical_before_max = viewer->scrollAreaXY->verticalScrollBar()->maximum();
        int offX;
        int offY;
        const QPointF center = viewer->xy->widgetPositionForSlicePixel(lastMouseX, lastMouseY);
        double centerXWanted = center.x();
        double centerYWanted = center.y();

        offX = static_cast<int>(centerXWanted - (rect.width() / 2.));
        offY = static_cast<int>(centerYWanted - (rect.height() / 2.));
        offX = offX > horizontal_before_max ? horizontal_before_max : offX;
        offY = offY > verical_before_max ? verical_before_max : offY;
        offX = offX < 0 ? 0 : offX;
        offY = offY < 0 ? 0 : offY;
        viewer->scrollAreaXY->horizontalScrollBar()->setValue(offX);
        viewer->scrollAreaXY->verticalScrollBar()->setValue(offY);
    }
    refreshBrushCursor();
}

OrthoViewer *SliceViewer::orthoViewer() const {
    return linkedOrthoViewer;
}

void SliceViewer::syncViewerSizeToImage() {
    const auto planeScale = getPlaneScale();
    const int scaledWidth = boundedWidgetExtent(
        static_cast<double>(getCurrentSliceWidth()) * zoomFactor * planeScale.horizontal);
    const int scaledHeight = boundedWidgetExtent(
        static_cast<double>(getCurrentSliceHeight()) * zoomFactor * planeScale.vertical);
    const QSize oldSize = size();
    setFixedSize(scaledWidth, scaledHeight);

    const QString planeName = planeNameForSliceAxis(sliceAxis);
    const QString logKey = QString("SliceViewerSize_%1").arg(planeName);
    const QString message = QString("[SliceViewerSize %1] zoom=%2 currentSlice=%3x%4 scaled=%5x%6 oldSize=%7x%8 newSize=%9x%10 min=%11x%12 max=%13x%14")
            .arg(planeName)
            .arg(zoomFactor, 0, 'f', 6)
            .arg(getCurrentSliceWidth()).arg(getCurrentSliceHeight())
            .arg(scaledWidth).arg(scaledHeight)
            .arg(oldSize.width()).arg(oldSize.height())
            .arg(width()).arg(height())
            .arg(minimumWidth()).arg(minimumHeight())
            .arg(maximumWidth()).arg(maximumHeight());
    logSliceViewerState(logKey, message);
}

void SliceViewer::refreshBrushCursor() {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    setCursor(Qt::CrossCursor);
    update();
}

unsigned int SliceViewer::getSliceIndexFromXYZ(unsigned int targetSliceAxis, int x, int y, int z) {
    switch (targetSliceAxis) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        default:
            throw (std::logic_error("SliceAxis not implemented!"));
    }
}

unsigned long SliceViewer::get3DIndexFromAnnotationSliceXY(int x, int y) {
    switch (sliceAxis) {
        case 0: {
            return sliceIndex + y * dimX + x * dimX * dimY;
        }
        case 1: {
            return x + sliceIndex * dimX + y * dimX * dimY;
        }
        case 2: {
            return x + y * dimX + sliceIndex * dimX * dimY;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
}

void
SliceViewer::getXYZfromPixmapPos(int posX, int posY, int &xOut, int &yOut, int &zOut, bool adjustForZoom) {
    const int sliceX = adjustForZoom
            ? slice_viewer_geometry::sourcePixelForPaintedPixel(posX, getCurrentSliceWidth(), width())
            : posX;
    const int sliceY = adjustForZoom
            ? slice_viewer_geometry::sourcePixelForPaintedPixel(posY, getCurrentSliceHeight(), height())
            : posY;

    switch (sliceAxis) {
        case 0: {
            xOut = sliceIndex;
            yOut = sliceY;
            zOut = sliceX;
            break;
        }
        case 1: {
            xOut = sliceX;
            yOut = sliceIndex;
            zOut = sliceY;
            break;
        }
        case 2: {
            xOut = sliceX;
            yOut = sliceY;
            zOut = sliceIndex;
            break;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
}

unsigned long SliceViewer::getAnnotationSliceXYFrom3D(itk::Index<3> index) {
    switch (sliceAxis) {
        case 0: {
            return index[2] + index[1] * dimZ;
        }
        case 1: {
            return index[0] + index[2] * dimX;
        }
        case 2: {
            return index[0] + index[1] * dimX;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
}

//SliceViewer::~SliceViewer() {
//    std::cout << "SliceViewer: Destructor\n";
//    for (auto &signal : signalList) {
//        delete signal;
//    }
//}
