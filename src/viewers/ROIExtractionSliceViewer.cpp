#include <QPainter>
#include <QtWidgets>
#include <omp.h>
#include <Qt>
#include "ROIExtractionSliceViewer.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "src/utils/utils.h"
#include "OrthoViewer.h"
#include "SliceViewer.h"


ROIExtractionSliceViewer::ROIExtractionSliceViewer(QWidget *, bool) : SliceViewer() {
    if (verbose) { std::cout << "ROIExtractionSliceViewer: Constructor\n"; }

    cursorColor = Qt::white;
    outerColor = QColor(0, 0, 0, 50);

    myPenWidth = 5;
    myPenColor = Qt::red;
    setUpCustomCursor();
    this->setMouseTracking(true);
}


void ROIExtractionSliceViewer::paintEvent(QPaintEvent *event) {
    int topLeftX = event->rect().topLeft().x();
    int topLeftY = event->rect().topLeft().y();
    int eventWidth = event->rect().width();
    int eventHeight = event->rect().height();

    if (verbose) {
        std::cout << "ROIExtractionSliceViewer: Paintevent triggered: " << event->rect().width()
                  << " x " << event->rect().height() << std::endl;
        printf("x0: %d y0: %d x1: %d y1: %d\n", topLeftX, topLeftY, eventWidth, eventHeight);
    }
    double tic = omp_get_wtime();

    QPainter painter(this);
    painter.scale(zoomFactor, zoomFactor);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    painter.drawImage(0, 0, backGroundImage);

    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) { std::cout << "AnnotationViewer: Painting new signal" << std::endl; }
            painter.drawImage(0, 0, *(signal->getAddressSliceQImage()));
        }
    }

    sliceIndicatorImage.fill(QColor(0, 0, 0, 0)); // erase old slice indicator image!
    for (auto &viewer : GraphBase::viewerList) {
        drawOtherViewerSliceIndicator(viewer->getSliceAxis(), viewer->getSliceIndex());
    }
    painter.drawImage(0, 0, sliceIndicatorImage);

    int dotRadius = 5;
    int dotAlpha = 255;

    QColor xColor = QColor(255, 0, 0, dotAlpha);
    QColor yColor = QColor(0, 255, 0, dotAlpha);
    QColor zColor = QColor(255, 255, 0, dotAlpha);

    switch (sliceAxis) {
        case 0:
            painter.setBrush(QBrush(yColor));
            painter.drawEllipse(QPoint(indexVerticalIndicator, lastMouseY), dotRadius, dotRadius);
            painter.setBrush(QBrush(zColor));
            painter.drawEllipse(QPoint(lastMouseZ, indexHorizontalIndicator), dotRadius, dotRadius);
            break;
        case 1:
            painter.setBrush(QBrush(zColor));
            painter.drawEllipse(QPoint(indexVerticalIndicator, lastMouseZ), dotRadius, dotRadius);
            painter.setBrush(QBrush(xColor));
            painter.drawEllipse(QPoint(lastMouseX, indexHorizontalIndicator), dotRadius, dotRadius);
            break;
        case 2:
            painter.setBrush(QBrush(yColor));
            painter.drawEllipse(QPoint(indexVerticalIndicator, lastMouseY), dotRadius, dotRadius);
            painter.setBrush(QBrush(xColor));
            painter.drawEllipse(QPoint(lastMouseX, indexHorizontalIndicator), dotRadius, dotRadius);
            break;
        default:
            throw (std::logic_error("SliceAxis not implemented!"));
    }

    double toc = omp_get_wtime();
    if (verbose) { std::cout << "duration ROIExtractionSliceViewer PaintEvent: " << toc - tic << std::endl; }
}


void ROIExtractionSliceViewer::mousePressEvent(QMouseEvent *event) {

}


void ROIExtractionSliceViewer::mouseMoveEvent(QMouseEvent *event) {
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

    sendStatusMessage(logMessage);

    for (auto &viewer : GraphBase::viewerList) {
        viewer->updateMousePosition(x, y, z);
        viewer->updateFunction();
    }

    //TODO: That stuff should be in sliceviewer right?
    if (event->buttons() == Qt::MiddleButton) {
        int current_x = event->pos().x();
        int current_y = event->pos().y();
        double scaleFactor = 0.2;
        double delta_x = scaleFactor * (current_x - old_middle_click_translate_x_pos);
        double delta_y = scaleFactor * (current_y - old_middle_click_translate_y_pos);

        QScrollAreaNoWheel *currentScrollArea;
        if (sliceAxis == 0) {
            currentScrollArea = GraphBase::pOrthoViewer->scrollAreaZY;
        } else if (sliceAxis == 1) { // xz
            currentScrollArea = GraphBase::pOrthoViewer->scrollAreaXZ;
        } else if (sliceAxis == 2) {
            currentScrollArea = GraphBase::pOrthoViewer->scrollAreaXY;
        } else {
            throw std::logic_error("slice axis not implemented");
        }

        int current_horizontal_value = currentScrollArea->horizontalScrollBar()->value();
        int current_vertical_value = currentScrollArea->verticalScrollBar()->value();
        currentScrollArea->horizontalScrollBar()->setValue(current_horizontal_value + delta_x);
        currentScrollArea->verticalScrollBar()->setValue(current_vertical_value + delta_y);

        old_middle_click_translate_x_pos = event->pos().x();
        old_middle_click_translate_y_pos = event->pos().y();
    }
}

void ROIExtractionSliceViewer::mouseReleaseEvent(QMouseEvent *event) {

}


void ROIExtractionSliceViewer::updateFunction() {
    update();
}

