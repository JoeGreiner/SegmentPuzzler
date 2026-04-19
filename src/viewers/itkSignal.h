#ifndef HELLOWORLD_ITKSIGNAL_H
#define HELLOWORLD_ITKSIGNAL_H

#include <itkImage.h>
#include <QRgb>
#include <vector>
#include <map>
#include <QDateTime>
#include <QTreeWidget>
#include "itkSignalBase.h"
#include "itkImageRegionConstIteratorWithIndex.h"
#include <unordered_map>
#include "src/file_definitions/dataTypes.h"
#include "src/qtUtils/SignalTreeWidgetUtils.h"
#include "src/utils/AppLogger.h"
#include "src/utils/utils.h"
//#include "graphBase.h"

#define LUT_SAVE_ACCESS 1

#ifdef USE_OMP
#include <omp.h>
#endif

#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

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

    void updateImage(itk::ImageBase<3>::Pointer newImage) override {
        pImage = dynamic_cast<SignalImageType *>(newImage.GetPointer());
        if (pImage.IsNull()) {
            throw std::runtime_error("itkSignal::updateImage: Invalid image type.");
        }
        calculateImageSize();
        computeExtrema();
        calculateLUT();
    }


    void calculateImageSize() override;

    void calculateLUT() override;

    void calculateLUTContinuous(long long dTypeMax) override;

    void calculateLUTCategorical(long long dTypeMax, size_t startIndex) override;

    void calculateLUTEdge(long long dTypeMax) override;

    void updateLUTEdge(std::set<unsigned int> labelsWithStatusUpdate) override;

    void checkAndResizeLUT(unsigned int value);

    // Re-randomizes all categorical LUT entries from scratch.
    void randomizeCategoricalLUT() override;


    virtual QImage
    calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis, std::vector<quint32> *sliceBuffer) override;

    void setNorm(double lower, double upper) override;

    bool computeNextAutoContrastRange(double &lower, double &upper) override;

    void resetAutoContrastState() override;

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

    double getMinimumValueAsDouble() const override { return static_cast<double>(minimumValue); }

    double getMaximumValueAsDouble() const override { return static_cast<double>(maximumValue); }

    unsigned int getAlpha() override;

    QString getNumberOfXYZAsString(int x, int y, int z) override;

    unsigned long getPixMapIndex(itk::Index<3> coords, unsigned int sliceAxis);

    QRgb getColor() override;

    QString getDisplayDataTypeName() const override { return displayDataTypeName(); }

    bool supportsNormControl() const override { return !isCategorical && !isEdge; }

    bool usesCategoricalLUT() const override { return isCategorical && !isEdge; }

    bool usesEdgeStatusColors() const override { return isEdge; }

    unsigned long getDimX() override;

    unsigned long getDimY() override;

    unsigned long getDimZ() override;

    bool getIsActive() override;

//    void isContinuous() override;

    void setName(QString name) override;

    itk::ImageBase<3>::Pointer getImageBase() const override { return pImage; }

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
    QRgb mainColor = qRgba(255, 255, 255, 255);
    unsigned char alpha = 150;
    double normLower = 0.0;
    double normUpper = 0.0;

    // decides if a continuous LUT (i.e., intensity of a staining)
    // or a categorical LUT (i.e. segments) should be used
    bool isCategorical = false;

    // flag if signal should be drawn or not
    bool isActive = true;

    // useLookUp for calculation
    bool isEdge = false;

    // true once the LUT has been built as categorical at least once;
    // false forces a full rebuild when first switching from continuous to categorical
    bool categoricalLUTInitialized = false;

    int autoContrastThreshold = 0;


    std::unordered_map<unsigned int, char> *labelToStatus = nullptr;
    std::unordered_map<char, std::vector<unsigned char>> *statusToColor = nullptr;

    bool ROI_set = false;
    int ROI_fx = -1;
    int ROI_fy = -1;
    int ROI_fz = -1;
    int ROI_tx = -1;
    int ROI_ty = -1;
    int ROI_tz = -1;


    bool verbose;

//    QString name;

