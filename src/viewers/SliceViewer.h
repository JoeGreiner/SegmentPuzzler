#ifndef HELLOWORLD_SLICEVIEWER_H
#define HELLOWORLD_SLICEVIEWER_H

#include <QLabel>
#include <itkImage.h>
#include <QSlider>
#include <src/viewers/SliceViewer.h>
#include "SliceViewerITKSignal.h"
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/graphBase.h"
#include "src/segment_handling/graph.h"



class SliceViewer : public QLabel {
Q_OBJECT

public:
    static const int Dimension = dataType::Dimension;
    using CharImageType = itk::Image<unsigned char, Dimension>;
    using ShortImageType = itk::Image<short, Dimension>;

//    ~SliceViewer() override;

    SliceViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent = nullptr, bool verbose = false
    );

    virtual void addSignal(SliceViewerITKSignal *signal);

    bool hasDimensionMisMatch(int dimXIn, int dimYIn, int dimZIn);

    void incrementSliceIndex();

    void decrementSliceIndex();

    void drawOtherViewerSliceIndicator(int otherSliceAxis, int otherSliceIndex);

    void updateMousePosition(int mouseX, int mouseY, int mouseZ);


    void setLinkedSlider(QSlider *linkedSliderIn);

    void setSliceIndexWithOutUpdating(int proposedSliceIndex);

    void addLinkedViewers(SliceViewer *viewer);

    void setLinkedViewers(std::vector<SliceViewer *> viewerList);

    std::vector<SliceViewer *> getLinkedViewers();

    virtual void setSliceAxis(int proposedSliceAxis);

    int getCurrentSliceWidth();

    int getCurrentSliceHeight();

    int getSliceAxis();

    int getSliceIndex();

    bool isSliceIndexValid(int proposedSliceIndex);

    virtual void prepareSliceIndex(int proposedSliceIndex);


    void setAllViewersToXYZCoordinates(int posX, int posY);

    static unsigned int getSliceIndexFromXYZ(unsigned int targetSliceAxis, int x, int y, int z);

    unsigned long get3DIndexFromAnnotationSliceXY(int x, int y);

    unsigned long getAnnotationSliceXYFrom3D(itk::Index<3> index);



    // this can be as simple as update() or a custom repaint function
    virtual void updateFunction();


//    void recalculateLUT();
    virtual void recalculateQImages();

    virtual void resetQImages();

    // if the cmd key was clicked
    bool cmdClicked;

    bool shiftClicked;
    // if watershed refinement keys are pressed: s for by [s]egment, p for by [p]osition
    // transfer node at cursor fom workingsegment to active segmentation
    bool sClicked;
    // delete node at cursor in active segmentation
    bool dClicked;
    // split workingnode into its initialnodes
    bool xClicked;
    // cut out initialsegment from mouse from current workingsegment
    bool cClicked;
    // select color for paintbrushmode
    bool qClicked;
    // refinement of watershed
    bool pClicked;
    // fill holes due to closing
    bool fClicked;
    bool gClicked;

    std::shared_ptr<GraphBase> graphBase;

    double zoomFactor;

    void modifyZoomInAllViewers(double factor);

    void modifyZoom(double factor);

    int predictedSliceIndex;
    std::mutex signalListMutex;
    std::vector<SliceViewerITKSignal *> signalList;

signals:

    void sendStatusMessage(QString);


public slots:

    void setSliceIndex(int proposedSliceIndex);

    void exportCurrentImageToFile(std::string fileName);

    void exportView();
    void exportVideo();


protected:
    void paintEvent(QPaintEvent *event) override;

    void wheelEvent(QWheelEvent *event) override;


    QImage backGroundImage;
    QImage sliceIndicatorImage;
    std::vector<SliceViewer *> linkedViewerList;

    bool verbose;

    // sliceIndex and sliceAxis indicate current slice position
    // e.g. sliceAxis = 0 -> slice through xAxis, 1->yAxis, 2->zAxis
    // sliceIndex=50, sliceAxis=3 -> XY-View at z=50
    // sliceIndex=10, sliceAxis=0 -> YZ-View at z=10
    int sliceIndex, sliceAxis;

    // dimensions of the signals
    int dimX, dimY, dimZ;

    void setUpCustomCursor();

    void getXYZfromPixmapPos(int posX, int posY, int &xOut, int &yOut, int &zOut, bool adjustForZoom = true);

    int old_middle_click_translate_x_pos;
    int old_middle_click_translate_y_pos;
public:
    int getDimX() const;

    int getDimY() const;

    int getDimZ() const;

protected:

    // coordinates of slice indicators
    int indexHorizontalIndicator;
    int indexVerticalIndicator;
    int lastMouseX, lastMouseY, lastMouseZ;

    // total number of signals
    int numberSignals;

    bool linkedSliderSet;
    QSlider *linkedSlider;

    // color of the qpainter pen
    QColor myPenColor;
    // color of the cursor. can be modified when in paintmode.
    QColor cursorColor;
    int myPenWidth;
    QPixmap cursorPixMap;
    QColor outerColor;



private:
    void updateLastMouseXYZAfterSliceInOrDecrement();

};


#endif //HELLOWORLD_SLICEVIEWER_H
