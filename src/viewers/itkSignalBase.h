#ifndef HELLOWORLD_ITKSIGNALBASE_H
#define HELLOWORLD_ITKSIGNALBASE_H

#include <itkImage.h>
#include <itkImageBase.h>
#include <QRgb>
#include <vector>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <QTreeWidget>


namespace slice_geometry {

struct Dimensions3D {
    unsigned long x;
    unsigned long y;
    unsigned long z;
};

inline Dimensions3D makeDimensions(unsigned long dimX, unsigned long dimY, unsigned long dimZ) {
    return {dimX, dimY, dimZ};
}

inline unsigned long sliceLimit(unsigned int sliceAxis, const Dimensions3D &dims) {
    switch (sliceAxis) {
        case 0:
            return dims.x;
        case 1:
            return dims.y;
        case 2:
            return dims.z;
        default:
            throw std::logic_error("sliceAxis not implemented!");
    }
}

inline int sliceWidth(unsigned int sliceAxis, const Dimensions3D &dims) {
    switch (sliceAxis) {
        case 0:
            return static_cast<int>(dims.z);
        case 1:
        case 2:
            return static_cast<int>(dims.x);
        default:
            throw std::logic_error("sliceAxis not implemented!");
    }
}

inline int sliceHeight(unsigned int sliceAxis, const Dimensions3D &dims) {
    switch (sliceAxis) {
        case 0:
        case 2:
            return static_cast<int>(dims.y);
        case 1:
            return static_cast<int>(dims.z);
        default:
            throw std::logic_error("sliceAxis not implemented!");
    }
}

inline unsigned long pixmapIndex(const itk::Index<3> &coords, unsigned int sliceAxis, const Dimensions3D &dims) {
    switch (sliceAxis) {
        case 0:
            return coords[2] + coords[1] * dims.z;
        case 1:
            return coords[0] + coords[2] * dims.x;
        case 2:
            return coords[0] + coords[1] * dims.x;
        default:
            throw std::logic_error("sliceAxis not implemented!");
    }
}

template<typename IndexType, typename SizeType, typename RegionType>
inline RegionType makeSliceRegion(unsigned int sliceIndex,
                                  unsigned int sliceAxis,
                                  const Dimensions3D &dims,
                                  unsigned int &width,
                                  unsigned int &height) {
    IndexType index;
    SizeType size;
    switch (sliceAxis) {
        case 0:
            index = {sliceIndex, 0, 0};
            size = {1, dims.y, dims.z};
            width = static_cast<unsigned int>(dims.z);
            height = static_cast<unsigned int>(dims.y);
            break;
        case 1:
            index = {0, sliceIndex, 0};
            size = {dims.x, 1, dims.z};
            width = static_cast<unsigned int>(dims.x);
            height = static_cast<unsigned int>(dims.z);
            break;
        case 2:
            index = {0, 0, sliceIndex};
            size = {dims.x, dims.y, 1};
            width = static_cast<unsigned int>(dims.x);
            height = static_cast<unsigned int>(dims.y);
            break;
        default:
            throw std::logic_error("sliceAxis not implemented!");
    }

    return RegionType(index, size);
}

} // namespace slice_geometry


// abstract class for viewers of itk signals
class itkSignalBase {

public:

    virtual ~itkSignalBase() {};

    virtual itk::ImageBase<3>::Pointer getImageBase() const = 0;

    virtual void updateImage(itk::ImageBase<3>::Pointer newImage) = 0;

    virtual void setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) = 0;


    virtual void calculateImageSize() = 0;

    virtual void calculateLUT() = 0;

    virtual void calculateLUTContinuous(long long dTypeMax) = 0;

    virtual void calculateLUTCategorical(long long dTypeMax, size_t startIndex = 0) = 0;

    virtual void calculateLUTEdge(long long dTypeMax) = 0;

    virtual void updateLUTEdge(std::set<unsigned int> labelsWithStatusUpdate) = 0;

    virtual void setNorm(double lower, double upper) = 0;

    virtual void setMainColor(int r, int g, int b) = 0;

    virtual void setMainColor(QColor color) = 0;

    virtual void setAlpha(unsigned char alphaIn) = 0;

    virtual void setLUTContinuous() = 0;

    virtual void setLUTCategorical() = 0;

    virtual void setLUTEdgeMap(std::unordered_map<unsigned int, char> *labelToStatus,
                               std::unordered_map<char, std::vector<unsigned char>> *statusToColor) = 0;

    virtual void setLUTValueToBlack(unsigned int value) = 0;

    virtual void setLUTValueToTransparent(unsigned int value) = 0;

    virtual void randomizeCategoricalLUT() = 0;

    virtual void setIsActive(bool isActiveIn) = 0;

    virtual QImage
    calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis, std::vector<quint32> *sliceBuffer) = 0;

    virtual QString getNumberOfXYZAsString(int x, int y, int z) = 0;
    // todo: make function return string? or double for xyz position for printout
    // or both? string better for printing, double would give universal useability, independent of data type

    virtual double getNormLower() = 0;

    virtual double getNormUpper() = 0;

    virtual unsigned int getAlpha() = 0;

    virtual QRgb getColor() = 0;

    virtual unsigned long getDimX() = 0;

    virtual unsigned long getDimY() = 0;

    virtual unsigned long getDimZ() = 0;

    virtual bool getIsActive() = 0;

//    virtual void isContinuous() = 0;

    virtual void setName(QString name) = 0;

    // holds LUTs for all char continuous data
    std::vector<quint32> LUT;

    QString name;

};


#endif //HELLOWORLD_ITKSIGNALBASE_H
