#include "OrthoViewer.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "ROIExtractionSliceViewer.h"
#include <Qt>
#include <QtWidgets>
#include "src/segment_handling/graphBase.h"
#include <QPainter>
#include "SliceViewer.h"
#include "SliceViewerITKSignal.h"
#include "src/utils/utils.h"
#include <itkImageRegionConstIteratorWithIndex.h>
#include <QWheelEvent>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <mutex>


SliceViewer::SliceViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent, bool verbose) : verbose{verbose} {
    setParent(parent);
    if (verbose) { std::cout << "SliceViewer: Constructor\n"; }
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    setFocusPolicy(Qt::WheelFocus);

    graphBase = graphBaseIn;

    linkedSliderSet = false;

    dimX = 1;
    dimY = 1;
    dimZ = 1;

    //TODO: I'm not sure these belong here -> move them to AnnotationViewer?
    cmdClicked = false;
    sClicked = false;
    pClicked = false;
    dClicked = false;
    xClicked = false;
    cClicked = false;
    qClicked = false;
    fClicked = false;
    gClicked = false;
    shiftClicked = false;
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
    switch (sliceAxis) {
        case 0:
            return proposedSliceIndex < dimX && proposedSliceIndex >= 0;
        case 1:
            return proposedSliceIndex < dimY && proposedSliceIndex >= 0;
        case 2:
            return proposedSliceIndex < dimZ && proposedSliceIndex >= 0;
        default:
            throw std::logic_error("SliceAxis not implemented!");
    }
}

void SliceViewer::setAllViewersToXYZCoordinates(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    for (auto &viewer : graphBase->viewerList) {
        viewer->setSliceIndexWithOutUpdating(getSliceIndexFromXYZ(viewer->getSliceAxis(), x, y, z));
    }
    for (auto &viewer : graphBase->viewerList) {
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
        resize(getCurrentSliceWidth(), getCurrentSliceHeight());
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
    switch (sliceAxis) {
        case 0: {
            return dimZ;
        }
        case 1: {
            return dimX;
        }
        case 2: {
            return dimX;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
}

int SliceViewer::getCurrentSliceHeight() {
    switch (sliceAxis) {
        case 0: {
            return dimY;
        }
        case 1: {
            return dimZ;
        }
        case 2: {
            return dimY;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
}

void SliceViewer::paintEvent(QPaintEvent *event) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    qWarning() << "This is a test!!!!";


    if (verbose) { std::cout << "Viewer: Paintevent triggered" << std::endl; }
    double tic = utils::tic();

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (backGroundImage.isNull()) {
        qWarning() << "backGroundImage is not initialized!";
    }
    if (sliceIndicatorImage.isNull()) {
        qWarning() << "sliceIndicatorImage is not initialized!";
    }
    painter.drawImage(event->rect(), backGroundImage, event->rect());
    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) { std::cout << "Viewer: Painting new signal" << std::endl; }
            if (signal->getAddressSliceQImage() == nullptr) {
                qWarning() << "signal->getAddressSliceQImage() is nullptr!";
            }
            painter.drawImage(event->rect(), *(signal->getAddressSliceQImage()), event->rect());
        }
    }
    painter.drawImage(event->rect(), sliceIndicatorImage, event->rect());
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
}


//TODO: add option to only recalculate qimages in a certain rect? would speed up things a lot when merging etc
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
    //TODO: SliceIndicator should only be a very narrow rectangle that gets painted not over the whole pixmap, but where it belongs to
//    check if sliceIndicatorImage is initialized
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

std::vector<SliceViewer *> SliceViewer::getLinkedViewers() {
    return linkedViewerList;
}

void SliceViewer::modifyZoomInAllViewers(double factor) {
    graphBase->pOrthoViewer->zy->modifyZoom(factor);
    graphBase->pOrthoViewer->xz->modifyZoom(factor);
    graphBase->pOrthoViewer->xy->modifyZoom(factor);
    graphBase->pOrthoViewer->updateMaximumSizes(zoomFactor);
}

void SliceViewer::modifyZoom(double factor) {
    zoomFactor *= factor;
    double currentWidth = this->width();
    double currentHeight = this->height();
    setFixedSize(static_cast<int>(currentWidth * factor), static_cast<int>(currentHeight * factor));

    // update only xy view, it linked sliders will update other views
    if (sliceAxis == 2) {
        printf("Ensuring that x0: %.1d x1:%.1d is visible!\n", lastMouseX, lastMouseY);
        auto rect = graphBase->pOrthoViewer->scrollAreaXY->viewport()->rect();
        printf("%d %d %d %d\n", rect.x(), rect.y(), rect.width(), rect.height());
//            GraphBase::pOrthoViewer->scrollAreaXY->scroll()
        int horizontal_before = graphBase->pOrthoViewer->scrollAreaXY->horizontalScrollBar()->value();
        int horizontal_before_max = graphBase->pOrthoViewer->scrollAreaXY->horizontalScrollBar()->maximum();
        int verical_before = graphBase->pOrthoViewer->scrollAreaXY->verticalScrollBar()->value();
        int verical_before_max = graphBase->pOrthoViewer->scrollAreaXY->verticalScrollBar()->maximum();
        printf("Scrollbars horizonal: %d/%d vertical: %d/%d\n",
               horizontal_before, horizontal_before_max,
               verical_before, verical_before_max);
        int offX;
        int offY;
        double centerXWanted = lastMouseX * zoomFactor;
        double centerYWanted = lastMouseY * zoomFactor;
        printf("Scaled Point: x0: %.1f x1:%.1f! (factor: %.1f)\n", centerXWanted, centerYWanted, zoomFactor);

        // centerX = off + (recW/2)

        offX = static_cast<int>(centerXWanted - (rect.width() / 2.));
        offY = static_cast<int>(centerYWanted - (rect.height() / 2.));
        offX = offX > horizontal_before_max ? horizontal_before_max : offX;
        offY = offY > verical_before_max ? verical_before_max : offY;
        offX = offX < 0 ? 0 : offX;
        offY = offY < 0 ? 0 : offY;
        printf("New scrollbars horizonal: %d/%d vertical: %d/%d\n",
               offX, horizontal_before_max,
               offX, verical_before_max);
        graphBase->pOrthoViewer->scrollAreaXY->horizontalScrollBar()->setValue(offX);
        graphBase->pOrthoViewer->scrollAreaXY->verticalScrollBar()->setValue(offY);
    }
    setUpCustomCursor();
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
