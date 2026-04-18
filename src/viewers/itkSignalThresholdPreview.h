#ifndef SEGMENTCOUPLER_ITKSIGNALTHRESHOLDPREVIEW_H
#define SEGMENTCOUPLER_ITKSIGNALTHRESHOLDPREVIEW_H

#include "itkSignal.h"

template<typename dType>
class itkSignalThresholdPreview : public itkSignal<dType> {
    static const int Dimension = 3;
    using SignalImageType = typename itk::Image<dType, Dimension>;
    using SignalImagePointerType = typename itk::Image<dType, Dimension>::Pointer;
    using SignalImageIndexType = typename itk::Image<dType, Dimension>::IndexType;
    using SignalImageSizeType = typename itk::Image<dType, Dimension>::SizeType;
    using SignalImageRegionType = typename itk::Image<dType, Dimension>::RegionType;

    using SegmentIdType = dataType::SegmentIdType;
    using EdgePairIdType = dataType::EdgePairIdType;
    using EdgeNumIdType = dataType::EdgeNumIdType;

public:
    double thresholdValue;

public:
    explicit itkSignalThresholdPreview(SignalImagePointerType pointerToImage, bool verboseIn = false);

    virtual QImage calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis, std::vector<quint32> *sliceBuffer) override;

};

template<typename dType>
itkSignalThresholdPreview<dType>::itkSignalThresholdPreview(SignalImagePointerType pointerToImage, bool verboseIn) :
itkSignal<dType>(pointerToImage, verboseIn){
    thresholdValue = 2000;
}

template<typename dType>
QImage itkSignalThresholdPreview<dType>::calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis,
                                              std::vector<quint32> *sliceBuffer) {
    // attention! slicebuffer has to be valid the whole time the qimage is used, therefore it is passed into the function
    unsigned int width, height;
    const auto dims = slice_geometry::makeDimensions(this->dimX, this->dimY, this->dimZ);
    SignalImageRegionType region = slice_geometry::makeSliceRegion<SignalImageIndexType,
                                                                   SignalImageSizeType,
                                                                   SignalImageRegionType>(sliceIndex,
                                                                                          sliceAxis,
                                                                                          dims,
                                                                                          width,
                                                                                          height);

//    std::fill(sliceBuffer.begin(), sliceBuffer.end(), 0);

    typename itk::ImageRegionConstIteratorWithIndex<SignalImageType> it(this->pImage, region);
    it.GoToBegin();

    const quint32 offColor = qRgba(0, 0, 0, 0);
    const quint32 onColor = qRgba(qRed(this->mainColor),
                                  qGreen(this->mainColor),
                                  qBlue(this->mainColor),
                                  this->alpha);

    while (!it.IsAtEnd()) {
        const bool isAboveThreshold = it.Get() > thresholdValue;
        const auto &coords = it.GetIndex();
        (*sliceBuffer)[this->getPixMapIndex(coords, sliceAxis)] = isAboveThreshold ? onColor : offColor;
        ++it;
    }

    return QImage((const unsigned char *) sliceBuffer->data(),
                  width,
                  height,
                  QImage::Format_ARGB32);
}

#endif //SEGMENTCOUPLER_ITKSIGNALTHRESHOLDPREVIEW_H
