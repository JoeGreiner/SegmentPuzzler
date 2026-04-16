#ifndef HELLOWORLD_ANNOTATIONSLICEVIEWER_H
#define HELLOWORLD_ANNOTATIONSLICEVIEWER_H

#include <src/viewers/SliceViewer.h>
#include "src/segment_handling/graphBase.h"
#include "src/segment_handling/Graph.h"

class AnnotationSliceViewer : public SliceViewer {
Q_OBJECT

public:
    explicit AnnotationSliceViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn,
                                   QWidget *parent = 0, bool verbose = false);

    ~AnnotationSliceViewer() override;

    void resetQImages() override;

    void togglePaintMode();

    void togglePaintBoundaryMode();

    bool isPaintModeActive() const { return paintModeIsActive; }
    bool isPaintBoundaryModeActive() const { return paintBoundaryModeIsActive; }
    bool isROISelectionModeActive() const { return ROISelectionModeIsActive; }

    itk::Image<unsigned char, 3>::Pointer pThresholdedBoundaries;
    itkSignalBase * pThresholdedBoundariesSignal;

//   this is also used to process annotations in paintmode!!
    dataType::SegmentIdType labelOfClickedSegmentation;


public slots:
    void toggleROISelectonModeIsActive();

    void turnROISelectonModeInactive();

    void turnROISelectonModeActive();

    void refineSegmentByPosition(int posX, int posY);
    void runInsertSegmentationSegmentIntoInitialSegments(int posX, int posY);


    void runOpenSegmentationLabel(int posX, int posY);

    void runFillSegmentationLabel(int posX, int posY);

    void runDilateSegmentationLabel(int posX, int posY);

    void runErodeSegmentationLabel(int posX, int posY);

    void openSegmentationLabel(int posX, int posY);

    void fillSegmentationLabel(int posX, int posY);

    void dilateSegmentationLabel(int posX, int posY);

    void erodeSegmentationLabel(int posX, int posY);

    void exportDebugInformation();

    void setPaintId(dataType::SegmentIdType);
    void setOpeningRadius(int radius);
    void setClosingRadius(int radius);
    void setDilationRadius(int radius);
    void setErosionRadius(int radius);
    int getOpeningRadius() const { return openingRadius; }
    int getClosingRadius() const { return closingRadius; }
    int getDilationRadius() const { return dilationRadius; }
    int getErosionRadius() const { return erosionRadius; }


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
    dataType::SegmentsImageType::Pointer active3DViewSegmentsImage() const;
    itkSignal<dataType::SegmentIdType> *active3DViewSignal() const;
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
    void setLinkedToolModeAndNotify(std::vector<SliceViewer *> &viewerList, ToolMode toolMode);
    void notifyOrthoViewerInteractionModeChanged();
    void showPrepared3DView(std::vector<std::pair<dataType::SegmentIdType, quint32>> labels,
                            const QString &progressText);
    void show3DSegmentView(int posX, int posY);
    void show3DAllLabelsView();
    QPoint ROISelectionOrigin;
    int openingRadius = 3;
    int closingRadius = 8;
    int dilationRadius = 1;
    int erosionRadius = 1;

};


#endif //HELLOWORLD_ANNOTATIONSLICEVIEWER_H
