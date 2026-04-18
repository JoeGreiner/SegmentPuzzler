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
#include "SliceViewerITKSignal.h"
#include "src/utils/utils.h"
#include <itkImageRegionConstIteratorWithIndex.h>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <mutex>

namespace {

constexpr double kMaximumScaledSliceExtent = 8192.0;

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
    static QHash<QString, QString> lastLogs;
    if (lastLogs.value(key) == message) {
        return;
    }

    qInfo().noquote() << message;
    lastLogs.insert(key, message);
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

} // namespace


SliceViewer::SliceViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent, bool verbose)
    : SliceViewer(graphBaseIn, nullptr, parent, verbose) {
}

SliceViewer::SliceViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn, QWidget *parent, bool verbose)
    : verbose{verbose} {
    setParent(parent);
    if (verbose) { std::cout << "SliceViewer: Constructor\n"; }
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    setFocusPolicy(Qt::WheelFocus);

    graphBase = graphBaseIn;
    taskRunner = taskRunnerIn;

    linkedSliderSet = false;

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
    outerColor = QColor(0, 0, 0, 50);
    myPenWidth = 5;
    myPenColor = Qt::red;
    setUpCustomCursor();

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
    if (verbose) { std::cout << posX << " " << posY << " \n"; }
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
            std::cout << "SliceAxis: " << sliceAxis << "\n";
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
        if (verbose) { std::cout << "Setting SliceIndex without updating: " << proposedSliceIndex << std::endl; }
        sliceIndex = proposedSliceIndex;
    }
}

void SliceViewer::setSliceIndex(int proposedSliceIndex) {
    if (isSliceIndexValid(proposedSliceIndex)) {
        if (verbose) { std::cout << "Setting Slice: " << proposedSliceIndex << std::endl; }
        int oldSliceIndex = sliceIndex;
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
//         update slice index indicators
// two things that have to be updated:
// old indicator has to be erase (around oldSliceIndex)
// new indicator has to be drawn
        int xStart, width, yStart, height, xStart2, yStart2;
        int buffer = 10;
        for (auto &viewer : linkedViewerList) {
            if (viewer->getSliceAxis() != sliceAxis) {
                if (sliceAxis == 2) { //TODO: Is there a prettier way?
                    // xy viewer updating xz and zy
                    if (viewer->getSliceAxis() == 0) { // zy
                        xStart = (proposedSliceIndex - buffer) * zoomFactor;
                        xStart2 = (oldSliceIndex - buffer) * zoomFactor;

                        width = 2 * buffer * zoomFactor;
                        height = (viewer->getCurrentSliceHeight()) * zoomFactor;

                        yStart = 0;
                        // attention: update works in zoomed-image-space!
                        viewer->update(xStart, yStart, width, height);
                        viewer->update(xStart2, yStart, width, height);
                    } else { // xz
                        xStart = 0;
                        width = (viewer->getCurrentSliceWidth()) * zoomFactor;
                        yStart = (oldSliceIndex - buffer) * zoomFactor;
                        height = 2 * buffer * zoomFactor;
                        yStart2 = (proposedSliceIndex - buffer) * zoomFactor;

                        viewer->update(xStart, yStart, width, height);
                        viewer->update(xStart, yStart2, width, height);
                    }
                } else if (sliceAxis == 1) { // xz viewer updating zy and xy
                    xStart = 0;
                    width = (viewer->getCurrentSliceWidth()) * zoomFactor;
                    yStart = (oldSliceIndex - buffer) * zoomFactor;
                    height = 2 * buffer * zoomFactor;
                    yStart2 = (proposedSliceIndex - buffer) * zoomFactor;
                    viewer->update(xStart, yStart, width, height);
                    viewer->update(xStart, yStart2, width, height);
                } else { // zy viewer updating xy and xz
                    xStart = (proposedSliceIndex - buffer) * zoomFactor;
                    xStart2 = (oldSliceIndex - buffer) * zoomFactor;
                    width = 2 * buffer * zoomFactor;
                    height = (viewer->getCurrentSliceHeight()) * zoomFactor;
                    yStart = 0;
                    viewer->update(xStart, yStart, width, height);
                    viewer->update(xStart2, yStart, width, height);
                }
            }
        }

        if (linkedSliderSet) {
            linkedSlider->blockSignals(
                    true); // the slider should not generate a signal to calculate the new index again
            linkedSlider->setValue(static_cast<int>(proposedSliceIndex));
            linkedSlider->blockSignals(false);
        }
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
        if (verbose) { std::cout << "Preparing Slice: " << proposedSliceIndex << std::endl; }
        for (auto &signal : signalList) {
            if (signal->getIsActive()) {
                signal->prepareNextSliceIndexAsync(proposedSliceIndex);
            }
        }
    }
}


