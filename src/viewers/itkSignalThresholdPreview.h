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
    // TODO: handle floats
    SignalImageIndexType index;
    SignalImageSizeType size;
    unsigned int width, height;
    switch (sliceAxis) {
        case 0: {
            index = {sliceIndex, 0, 0};
            size = {1, static_cast<unsigned long>(this->dimY), static_cast<unsigned long>(this->dimZ)};
            width = this->dimZ;
            height = this->dimY;
            break;
        }
        case 1: {
            index = {0, sliceIndex, 0};
            size = {static_cast<unsigned long>(this->dimX), 1, static_cast<unsigned long>(this->dimZ)};
            width = this->dimX;
            height = this->dimZ;
            break;
        }
        case 2: {
            index = {0, 0, sliceIndex};
            size = {static_cast<unsigned long>(this->dimX), static_cast<unsigned long>(this->dimY), 1};
            width = this->dimX;
            height = this->dimY;
            break;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
    SignalImageRegionType region(index, size);

//    std::fill(sliceBuffer.begin(), sliceBuffer.end(), 0);

    typename itk::ImageRegionConstIteratorWithIndex<SignalImageType> it(this->pImage, region);
    it.GoToBegin();

    while (!it.IsAtEnd()) {
        const auto value = it.Get() > thresholdValue;
        const auto &coords = it.GetIndex();
        if (!this->isFloatingPoint) {
            this->checkAndResizeLUT(value);
            quint32 LUTValue = 0;
            try {
                LUTValue = this->LUT.at(value);
            } catch (const std::out_of_range &e) {
                std::cout << "Out of Range error. LUT access." << std::endl;
                std::cout << "Exception: " << e.what() << std::endl;
                std::cout << "Requested value in LUT: " << std::to_string(value) << std::endl;
            }
            try {
                sliceBuffer->at(this->getPixMapIndex(coords, sliceAxis)) = LUTValue;
            } catch (const std::out_of_range &e) {
                std::cout << "Out of Range error. Slicebuffer access." << std::endl;
                std::cout << "Exception: " << e.what() << std::endl;
                std::cout << "index: " << std::to_string(this->getPixMapIndex(coords, sliceAxis)) << std::endl;
                std::cout << "region size: " << std::to_string(size[0]) << " " << std::to_string(size[1]) << " "
                          << std::to_string(size[2]) << std::endl;
                std::cout << "dimX: " << this->dimX << std::endl;
                std::cout << "dimY: " << this->dimY << std::endl;
                std::cout << "dimZ: " << this->dimZ << std::endl;
            }
        } else {
            double normFactorDecimal = 255. / (this->normUpper - this->normLower);
            double normedValue = std::min<double>(std::max<double>(value - this->normLower, 0) * normFactorDecimal, 255);
            auto colorR = static_cast<unsigned char>(normedValue * (qRed(this->mainColor) / 255.));
            auto colorG = static_cast<unsigned char>(normedValue * (qGreen(this->mainColor) / 255.));
            auto colorB = static_cast<unsigned char>(normedValue * (qBlue(this->mainColor) / 255.));
            auto colorA = static_cast<unsigned char>(normedValue * (this->alpha / 255.));
            try {
                sliceBuffer->at(this->getPixMapIndex(coords, sliceAxis)) = qRgba(colorR, colorG, colorB, colorA);
            } catch (const std::out_of_range &e) {
                std::cout << "Out of Range error. Second access." << std::endl;
                std::cout << "Exception: " << e.what() << std::endl;
                std::cout << "index: :" << std::to_string(this->getPixMapIndex(coords, sliceAxis)) << std::endl;
            }
        }
//        std::cout << LUT.at(value) <<  " ";
        ++it;
    }

    return QImage((const unsigned char *) sliceBuffer->data(),
                  width,
                  height,
                  QImage::Format_ARGB32);
}

#endif //SEGMENTCOUPLER_ITKSIGNALTHRESHOLDPREVIEW_H