private:
    static QString displayDataTypeName();

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
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();

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

    SP_LOG_DEBUG("viewer.render",
                 QStringLiteral("Computed extrema in %1 ms min=%2 max=%3")
                     .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs)
                     .arg(minimumValue)
                     .arg(maximumValue));
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
QString itkSignal<dType>::displayDataTypeName() {
    if constexpr (std::is_same_v<dType, short>) {
        return QStringLiteral("short");
    } else if constexpr (std::is_same_v<dType, unsigned char>) {
        return QStringLiteral("unsigned char");
    } else if constexpr (std::is_same_v<dType, unsigned short>) {
        return QStringLiteral("unsigned short");
    } else if constexpr (std::is_same_v<dType, unsigned int>) {
        return QStringLiteral("unsigned int");
    } else if constexpr (std::is_same_v<dType, int>) {
        return QStringLiteral("int");
    } else if constexpr (std::is_same_v<dType, unsigned long>) {
        return QStringLiteral("unsigned long");
    } else if constexpr (std::is_same_v<dType, long>) {
        return QStringLiteral("long");
    } else if constexpr (std::is_same_v<dType, unsigned long long>) {
        return QStringLiteral("unsigned long long");
    } else if constexpr (std::is_same_v<dType, long long>) {
        return QStringLiteral("long long");
    } else if constexpr (std::is_same_v<dType, float>) {
        return QStringLiteral("float");
    } else if constexpr (std::is_same_v<dType, double>) {
        return QStringLiteral("double");
    } else if constexpr (std::is_same_v<dType, char>) {
        return QStringLiteral("char");
    } else {
        throw std::logic_error("itkSignal::dataTypeName Unknown type!");
    }
}

template<typename dType>
itkSignal<dType>::itkSignal(SignalImagePointerType pointerToImage, bool verboseIn):
        pImage{pointerToImage}, verbose{verboseIn} {
    // sets dimX, dimY, dimZ

    calculateImageSize();
    computeExtrema();
    normLower = minimumValue;
    normUpper = maximumValue;

    // calculate LUT
    calculateLUT();
}

template<typename dType>
itkSignal<dType>::itkSignal(SignalImagePointerType pointerToImage, QTreeWidget *motherTreeWidget, size_t signalIndex,
                            QString fileName, bool verboseIn)
    : pImage{pointerToImage}, verbose{verboseIn} {
    // sets dimX, dimY, dimZ
    calculateImageSize();
    computeExtrema();
    normLower = minimumValue;
    normUpper = maximumValue;
    name = fileName;

    setupTreeWidget(motherTreeWidget, signalIndex);

    // calculate LUT
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setName(QString nameIn) {
    SP_LOG_DEBUG("viewer.render", QStringLiteral("Setting signal name to %1").arg(nameIn));
    name = nameIn;
}


template<typename dType>
void itkSignal<dType>::setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) {

    QPixmap colorIcon(30, 30);
    colorIcon.fill(QColor::fromRgba(mainColor));

    auto *treeWidget = new QTreeWidgetItem(motherTreeWidget);
    treeWidget->setText(0, name);
    treeWidget->setText(1, "active");
    treeWidget->setIcon(1, colorIcon);
    treeWidget->setCheckState(0, Qt::CheckState::Checked);
    treeWidget->setData(0, signal_tree::SignalIndexRole, static_cast<qulonglong>(signalIndex));
    treeWidget->setData(0, signal_tree::RowKindRole, static_cast<int>(signal_tree::RowKind::Root));


    auto *colorWidget = new QTreeWidgetItem(treeWidget);
    colorWidget->setIcon(1, colorIcon);
    colorWidget->setText(0, "Color");
    colorWidget->setData(0, signal_tree::RowKindRole, static_cast<int>(signal_tree::RowKind::Color));
    std::string colorString = std::to_string(qRed(mainColor))
                              + " " + std::to_string(qGreen(mainColor))
                              + " " + std::to_string(qBlue(mainColor));
    colorWidget->setText(1, QString::fromStdString(colorString));

    auto *normWidget = new QTreeWidgetItem(treeWidget);
    normWidget->setText(0, "Norm");
    normWidget->setData(0, signal_tree::RowKindRole, static_cast<int>(signal_tree::RowKind::Norm));
    QString normString = QString("%1").arg(normLower) + " " + QString("%1").arg(normUpper);
    normWidget->setText(1, normString);

    auto *alphaWidget = new QTreeWidgetItem(treeWidget);
    alphaWidget->setText(0, "Alpha");
    alphaWidget->setData(0, signal_tree::RowKindRole, static_cast<int>(signal_tree::RowKind::Alpha));
    std::string alphaString = std::to_string(alpha);
    alphaWidget->setText(1, QString::fromStdString(alphaString));


    auto *dataTypeWidget = new QTreeWidgetItem(treeWidget);
    dataTypeWidget->setText(0, "data type");
    dataTypeWidget->setData(0, signal_tree::RowKindRole, static_cast<int>(signal_tree::RowKind::DataType));
    dataTypeWidget->setText(1, displayDataTypeName());
    motherTreeWidget->update();
}


template<typename dType>
void itkSignal<dType>::calculateImageSize() {
    auto &size = pImage->GetLargestPossibleRegion().GetSize();
    if (verbose) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Signal image size=%1x%2x%3")
                         .arg(size[0])
                         .arg(size[1])
                         .arg(size[2]));
    }
    dimX = size[0];
    dimY = size[1];
    dimZ = size[2];
}

