#ifndef HELLOWORLD_ITKSIGNAL_H
#define HELLOWORLD_ITKSIGNAL_H

#include <itkImage.h>
#include <QRgb>
#include <vector>
#include <map>
#include <QTreeWidget>
#include "itkSignalBase.h"
#include "itkImageRegionConstIteratorWithIndex.h"
#include <unordered_map>
#include "src/file_definitions/dataTypes.h"
#include "src/utils/utils.h"
//#include "graphBase.h"

#define LUT_SAVE_ACCESS 1

#ifdef USE_OMP
#include <omp.h>
#endif

#include <iostream>

#include <itkMinimumMaximumImageCalculator.h>
#include <itkImageToHistogramFilter.h>


template<typename dType>
class itkSignal : public itkSignalBase {
// this class should wrap the itk image with information how to display it
// i.e., what color it should be, if it is continuous or categorical, etc.
// slicevieweritk signal will take that information and construct displayable qimages
// for the requested slice index/slice axis
// while you can have many slicevieweritksignals pointing to the same image, i.e.,
// one for each slice axis, the image data and color lookup is managed by the same (this!) class

public:
    //TODO: Add virtual mother class to nicely manage signals
    static const int Dimension = 3;
    using SignalImageType = typename itk::Image<dType, Dimension>;
    using SignalImagePointerType = typename itk::Image<dType, Dimension>::Pointer;
    using SignalImageIndexType = typename itk::Image<dType, Dimension>::IndexType;
    using SignalImageSizeType = typename itk::Image<dType, Dimension>::SizeType;
    using SignalImageRegionType = typename itk::Image<dType, Dimension>::RegionType;

    using SegmentIdType = dataType::SegmentIdType;
    using EdgePairIdType = dataType::EdgePairIdType;
    using EdgeNumIdType = dataType::EdgeNumIdType;

    explicit itkSignal(SignalImagePointerType pointerToImage, bool verboseIn = true);

    itkSignal(SignalImagePointerType pointerToImage, QTreeWidget *motherTreeWidget, size_t signalIndex,
              QString fileName, bool verboseIn = true);

    itkSignal() = default;

    ~itkSignal() override;

    void setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) override;


    void calculateImageSize() override;

    void calculateLUT() override;

    void calculateLUTContinuous(long long dTypeMax) override;

    void calculateLUTCategorical(long long dTypeMax) override;

    void calculateLUTEdge(long long dTypeMax) override;

    void updateLUTEdge(std::set<unsigned int> labelsWithStatusUpdate) override;

    void checkAndResizeLUT(unsigned int value);


    virtual QImage
    calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis, std::vector<quint32> *sliceBuffer) override;

    void setNorm(double lower, double upper) override;

    void setMainColor(int r, int g, int b) override;

    void setMainColor(QColor color) override;

    void setAlpha(unsigned char alphaIn) override;

    void setLUTContinuous() override;

    void setLUTCategorical() override;

    void setLUTEdgeMap(std::unordered_map<EdgeNumIdType, char> *labelToStatus,
                       std::unordered_map<char, std::vector<unsigned char>> *statusToColor) override;

    void setLUTValueToBlack(unsigned int value) override;

    void setLUTValueToTransparent(unsigned int value) override;

    void setIsActive(bool isActiveIn) override;

    double getNormLower() override;

    double getNormUpper() override;

    unsigned int getAlpha() override;

    QString getNumberOfXYZAsString(int x, int y, int z) override;

    unsigned long getPixMapIndex(itk::Index<3> coords, unsigned int sliceAxis);

    QRgb getColor() override;

    unsigned long getDimX() override;

    unsigned long getDimY() override;

    unsigned long getDimZ() override;

    bool getIsActive() override;

//    void isContinuous() override;

    void setName(QString name) override;

    bool isShapeMatched(itkSignalBase *otherSignal);

    void computeExtrema();

    dType getMinimumValue();

    dType getMaximumValue();

    dType minimumValue;
    dType maximumValue;
    dType q01;
    dType q95;

    SignalImagePointerType pImage;

    // holds LUTs for all char continuous data