void SliceViewer::wheelEvent(QWheelEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()) {
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
    if (verbose) { std::cout << "Viewer: Adding Signal" << std::endl; }
    std::lock_guard<std::mutex> lock(signalListMutex);
    signalList.push_back(signal);
    int newDimX, newDimY, newDimZ;
    newDimX = signalList.back()->getDimX();
    newDimY = signalList.back()->getDimY();
    newDimZ = signalList.back()->getDimZ();
    if (verbose) { std::cout << "Viewer: Size: " << newDimX << " " << newDimY << " " << newDimZ << std::endl; }
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
    signalList.back()->setSliceIndex(sliceIndex);
    signalList.back()->setSliceAxis(sliceAxis);
    numberSignals++;
    recalculateQImages();
    updateFunction();
}


int SliceViewer::getCurrentSliceWidth() {
    return slice_geometry::sliceWidth(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

int SliceViewer::getCurrentSliceHeight() {
    return slice_geometry::sliceHeight(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

void SliceViewer::paintEvent(QPaintEvent *event) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    if (verbose) { std::cout << "Viewer: Paintevent triggered" << std::endl; }
    double tic = utils::tic();

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const QRect targetRect = rect();
    const QRect eventRect = event != nullptr ? event->rect() : QRect();
    if (backGroundImage.isNull()) {
        qWarning() << "backGroundImage is not initialized!";
    }
    if (sliceIndicatorImage.isNull()) {
        qWarning() << "sliceIndicatorImage is not initialized!";
    }
    painter.drawImage(targetRect, backGroundImage, backGroundImage.rect());
    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) { std::cout << "Viewer: Painting new signal" << std::endl; }
            if (signal->getAddressSliceQImage() == nullptr) {
                qWarning() << "signal->getAddressSliceQImage() is nullptr!";
            }
            painter.drawImage(targetRect,
                              *(signal->getAddressSliceQImage()),
                              signal->getAddressSliceQImage()->rect());
        }
    }
    painter.drawImage(targetRect, sliceIndicatorImage, sliceIndicatorImage.rect());

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

    if (verbose) { utils::toc(tic, "Viewer: finished PaintEvent: ");}

}

void SliceViewer::setSliceAxis(int proposedSliceAxis) {
    if (proposedSliceAxis <= 2 && proposedSliceAxis >= 0) {
        if (verbose) { std::cout << "Viewer: sliceAxis: " << proposedSliceAxis << std::endl; }
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
        qWarning() << "SliceViewer::resetQImages() called with invalid dimensions!";
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
    double t = 0;
    if (verbose) { t = utils::tic("Viewer: recalculating QImages"); }
    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) { std::cout << "Viewer: recalculating Signal" << std::endl; }
            signal->calculateSliceQImages();
            // TODO: Put back in if bugs are fixed
//            signal->prepareNextSliceIndexAsync(predictedSliceIndex);
        }
    }
    if (verbose) { utils::toc(t, "Viewer: finished recalculating QImages"); }
    updateFunction();
}

