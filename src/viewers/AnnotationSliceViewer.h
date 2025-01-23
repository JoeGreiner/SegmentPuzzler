#ifndef HELLOWORLD_ANNOTATIONSLICEVIEWER_H
#define HELLOWORLD_ANNOTATIONSLICEVIEWER_H

#include <src/viewers/SliceViewer.h>
#include "src/segment_handling/graphBase.h"
#include "src/segment_handling/graph.h"

class AnnotationSliceViewer : public SliceViewer {
Q_OBJECT

public:
    explicit AnnotationSliceViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent = 0, bool verbose = false);

    ~AnnotationSliceViewer() override;

    void resetQImages() override;

    void togglePaintMode();

    void togglePaintBoundaryMode();

    itk::Image<unsigned char, 3>::Pointer pThresholdedBoundaries;
    itkSignalBase * pThresholdedBoundariesSignal;

//   this is also used to process annotations in paintmode!!
    dataType::SegmentIdType labelOfClickedSegmentation;


public slots:
    void toggleROISelectonModeIsActive();

    void turnROISelectonModeInactive();

    void turnROISelectonModeActive();

    void refineSegmentByPosition(int posX, int posY);

    void runOpenSegmentationLabel(int posX, int posY);

    void runFillSegmentationLabel(int posX, int posY);

    void openSegmentationLabel(int posX, int posY);

    void fillSegmentationLabel(int posX, int posY);

    void exportDebugInformation();

    void setPaintId(dataType::SegmentIdType);


protected:
    void paintEvent(QPaintEvent *event) override;

    void keyPressEvent(QKeyEvent *event) override;

    void keyReleaseEvent(QKeyEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    void updateFunction() override;

    void drawLineTo(QPoint endPoint);

    void setPenWidth(int newPenWidth);

    void processAnnotationImage(QImage image);

    void splitWorkingNodeIntoInitialNodes(int posX, int posY);

    void removeInitialSegmentFromWorkingSegmentAtClick(int posX, int posY);

    void transferWorkingNodeToSegmentation(int posX, int posY);

    void deleteConnectedLabelFromSegmentation(int posX, int posY);

    void updatePenWidthInAllViewers(int newPenWidth);



private:
    std::vector<quint32> tmpImageEdge;
    QImage annotationImage, edgeImage;

    bool paintModeIsActive;
    bool paintBoundaryModeIsActive;

    bool scribbling, rightClicked;

    bool ROISelectionModeIsActive;


    // last recognizerd point when cursor is drawing in paintmode
    QPoint lastPoint;

    void drawPoint(QPoint point);

    QRubberBand *ROISelectionRubberBand;

    void getSegmentationLabelIdAtCursor(int x, int y);
    QPoint ROISelectionOrigin;

};


#endif //HELLOWORLD_ANNOTATIONSLICEVIEWER_H