//    std::vector<quint32> LUT;

    std::vector<unsigned int> blackLUTValues;
    std::vector<unsigned int> transparentLUTValues;


    long unsigned int dimX, dimY, dimZ;

    // default values for initialization of LUTs
    QRgb mainColor;
    unsigned char alpha;
    double normLower, normUpper;

    // decides if a continuous LUT (i.e., intensity of a staining)
    // or a categorical LUT (i.e. segments) should be used
    bool isCategorical;

    // flag if signal should be drawn or not
    bool isActive;

    // useLookUp for calculation
    bool isEdge;

    bool isFloatingPoint;


    std::unordered_map<unsigned int, char> *labelToStatus;
    std::unordered_map<char, std::vector<unsigned char>> *statusToColor;

    bool ROI_set;
    int ROI_fx, ROI_fy, ROI_fz, ROI_tx, ROI_ty, ROI_tz;


    bool verbose;

//    QString name;

};

template<>
inline QString itkSignal<float>::getNumberOfXYZAsString(int x, int y, int z);

template<>
inline QString itkSignal<double>::getNumberOfXYZAsString(int x, int y, int z);


template<typename dType>
itkSignal<dType>::~itkSignal() = default;


template<typename dType>
bool itkSignal<dType>::isShapeMatched(itkSignalBase *otherSignal) {
    bool isMatched = false;
    if (otherSignal != nullptr) {
        int segDimX = otherSignal->getDimX();
        int segDimY = otherSignal->getDimY();
        int segDimZ = otherSignal->getDimZ();
        isMatched = (dimX == segDimX) && (dimY == segDimY) && (dimZ == segDimZ);
    }
    return isMatched;
};


template<typename dType>
void itkSignal<dType>::computeExtrema() {
    double tic = utils::tic();

//    typename itk::MinimumMaximumImageCalculator<SignalImageType>::Pointer minMaxCalc = itk::MinimumMaximumImageCalculator<SignalImageType>::New();
//    // calculate xy
//    unsigned int z_half = dimZ / 2;
//    typename SignalImageType::IndexType startIndexXY = {0, 0, z_half};
//    typename SignalImageType::SizeType sizeXY = {dimX, dimY, 1};
//    typename SignalImageType::RegionType regionXY = {startIndexXY, sizeXY};
//    minMaxCalc->SetImage(pImage);
//    minMaxCalc->SetRegion(regionXY);
//    minMaxCalc->Compute();
//    minimumValue = minMaxCalc->GetMinimum();
//    maximumValue = minMaxCalc->GetMaximum();
//
//    unsigned int y_half = dimY / 2;
//    typename SignalImageType::IndexType startIndexXZ = {0, y_half, 0};
//    typename SignalImageType::SizeType sizeXZ = {dimX, 1, dimZ};
//    typename SignalImageType::RegionType regionXZ = {startIndexXZ, sizeXZ};
//    minMaxCalc->SetRegion(regionXZ);
//    minMaxCalc->Compute();
//    minimumValue = std::min<dType>(minimumValue, minMaxCalc->GetMinimum());
//    maximumValue = std::max<dType>(maximumValue, minMaxCalc->GetMaximum());
//
//    unsigned int x_half = dimX / 2;
//    typename SignalImageType::IndexType startIndexYZ = {x_half, 0, 0};
//    typename SignalImageType::SizeType sizeYZ = {1, dimY, dimZ};
//    typename SignalImageType::RegionType regionYZ = {startIndexYZ, sizeYZ};
//    minMaxCalc->SetRegion(regionYZ);
//    minMaxCalc->Compute();
//    minimumValue = std::min<dType>(minimumValue, minMaxCalc->GetMinimum());
//    maximumValue = std::max<dType>(maximumValue, minMaxCalc->GetMaximum());

//    calculate for whole image
    typename itk::MinimumMaximumImageCalculator<SignalImageType>::Pointer minMaxCalc = itk::MinimumMaximumImageCalculator<SignalImageType>::New();
    minMaxCalc->SetImage(pImage);
    minMaxCalc->SetRegion(pImage->GetLargestPossibleRegion());
    minMaxCalc->Compute();

//    minimumValue = std::max<dType>(minimumValue, minMaxCalc->GetMinimum());
//    maximumValue = std::min<dType>(maximumValue, minMaxCalc->GetMaximum());
    minimumValue = minMaxCalc->GetMinimum();
    maximumValue = minMaxCalc->GetMaximum();


    utils::toc(tic, "duration MinimumMaximumImageCalculator computeExtrema: ");
    std::cout << name.toStdString() << ": min: " << std::to_string(minimumValue) << " " << "max: " << std::to_string(maximumValue) << "\n";
//
//    tic = omp_get_wtime();
//    typedef itk::Statistics::ImageToHistogramFilter<SignalImageType> ImageToHistogramFilterType;
//    typename ImageToHistogramFilterType::Pointer imageHist = itk::Statistics::ImageToHistogramFilter<SignalImageType>::New();
//    typename ImageToHistogramFilterType::HistogramSizeType size(1);
//    size[0] = 1000;
//    imageHist->SetHistogramSize(size);
//    typename ImageToHistogramFilterType::HistogramSizeType minimumVal(1);
//    typename ImageToHistogramFilterType::HistogramSizeType maximumVal(1);
//    imageHist->SetHistogramBinMinimum(minimumVal);
//    imageHist->SetHistogramBinMaximum(maximumVal);
//    imageHist->SetInput(pImage);
//    imageHist->SetMarginalScale(10);
//    imageHist->Update();
//    typename ImageToHistogramFilterType::HistogramType::Pointer calculatedHistogram = imageHist->GetOutput();
//    q01 = static_cast<dType>(calculatedHistogram->Quantile(0, 0.01));
//    q95 = static_cast<dType>(calculatedHistogram->Quantile(0, 0.95));
//    std::cout << "1% quantile: " << std::to_string(minimumValue) << " " << "95% quantile: "
//              << std::to_string(maximumValue) << "\n";
//    toc = omp_get_wtime();
//    if (verbose) { std::cout << "duration itkSignal ImageToHistogramFilter: " << toc - tic << std::endl; }
};