void SliceViewer::drawOtherViewerSliceIndicator(int otherSliceAxis, int otherSliceIndex) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (verbose) { std::cout << "drawing indicator: " << otherSliceAxis << " " << otherSliceIndex << std::endl; }
    if (sliceIndicatorImage.isNull()) {
        qWarning() << "sliceIndicatorImage is not initialized!";
    }

    QPainter painter(&sliceIndicatorImage);

    int lineAlpha = 255;
    QColor xy_red = QColor(255, 0, 0, lineAlpha);
    QColor xz_green = QColor(0, 255, 0, lineAlpha);
    QColor yz_yellow = QColor(255, 255, 0, lineAlpha);
    int penWidth = static_cast<int>(2/zoomFactor);

    switch (sliceAxis) {
        case 0: // zy
            if (otherSliceAxis == 1) { // y
                painter.setPen(QPen(xz_green, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(0, otherSliceIndex, getCurrentSliceWidth(), otherSliceIndex);
                indexHorizontalIndicator = otherSliceIndex;
            } else if (otherSliceAxis == 2) { // z
                painter.setPen(QPen(xy_red, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(otherSliceIndex, 0, otherSliceIndex, getCurrentSliceHeight());
                indexVerticalIndicator = otherSliceIndex;
            }
            break;
        case 1: // xz
            if (otherSliceAxis == 0) { // zy
                painter.setPen(QPen(yz_yellow, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(otherSliceIndex, 0, otherSliceIndex, getCurrentSliceHeight());
                indexVerticalIndicator = otherSliceIndex;
            } else if (otherSliceAxis == 2) { // xy
                painter.setPen(QPen(xy_red, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(0, otherSliceIndex, getCurrentSliceWidth(), otherSliceIndex);
                indexHorizontalIndicator = otherSliceIndex;
            }
            break;
        case 2: // xy
            if (otherSliceAxis == 0) { // x = 10
                painter.setPen(QPen(yz_yellow, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(otherSliceIndex, 0, otherSliceIndex, getCurrentSliceHeight());
                indexVerticalIndicator = otherSliceIndex;
            } else if (otherSliceAxis == 1) {
                painter.setPen(QPen(xz_green, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawLine(0, otherSliceIndex, getCurrentSliceWidth(), otherSliceIndex);
                indexHorizontalIndicator = otherSliceIndex;
            }
            break;
        default:
            throw std::logic_error("SliceAxis not implemented!");
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


void SliceViewer::exportView() {
    std::string prefix;
    switch (sliceAxis) {
        case 0:
            prefix = "ZY";
            break;
        case 1:
            prefix = "XZ";
            break;
        case 2:
            prefix = "XY";
            break;
        default:
            throw std::logic_error("SliceAxis not implemented!");
    }
    exportCurrentImageToFile(prefix);
}


void SliceViewer::exportVideo() {
    int maxSliceIndex;
    std::string prefix;
    switch (sliceAxis) {
        case 0:
            maxSliceIndex = dimX;
            prefix = "ZY";
            break;
        case 1:
            maxSliceIndex = dimY;
            prefix = "XZ";
            break;
        case 2:
            maxSliceIndex = dimZ;
            prefix = "XY";
            break;
        default:
            throw std::logic_error("SliceAxis not implemented!");
    }
    std::string tmpFileName;
    for (int i = 0; i < maxSliceIndex; i++) {
        tmpFileName = prefix + "_" + std::to_string(i);
        setSliceIndex(i);
        exportCurrentImageToFile(tmpFileName);
    }
}

void SliceViewer::exportCurrentImageToFile(std::string filePrefix) {
    filePrefix = "imgExport/" + filePrefix + ".png";

    std::cout << "Saving current view to: " << filePrefix << "\n";
    QFile file(filePrefix.c_str());
    file.open(QIODevice::WriteOnly);

    QPixmap newPixmap = QPixmap::fromImage(backGroundImage);

    QPainter painter(&newPixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(0, 0, backGroundImage);
    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            painter.drawImage(0, 0, *(signal->getAddressSliceQImage()));
        }
    }


    newPixmap.save(&file, "PNG");
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

void SliceViewer::modifyZoomInAllViewers(double factor) {
    auto *viewer = orthoViewer();
    if (viewer == nullptr || viewer->xy == nullptr || viewer->xz == nullptr || viewer->zy == nullptr) {
        return;
    }

    const double currentZoom = zoomFactor;
    if (currentZoom <= 0.0) {
        return;
    }

    const double fittedZoom = viewer->computeFittedZoom();
    const double minimumZoom = fittedZoom;
    const int maxSliceExtent = std::max(std::max(viewer->xy->getCurrentSliceWidth(),
                                                 viewer->xy->getCurrentSliceHeight()),
                                        std::max(std::max(viewer->xz->getCurrentSliceWidth(),
                                                          viewer->xz->getCurrentSliceHeight()),
                                                 std::max(viewer->zy->getCurrentSliceWidth(),
                                                          viewer->zy->getCurrentSliceHeight())));
    const double maximumZoom = std::max(minimumZoom,
                                        kMaximumScaledSliceExtent / std::max(1, maxSliceExtent));
    const double proposedZoom = currentZoom * factor;
    const double clampedZoom = std::clamp(proposedZoom, minimumZoom, maximumZoom);
    if (std::abs(clampedZoom - proposedZoom) > 1e-9) {
        const QString message = QString("[SliceViewerZoomClamp] current=%1 proposed=%2 minimum=%3 maximum=%4 applied=%5 fitted=%6 maxSliceExtent=%7")
                .arg(currentZoom, 0, 'f', 6)
                .arg(proposedZoom, 0, 'f', 6)
                .arg(minimumZoom, 0, 'f', 6)
                .arg(maximumZoom, 0, 'f', 6)
                .arg(clampedZoom, 0, 'f', 6)
                .arg(fittedZoom, 0, 'f', 6)
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
        double centerXWanted = lastMouseX * zoomFactor;
        double centerYWanted = lastMouseY * zoomFactor;

        offX = static_cast<int>(centerXWanted - (rect.width() / 2.));
        offY = static_cast<int>(centerYWanted - (rect.height() / 2.));
        offX = offX > horizontal_before_max ? horizontal_before_max : offX;
        offY = offY > verical_before_max ? verical_before_max : offY;
        offX = offX < 0 ? 0 : offX;
        offY = offY < 0 ? 0 : offY;
        viewer->scrollAreaXY->horizontalScrollBar()->setValue(offX);
        viewer->scrollAreaXY->verticalScrollBar()->setValue(offY);
    }
    setUpCustomCursor();
}

OrthoViewer *SliceViewer::orthoViewer() const {
    return linkedOrthoViewer;
}

void SliceViewer::syncViewerSizeToImage() {
    const int scaledWidth = std::max(1, static_cast<int>(std::lround(getCurrentSliceWidth() * zoomFactor)));
    const int scaledHeight = std::max(1, static_cast<int>(std::lround(getCurrentSliceHeight() * zoomFactor)));
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

void SliceViewer::setUpCustomCursor() {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    int zoomAdjustedPenWidth = myPenWidth * zoomFactor;
    cursorPixMap = QPixmap(QSize(zoomAdjustedPenWidth + 5, zoomAdjustedPenWidth + 5));

    cursorPixMap.fill(Qt::transparent);

    QPainter cursorPainter(&cursorPixMap);
    cursorPainter.setRenderHint(QPainter::Antialiasing);

    int cursorLineWidth = 2;

    int startX = 2;
    int startY = 2;

    int rectWidth = zoomAdjustedPenWidth;
    int rectHeight = zoomAdjustedPenWidth;

    cursorPainter.setPen(QPen(outerColor, cursorLineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    cursorPainter.drawEllipse(startX - 1, startY - 1, rectWidth + 2, rectHeight + 2);

    cursorPainter.setPen(QPen(cursorColor, cursorLineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    cursorPainter.drawEllipse(startX, startY, rectWidth, rectHeight);

    this->setCursor(cursorPixMap);

    // update application around cursor
    QPoint pos = QCursor::pos();
    this->update(pos.x() - (zoomAdjustedPenWidth / 2) - 2,
                 pos.x() - (zoomAdjustedPenWidth / 2) - 2,
                 zoomAdjustedPenWidth + 5,
                 zoomAdjustedPenWidth + 5);
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
    switch (sliceAxis) {
        case 0: {
            xOut = sliceIndex;
            yOut = adjustForZoom ? static_cast<int>(posY / zoomFactor) : posY;
            zOut = adjustForZoom ? static_cast<int>(posX / zoomFactor) : posX;
            break;
        }
        case 1: {
            xOut = adjustForZoom ? static_cast<int>(posX / zoomFactor) : posX;
            yOut = sliceIndex;
            zOut = adjustForZoom ? static_cast<int>(posY / zoomFactor) : posY;
            break;
        }
        case 2: {
            xOut = adjustForZoom ? static_cast<int>(posX / zoomFactor) : posX;
            yOut = adjustForZoom ? static_cast<int>(posY / zoomFactor) : posY;
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