template<typename dType>
void itkSignal<dType>::calculateLUT() {
    long long dTypeMax;
//  if floating point
    if constexpr (std::is_floating_point_v<dType>) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.render", QStringLiteral("Read signal uses floating-point voxels"));
        }
        dTypeMax = maximumValue;
    } else {
//         dtype is normally chart short or int, so long long and 10* should be fine
        long long upperLimitLUT = 10 * maximumValue; // limit number of maximum LUT values
        dTypeMax = std::min<long long>(upperLimitLUT, std::numeric_limits<dType>::max());
        // TODO: Automatically resize if requested LUTvalue is not in the array. This may cause errors downstream ...
    }

    long long int LUTSizeWanted = dTypeMax;
//        if max is e.g. 255, LUT size has to be 256!
    LUTSizeWanted = dTypeMax + 1;

    SP_LOG_DEBUG("viewer.render",
                 QStringLiteral("Calculating LUT for %1 values (currentSize=%2)")
                     .arg(LUTSizeWanted)
                     .arg(LUT.size()));
    size_t oldLUTSize = LUT.size();
    if (size_t(LUTSizeWanted) > LUT.size()) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Resizing LUT to %1 entries")
                         .arg(LUTSizeWanted));
        LUT.resize(LUTSizeWanted);
    }

    if (isCategorical) {
        size_t categoricalStart = categoricalLUTInitialized ? oldLUTSize : 0;
        calculateLUTCategorical(LUTSizeWanted, categoricalStart);
        categoricalLUTInitialized = true;
    } else if (isEdge) {
        calculateLUTEdge(LUTSizeWanted);
    } else {
        calculateLUTContinuous(LUTSizeWanted);
    }
}

template<typename dType>
void itkSignal<dType>::calculateLUTContinuous(long long) {
    //TODO: Handle negative continous luts
    //TODO: Handle Floats
    if (verbose) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Calculating continuous LUT with norm=[%1, %2]")
                         .arg(normLower)
                         .arg(normUpper));
    }

    double normFactorDecimal = 255. / (normUpper - normLower);
    if (verbose) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Continuous LUT norm factor=%1")
                         .arg(normFactorDecimal, 0, 'g', 6));
    }

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
void itkSignal<dType>::calculateLUTCategorical(long long, size_t startIndex) {
    if (verbose) {
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Calculating categorical LUT startIndex=%1")
                         .arg(startIndex));
    }
    // Generate new random colors only for entries beyond startIndex
    for (size_t i = startIndex; i < LUT.size(); ++i) {
        auto colorR = (std::rand() % 255);
        auto colorG = (std::rand() % 255);
        auto colorB = (std::rand() % 255);
        auto colorA = alpha;
        LUT.at(static_cast<unsigned long>(i)) = qRgba(colorR, colorG, colorB, colorA);
    }
    // Update alpha for existing entries, preserving their RGB colors
    for (size_t i = 0; i < startIndex; ++i) {
        LUT[i] = qRgba(qRed(LUT[i]), qGreen(LUT[i]), qBlue(LUT[i]), alpha);
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
void itkSignal<dType>::resetAutoContrastState() {
    autoContrastThreshold = 0;
}

template<typename dType>
bool itkSignal<dType>::computeNextAutoContrastRange(double &lower, double &upper) {
    constexpr int histogramBinCount = 256;
    const double imageMinimum = static_cast<double>(minimumValue);
    const double imageMaximum = static_cast<double>(maximumValue);

    lower = imageMinimum;
    upper = imageMaximum;

    if (pImage.IsNull()) {
        return false;
    }

    if (!std::isfinite(imageMinimum) || !std::isfinite(imageMaximum) || imageMaximum <= imageMinimum) {
        return true;
    }

    std::array<long long, histogramBinCount> histogram{};
    itk::ImageRegionConstIteratorWithIndex<SignalImageType> iterator(pImage, pImage->GetLargestPossibleRegion());
    const double scale = static_cast<double>(histogramBinCount) / (imageMaximum - imageMinimum);
    long long pixelCount = 0;

    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        const double value = static_cast<double>(iterator.Get());
        if (!std::isfinite(value)) {
            continue;
        }

        int histogramIndex = 0;
        if (value >= imageMaximum) {
            histogramIndex = histogramBinCount - 1;
        } else if (value > imageMinimum) {
            histogramIndex = static_cast<int>((value - imageMinimum) * scale);
            histogramIndex = std::clamp(histogramIndex, 0, histogramBinCount - 1);
        }

        histogram[histogramIndex] += 1;
        pixelCount += 1;
    }

    if (pixelCount <= 0) {
        return true;
    }

    const long long limit = pixelCount / 10;
    if (autoContrastThreshold < 10) {
        autoContrastThreshold = 5000;
    } else {
        autoContrastThreshold /= 2;
    }

    const long long threshold = pixelCount / autoContrastThreshold;

    int lowerIndex = 0;
    while (lowerIndex < histogramBinCount - 1) {
        long long count = histogram[lowerIndex];
        if (count > limit) {
            count = 0;
        }
        if (count > threshold) {
            break;
        }
        ++lowerIndex;
    }

    int upperIndex = histogramBinCount - 1;
    while (upperIndex > 0) {
        long long count = histogram[upperIndex];
        if (count > limit) {
            count = 0;
        }
        if (count > threshold) {
            break;
        }
        --upperIndex;
    }

    if (upperIndex < lowerIndex) {
        autoContrastThreshold = 0;
        return true;
    }

    const double binSize = (imageMaximum - imageMinimum) / static_cast<double>(histogramBinCount);
    lower = imageMinimum + static_cast<double>(lowerIndex) * binSize;
    upper = imageMinimum + static_cast<double>(upperIndex) * binSize;

    if (lower == upper) {
        lower = imageMinimum;
        upper = imageMaximum;
    }

    return true;
}