template<typename dType>
dType itkSignal<dType>::getMinimumValue() {
    return minimumValue;
};

template<typename dType>
dType itkSignal<dType>::getMaximumValue() {
    return maximumValue;
};

template<typename dType>
itkSignal<dType>::itkSignal(SignalImagePointerType pointerToImage, bool verboseIn):
        pImage{pointerToImage}, verbose{verboseIn} {
    // sets dimX, dimY, dimZ

    calculateImageSize();

    // set default LUT values
    mainColor = qRgba(255, 255, 255, 255);
    computeExtrema();
    normLower = minimumValue;
    normUpper = maximumValue;
    alpha = 150;
    isCategorical = false;
    isEdge = false;
    name = "";
    isActive = true;

    ROI_set = false;
    ROI_fx = -1;
    ROI_fy = -1;
    ROI_fz = -1;
    ROI_tx = -1;
    ROI_ty = -1;
    ROI_tz = -1;

    // calculate LUT
    calculateLUT();
}

template<typename dType>
itkSignal<dType>::itkSignal(SignalImagePointerType pointerToImage, QTreeWidget *motherTreeWidget, size_t signalIndex,
                            QString fileName, bool verboseIn) {
    pImage = pointerToImage;
    verbose = verboseIn;
    // sets dimX, dimY, dimZ
    calculateImageSize();

    // set default LUT values
    mainColor = qRgba(255, 255, 255, 255);
    computeExtrema();
    normLower = minimumValue;
//    normLower = q01;
//    normUpper = q95;
    normUpper = maximumValue;
    alpha = 150;
    isCategorical = false;
    isEdge = false;
    name = fileName;
    isActive = true;

    ROI_set = false;
    ROI_fx = -1;
    ROI_fy = -1;
    ROI_fz = -1;
    ROI_tx = -1;
    ROI_ty = -1;
    ROI_tz = -1;

    setupTreeWidget(motherTreeWidget, signalIndex);

    // calculate LUT
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setName(QString nameIn) {
    std::cout << "Stackname: " << nameIn.toStdString() << "\n";
    name = nameIn;
}


template<typename dType>
void itkSignal<dType>::setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) {

    QImage colorIcon = QImage(30, 30, QImage::Format_RGBA8888);
    colorIcon.fill(mainColor);

    auto treeWidget = new QTreeWidgetItem();
    treeWidget->setText(0, name);
    treeWidget->setText(1, "active");
    treeWidget->setIcon(1, QPixmap::fromImage(colorIcon));
    treeWidget->setCheckState(0, Qt::CheckState::Checked);


    auto colorWidget = new QTreeWidgetItem();
    colorWidget->setIcon(1, QPixmap::fromImage(colorIcon));
    colorWidget->setText(0, "Color");
    std::string colorString = std::to_string(qRed(mainColor))
                              + " " + std::to_string(qGreen(mainColor))
                              + " " + std::to_string(qBlue(mainColor));
    colorWidget->setText(1, QString::fromStdString(colorString));
    treeWidget->addChild(colorWidget);

    auto normWidget = new QTreeWidgetItem();
    normWidget->setText(0, "Norm");
    QString normString = QString("%1").arg(normLower) + " " + QString("%1").arg(normUpper);
    normWidget->setText(1, normString);
    treeWidget->addChild(normWidget);

    auto alphaWidget = new QTreeWidgetItem();
    alphaWidget->setText(0, "Alpha");
    std::string alphaString = std::to_string(alpha);
    alphaWidget->setText(1, QString::fromStdString(alphaString));
    treeWidget->addChild(alphaWidget);

    auto signalIndexWidget = new QTreeWidgetItem();
    signalIndexWidget->setText(0, "SignalIndex");
    std::string indexString = std::to_string(signalIndex);
    signalIndexWidget->setText(1, QString::fromStdString(indexString));
    treeWidget->addChild(signalIndexWidget);


    auto dataTypeWidget = new QTreeWidgetItem();
    dataTypeWidget->setText(0, "data type");
    std::string dataTypeString = "";
    std::cout << typeid(dType).name() << std::endl;
    if ((std::strcmp(typeid(dType).name(), "s") == 0) || (std::strcmp(typeid(dType).name(), "short") == 0)) {
        dataTypeString = "short";
    } else if ((std::strcmp(typeid(dType).name(), "h") == 0) || (std::strcmp(typeid(dType).name(), "char") == 0)) {
        dataTypeString = "char";
    } else if ((std::strcmp(typeid(dType).name(), "t") == 0) ||
               (std::strcmp(typeid(dType).name(), "unsigned short") == 0)) {
        dataTypeString = "unsigned short";
    } else if ((std::strcmp(typeid(dType).name(), "j") == 0) ||
               (std::strcmp(typeid(dType).name(), "unsigned int") == 0)) {
        dataTypeString = "unsigned int";
    } else if ((std::strcmp(typeid(dType).name(), "f") == 0) || (std::strcmp(typeid(dType).name(), "float") == 0)) {
        dataTypeString = "float";
    } else if (std::strcmp(typeid(dType).name(), "unsigned char") == 0) {
        dataTypeString = "char"; //TODO: this should be unsigned char, check downstream
    } else if (std::strcmp(typeid(dType).name(), "unsigned short") == 0) {
        dataTypeString = "unsigned short";
    } else if (std::strcmp(typeid(dType).name(), "int") == 0) {
        dataTypeString = "int";
    } else {
        std::cout << typeid(dType).name() << std::endl;
        throw std::logic_error("itkSignal:setupTreeWidget Unknown type!");
    }
    dataTypeWidget->setText(1, QString::fromStdString(dataTypeString));
    treeWidget->addChild(dataTypeWidget);


    motherTreeWidget->addTopLevelItem(treeWidget);
    motherTreeWidget->update();
}


