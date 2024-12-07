#ifndef HELLOWORLD_ITKSIGNALBASE_H
#define HELLOWORLD_ITKSIGNALBASE_H

#include <itkImage.h>
#include <QRgb>
#include <vector>
#include <map>
#include <unordered_map>
#include <QTreeWidget>


// abstract class for viewers of itk signals
class itkSignalBase {

public:

    virtual ~itkSignalBase() {};

    virtual void setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) = 0;


    virtual void calculateImageSize() = 0;

    virtual void calculateLUT() = 0;

    virtual void calculateLUTContinuous(long long dTypeMax) = 0;

    virtual void calculateLUTCategorical(long long dTypeMax) = 0;

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