template<typename dType>
void itkSignal<dType>::setAlpha(unsigned char alphaIn) {
    alpha = alphaIn;
    calculateLUT();
}

template<typename dType>
void itkSignal<dType>::setLUTContinuous() {
    isCategorical = false;
    categoricalLUTInitialized = false;
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
    SP_LOG_INFO("viewer.render",
                QStringLiteral("Setting LUT value %1 to black")
                    .arg(value));
    LUT.at(value) = qRgba(0, 0, 0, 255);
    blackLUTValues.push_back(value);
}


template<typename dType>
void itkSignal<dType>::checkAndResizeLUT(unsigned int value) {
    if (value >= LUT.size()) {
        constexpr size_t minimumCategoricalLUTSize = 256;
        size_t oldSize = LUT.size();
        size_t newSize = oldSize;
        if (newSize < minimumCategoricalLUTSize) {
            newSize = minimumCategoricalLUTSize;
        }
        while (newSize <= value) {
            newSize *= 2;
        }
        LUT.resize(newSize);
        SP_LOG_DEBUG("viewer.render",
                     QStringLiteral("Resizing LUT to fit index %1 (newSize=%2)")
                         .arg(value)
                         .arg(newSize));
        if (isCategorical) {
            // Extend with new random colors; existing entries keep their colours
            calculateLUTCategorical(newSize, oldSize);
        } else {
            calculateLUT();
        }
    }
}


template<typename dType>
void itkSignal<dType>::randomizeCategoricalLUT() {
    if (!isCategorical || isEdge) {
        return;
    }

    categoricalLUTInitialized = false;
    calculateLUT();
}


template<typename dType>
void itkSignal<dType>::setLUTValueToTransparent(unsigned int value) {
    checkAndResizeLUT(value);
    SP_LOG_INFO("viewer.render",
                QStringLiteral("Setting LUT value %1 to transparent")
                    .arg(value));
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
    unsigned int width, height;
    const auto dims = slice_geometry::makeDimensions(dimX, dimY, dimZ);
    SignalImageRegionType region = slice_geometry::makeSliceRegion<SignalImageIndexType,
                                                                   SignalImageSizeType,
                                                                   SignalImageRegionType>(sliceIndex,
                                                                                          sliceAxis,
                                                                                          dims,
                                                                                          width,
                                                                                          height);

//    std::fill(sliceBuffer.begin(), sliceBuffer.end(), 0);

    typename itk::ImageRegionConstIteratorWithIndex<SignalImageType> it(pImage, region);
    it.GoToBegin();
//    TODO: is checkAndResizeLut needed?


    const dType* imageBuffer = pImage->GetBufferPointer();
    quint32* sliceBufferPtr = sliceBuffer->data();
    if constexpr (!std::is_floating_point_v<dType>) {
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
            // for now, rescale manually every time and check, there is a logic error when refining the segments
            // LUT is not increased automatically it seems, so we have to check every time

            for (unsigned long idx = 0; idx < sliceSize; ++idx) {
                dType value = sliceData[idx];
                if (value >= LUT.size()) {
                    checkAndResizeLUT(value);
                }
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
                    SP_LOG_ERROR("viewer.render",
                                 QStringLiteral("Out-of-range slice buffer access index=%1 exception=%2")
                                     .arg(getPixMapIndex(coords, sliceAxis))
                                     .arg(QString::fromUtf8(e.what())));
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
    return slice_geometry::pixmapIndex(coords, sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
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