template<typename dType>
void itkSignal<dType>::calculateImageSize() {
    auto &size = pImage->GetLargestPossibleRegion().GetSize();
    if (verbose) { std::cout << size << std::endl; }
    dimX = size[0];
    dimY = size[1];
    dimZ = size[2];
}

template<typename dType>
void itkSignal<dType>::calculateLUT() {
    long long dTypeMax;
//  if floating point
    if ((std::is_same<dType, float>::value) | (std::is_same<dType, double>::value)) {
        if (verbose) {
            std::cout << "Read signal is floating point!\n";
        }
        isFloatingPoint = true;
        dTypeMax = maximumValue;
    } else {
        isFloatingPoint = false;
        long long upperLimitLUT = 10 * maximumValue; // limit number of maximum LUT values
        // TODO: Automatically resize if requested LUTvalue is not in the array. This may cause errors downstream ...

        if (std::numeric_limits<dType>::max() < std::numeric_limits<int>::max()) {
            dTypeMax = std::min<unsigned long long>(upperLimitLUT, std::numeric_limits<dType>::max()); // issue: cant add +1 because it would result in overflow for long longs ...
        } else
            dTypeMax = std::min<long long>(upperLimitLUT, std::numeric_limits<dType>::max());
    }

    std::cout << "Calculating LUTs for " << dTypeMax << " values.\n";

    if (size_t(dTypeMax) > LUT.size()) {
        LUT.resize(dTypeMax);
    }

    if (verbose) { std::cout << "LUT Max Val.: " << dTypeMax << std::endl; }
    if (isCategorical) {
        calculateLUTCategorical(dTypeMax);
    } else if (isEdge) {
        calculateLUTEdge(dTypeMax);
    } else {
        calculateLUTContinuous(dTypeMax);
    }
}

