#ifndef HELLOWORLD_SLICEVIEWERITKSIGNAL_H
#define HELLOWORLD_SLICEVIEWERITKSIGNAL_H


#include <itkImage.h>
#include "itkSignal.h"
#include <future>

class SliceViewerITKSignal {

public:
    static const int Dimension = 3;

    explicit SliceViewerITKSignal(itkSignalBase *pSignalIn, int sliceIndex = 0, int sliceAxis = 2,
                                  bool verbose = false);


    void calculateImageSize();

    void calculateSliceQImages();

    void prepareNextSliceIndexAsync(unsigned int proposedSliceIndex);

    QImage predictSliceQImage(unsigned int predictedSliceIndexIn);

    void initializeBuffer();

    void setSliceIndex(unsigned int proposedSliceIndex);

    void setSliceAxis(int proposedSliceAxis);

    QString getName();

    int getCurrentSliceWidth();

    int getCurrentSliceHeight();

    unsigned long getDimX();

    unsigned long getDimY();

    unsigned long getDimZ();

    QString getNumberOfXYZAsString(int x, int y, int z);

    unsigned long getPixMapIndex(itk::Index<3> coords);

    bool getIsActive();

    QImage *getAddressSliceQImage();

    bool isValidSliceIndex(unsigned int proposedSliceIndex);

protected:
    itkSignalBase *pSignal;

    // sliceIndex and sliceAxis indicate current slice position
    // e.g. sliceAxis = 0 -> slice through xAxis, 1->yAxis, 2->zAxis
    // sliceIndex=50, sliceAxis=3 -> XY-View at z=50
    // sliceIndex=10, sliceAxis=0 -> YZ-View at z=10
    int sliceIndex, sliceAxis;
    int predictedSliceIndex, predictedSliceAxis;

    // dimensions of the signals
    unsigned long dimX, dimY, dimZ;

    // print debug information
    bool verbose;

    // current QImage at given sliceIndex/sliceAxis for each signal
    // buffer of QImage as to be valid throughout use
    // TODO: push to heap?
    std::shared_ptr<std::vector<quint32>> currentSignalSliceBuffer;
    std::shared_ptr<std::vector<quint32>> predictedSignalSliceBuffer;

    // holds the qImage for given sliceindex/sliceaxis
    QImage sliceQImage;
    std::future<QImage> predictedSliceQImage;
    bool isActive;
};

#endif //HELLOWORLD_SLICEVIEWERITKSIGNAL_H