template<typename dType>
void itkSignal<dType>::calculateLUTContinuous(long long) {
    //TODO: Handle negative continous luts
    //TODO: Handle Floats
    if (verbose) { std::cout << "Continous LUT" << std::endl; }
    if (verbose) { std::cout << "Norm min.: " << normLower << " Norm max.: " << normUpper << std::endl; }

    double normFactorDecimal = 255. / (normUpper - normLower);
    if (verbose) { std::cout << "normFactor: " << normFactorDecimal << std::endl; }

    for (size_t i = 0; i < LUT.size(); ++i) {
        double normedValue = std::min<double>(std::max<double>(i - normLower, 0) * normFactorDecimal, 255);
        auto colorR = static_cast<unsigned char>(normedValue * (qRed(mainColor) / 255.));
        auto colorG = static_cast<unsigned char>(normedValue * (qGreen(mainColor) / 255.));
        auto colorB = static_cast<unsigned char>(normedValue * (qBlue(mainColor) / 255.));
        auto colorA = static_cast<unsigned char>(normedValue * (alpha / 255.));
        LUT.at(static_cast<unsigned long>(i)) = qRgba(colorR, colorG, colorB, colorA);
//        if (verbose) {
//            std::cout << i << " " << normedValue << " " << (int) colorR << " " << (int) colorG
//                      << " " << (int) colorB << " " << (int) colorA << std::endl;
//        }
    }
}

template<typename dType>
void itkSignal<dType>::calculateLUTCategorical(long long) {
    if (verbose) { std::cout << "Categorical LUT" << std::endl; }
    for (size_t i = 0; i < LUT.size(); ++i) {
        auto colorR = (std::rand() % 255);
        auto colorG = (std::rand() % 255);
        auto colorB = (std::rand() % 255);
        auto colorA = alpha;
        LUT.at(static_cast<unsigned long>(i)) = qRgba(colorR, colorG, colorB, colorA);
//        if (verbose) {
//            std::cout << i << " " << (int) colorR << " " << (int) colorG
//                      << " " << (int) colorB << " " << (int) colorA << std::endl;
//        }
    }
    for (auto val: blackLUTValues) {
        LUT.at(val) = qRgba(0, 0, 0, 255);
    }
    for (auto val: transparentLUTValues) {
        LUT.at(val) = qRgba(0, 0, 0, 0);
    }

}

template<typename dType>
void itkSignal<dType>::setMainColor(int r, int g, int b) {
    mainColor = qRgba(r, g, b, 255);
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setMainColor(QColor color) {
    mainColor = color.rgba();
    calculateLUT();
}


template<typename dType>
void itkSignal<dType>::setNorm(double lower, double upper) {
    normLower = lower;
    normUpper = upper;
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setAlpha(unsigned char alphaIn) {
    alpha = alphaIn;
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setLUTContinuous() {
    isCategorical = false;
    calculateLUT();
}


template<typename dType>
void itkSignal<dType>::setLUTCategorical() {
    isCategorical = true;
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setLUTValueToBlack(unsigned int value) {
    checkAndResizeLUT(value);
    std::cout << "Setting " << value << " to be displayed as black! \n";
    LUT.at(value) = qRgba(0, 0, 0, 255);
    blackLUTValues.push_back(value);
}


template<typename dType>
void itkSignal<dType>::checkAndResizeLUT(unsigned int value) {
    if (value >= LUT.size()) {
        LUT.resize((value + 1) * 2);
        calculateLUT();
        std::cout << "Resizing LUT to fit index " << value << ". New size: " << (value + 1) * 2 << "\n";
    }
}


template<typename dType>
void itkSignal<dType>::setLUTValueToTransparent(unsigned int value) {
    checkAndResizeLUT(value);
    std::cout << "Setting " << value << " to be displayed as transparent! \n";
    LUT.at(value) = qRgba(0, 0, 0, 0);
    transparentLUTValues.push_back(value);
}


template<typename dType>
void itkSignal<dType>::setIsActive(bool isActiveIn) {
    isActive = isActiveIn;
}


template<typename dType>
QRgb itkSignal<dType>::getColor() {
    return mainColor;
}

template<typename dType>
bool itkSignal<dType>::getIsActive() {
    return isActive;
}

template<typename dType>
void itkSignal<dType>::calculateLUTEdge(long long) {
    std::fill(LUT.begin(), LUT.end(), qRgba(0, 0, 0, 0));
    for (auto &labelMapping: *labelToStatus) {
        std::vector<unsigned char> colorVec = statusToColor->at(labelMapping.second);
        checkAndResizeLUT(labelMapping.first);
        LUT.at(labelMapping.first) = qRgba(colorVec.at(0), colorVec.at(1), colorVec.at(2), alpha);
    }
}

template<typename dType>
void itkSignal<dType>::updateLUTEdge(std::set<unsigned int> labelsWithStatusUpdate) {
    for (auto &idOfEdgeWithChangedStatus: labelsWithStatusUpdate) {
//        std::cout << "edge id to update: " << idOfEdgeWithChangedStatus;
//        std::cout << "LUT size: " << LUT.size();
//        std::cout << "statusToColor size: " << statusToColor->size();
        char newStatusOfEdgeWithChangedStatus = labelToStatus->at(idOfEdgeWithChangedStatus);
        std::vector<unsigned char> colorVec = statusToColor->at(newStatusOfEdgeWithChangedStatus);
        checkAndResizeLUT(idOfEdgeWithChangedStatus);
        LUT.at(idOfEdgeWithChangedStatus) = qRgba(colorVec.at(0), colorVec.at(1), colorVec.at(2), alpha);
    }
}

template<typename dType>
void itkSignal<dType>::setLUTEdgeMap(std::unordered_map<unsigned int, char> *labelToStatusIn,
                                     std::unordered_map<char, std::vector<unsigned char>> *statusToColorIn) {
    isEdge = true;
    labelToStatus = labelToStatusIn;
    statusToColor = statusToColorIn;
}

template<typename dType>
double itkSignal<dType>::getNormUpper() {
    return normUpper;
}


template<typename dType>
double itkSignal<dType>::getNormLower() {
    return normLower;
}


template<typename dType>
unsigned int itkSignal<dType>::getAlpha() {
    return alpha;
}

template<typename dType>
QImage itkSignal<dType>::calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis,
                                              std::vector<quint32> *sliceBuffer) {
    // attention! slicebuffer has to be valid the whole time the qimage is used, therefore it is passed into the function
    // TODO: handle floats
    SignalImageIndexType index;
    SignalImageSizeType size;
    unsigned int width, height;

    switch (sliceAxis) {
        case 0: {
            index = {sliceIndex, 0, 0};
            size = {1, static_cast<unsigned long>(dimY), static_cast<unsigned long>(dimZ)};
            width = dimZ;
            height = dimY;
            break;
        }
        case 1: {
            index = {0, sliceIndex, 0};
            size = {static_cast<unsigned long>(dimX), 1, static_cast<unsigned long>(dimZ)};
            width = dimX;
            height = dimZ;
            break;
        }
        case 2: {
            index = {0, 0, sliceIndex};
            size = {static_cast<unsigned long>(dimX), static_cast<unsigned long>(dimY), 1};
            width = dimX;
            height = dimY;
            break;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
    SignalImageRegionType region(index, size);

//    std::fill(sliceBuffer.begin(), sliceBuffer.end(), 0);

    typename itk::ImageRegionConstIteratorWithIndex<SignalImageType> it(pImage, region);
    it.GoToBegin();
//    TODO: is checkAndResizeLut needed?


    const dType* imageBuffer = pImage->GetBufferPointer();
    quint32* sliceBufferPtr = sliceBuffer->data();
    if (!isFloatingPoint) {
        if (sliceAxis == 0) {  // x slices
            for (unsigned long z = 0; z < dimZ; ++z) { // Loop over depth (z-axis)
                unsigned long zOffset = z * dimX * dimY; // Start of the z-plane in the buffer

                for (unsigned long y = 0; y < dimY; ++y) { // Loop over height (y-axis)
                    unsigned long imageOffset = sliceIndex + (y * dimX) + zOffset;

                    // Access the voxel at (sliceIndex, y, z)
                    dType value = imageBuffer[imageOffset];

                    // Write to the sliceBuffer at (z, y)
                    unsigned long sliceBufferIndex = z + (y * dimZ);
#if LUT_SAVE_ACCESS
                    sliceBufferPtr[sliceBufferIndex] = LUT.at(value);
#else
                    sliceBufferPtr[sliceBufferIndex] = LUT[value];
#endif
                }
            }

//            while (!it.IsAtEnd()) {
//                const auto value = it.Get();
//                const auto &coords = it.GetIndex();
////                checkAndResizeLUT(value);
//                (*sliceBuffer)[coords[2] + coords[1] * dimZ] = LUT[value];
//                ++it;
//            }
        } else if (sliceAxis == 1) {
            unsigned long sliceSize = dimX * dimZ;
            unsigned long bufferIndex = 0;

            for (unsigned long z = 0; z < dimZ; ++z) {
                unsigned long zOffset = z * dimX * dimY;
                unsigned long yOffset = sliceIndex * dimX;
                for (unsigned long x = 0; x < dimX; ++x) {
                    unsigned long imageOffset = x + yOffset + zOffset;
                    dType value = imageBuffer[imageOffset];
#if LUT_SAVE_ACCESS
                    sliceBufferPtr[bufferIndex++] = LUT.at(value);
#else
                    sliceBufferPtr[bufferIndex++] = LUT[value];
#endif
                }
            }
//            while (!it.IsAtEnd()) {
//                const auto value = it.Get();
//                const auto &coords = it.GetIndex();
////                checkAndResizeLUT(value);
//                (*sliceBuffer)[coords[0] + coords[2] * dimX] = LUT[value];
//                ++it;
//            }
        } else if (sliceAxis == 2) {
            unsigned long sliceSize = dimX * dimY;
            unsigned long imageOffset = sliceIndex * dimX * dimY;

            const dType* sliceData = imageBuffer + imageOffset;
            for (unsigned long idx = 0; idx < sliceSize; ++idx) {
                dType value = sliceData[idx];
#if LUT_SAVE_ACCESS
                sliceBufferPtr[idx] = LUT.at(value);
#else
                sliceBufferPtr[idx] = LUT[value];
#endif
            }
//
//            while (!it.IsAtEnd()) {
//                const auto value = it.Get();
//                const auto &coords = it.GetIndex();
////                checkAndResizeLUT(value);
//                (*sliceBuffer)[coords[0] + coords[1] * dimX] = LUT[value];
//                ++it;
//            }
        }
    } else {
            while (!it.IsAtEnd()) {
                const auto value = it.Get();
                const auto &coords = it.GetIndex();
                double normFactorDecimal = 255. / (normUpper - normLower);
                double normedValue = std::min<double>(std::max<double>(value - normLower, 0) * normFactorDecimal, 255);
                auto colorR = static_cast<unsigned char>(normedValue * (qRed(mainColor) / 255.));
                auto colorG = static_cast<unsigned char>(normedValue * (qGreen(mainColor) / 255.));
                auto colorB = static_cast<unsigned char>(normedValue * (qBlue(mainColor) / 255.));
                auto colorA = static_cast<unsigned char>(normedValue * (alpha / 255.));
                try {
                    sliceBuffer->at(getPixMapIndex(coords, sliceAxis)) = qRgba(colorR, colorG, colorB, colorA);
                } catch (const std::out_of_range &e) {
                    std::cout << "Out of Range error. Second access." << std::endl;
                    std::cout << "Exception: " << e.what() << std::endl;
                    std::cout << "index: " << std::to_string(getPixMapIndex(coords, sliceAxis)) << std::endl;
                }
                ++it;
            }
        }

//    while (!it.IsAtEnd()) {
//        const auto value = it.Get();
//        const auto &coords = it.GetIndex();
//        if (!isFloatingPoint) {
//            checkAndResizeLUT(value);
//            (*sliceBuffer)[getPixMapIndex(coords, sliceAxis)] = LUT[value];

//           save method for debugging reasons below. top doesnt do rangechecks etc and is faster
//            quint32 LUTValue = 0;
//            try {
//                LUTValue = LUT.at(value);
//            } catch (const std::out_of_range &e) {
//                std::cout << "Out of Range error. LUT access." << std::endl;
//                std::cout << "Exception: " << e.what() << std::endl;
//                std::cout << "Requested value in LUT: " << std::to_string(value) << std::endl;
//            }
//            try {
//                sliceBuffer->at(getPixMapIndex(coords, sliceAxis)) = LUTValue;
//            } catch (const std::out_of_range &e) {
//                std::cout << "Out of Range error. Slicebuffer access." << std::endl;
//                std::cout << "Exception: " << e.what() << std::endl;
//                std::cout << "index: " << std::to_string(getPixMapIndex(coords, sliceAxis)) << std::endl;
//                std::cout << "region size: " << std::to_string(size[0]) << " " << std::to_string(size[1]) << " "
//                          << std::to_string(size[2]) << std::endl;
//                std::cout << "dimX: " << dimX << std::endl;
//                std::cout << "dimY: " << dimY << std::endl;
//                std::cout << "dimZ: " << dimZ << std::endl;
//            }
//        } else {
//            double normFactorDecimal = 255. / (normUpper - normLower);
//            double normedValue = std::min<double>(std::max<double>(value - normLower, 0) * normFactorDecimal, 255);
//            auto colorR = static_cast<unsigned char>(normedValue * (qRed(mainColor) / 255.));
//            auto colorG = static_cast<unsigned char>(normedValue * (qGreen(mainColor) / 255.));
//            auto colorB = static_cast<unsigned char>(normedValue * (qBlue(mainColor) / 255.));
//            auto colorA = static_cast<unsigned char>(normedValue * (alpha / 255.));
//            try {
//                sliceBuffer->at(getPixMapIndex(coords, sliceAxis)) = qRgba(colorR, colorG, colorB, colorA);
//            } catch (const std::out_of_range &e) {
//                std::cout << "Out of Range error. Second access." << std::endl;
//                std::cout << "Exception: " << e.what() << std::endl;
//                std::cout << "index: " << std::to_string(getPixMapIndex(coords, sliceAxis)) << std::endl;
//            }
//        }
////        std::cout << LUT.at(value) <<  " ";
//        ++it;
//    }

    return QImage((const unsigned char *) sliceBuffer->data(),
                  width,
                  height,
                  QImage::Format_ARGB32);
}


template<typename dType>
inline
unsigned long itkSignal<dType>::getPixMapIndex(itk::Index<3> coords, unsigned int sliceAxis) {
    switch (sliceAxis) {
        case 0: {
            return coords[2] + coords[1] * dimZ;
        }
        case 1: {
            return coords[0] + coords[2] * dimX;
        }
        case 2: {
            return coords[0] + coords[1] * dimX;
        }
        default:
            throw (std::logic_error("sliceAxis not implemented!"));
    }
}

template<typename dType>
QString itkSignal<dType>::getNumberOfXYZAsString(int x, int y, int z) {
    QString value2String = "";
    if (x >= 0 && y >= 0 && z >= 0) {
        if (x < static_cast<int>(dimX) && y < static_cast<int>(dimY) && z < static_cast<int>(dimZ)) {
            auto value = static_cast<long int>(pImage->GetPixel({x, y, z}));
            value2String = QString("%1").arg(value);
        }
    }
    return value2String;
}

template<>
inline QString itkSignal<float>::getNumberOfXYZAsString(int x, int y, int z) {
    QString value2String = "";
    if (x >= 0 && y >= 0 && z >= 0) {
        if (x < static_cast<int>(dimX) && y < static_cast<int>(dimY) && z < static_cast<int>(dimZ)) {
            auto value = static_cast<long int>(pImage->GetPixel({x, y, z}));
            value2String = QString::number(value, 'g', 3);
        }
    }
    return value2String;
}

template<>
inline QString itkSignal<double>::getNumberOfXYZAsString(int x, int y, int z) {
    QString value2String = "";
    if (x >= 0 && y >= 0 && z >= 0) {
        if (x < static_cast<int>(dimX) && y < static_cast<int>(dimY) && z < static_cast<int>(dimZ)) {
            auto value = static_cast<long int>(pImage->GetPixel({x, y, z}));
            value2String = QString::number(value, 'g', 3);
        }
    }
    return value2String;
}


template<typename dType>
unsigned long itkSignal<dType>::getDimX() {
    return dimX;
}

template<typename dType>
unsigned long itkSignal<dType>::getDimY() {
    return dimY;
}

template<typename dType>
unsigned long itkSignal<dType>::getDimZ() {
    return dimZ;
}


#endif //HELLOWORLD_ITKSIGNAL_H
