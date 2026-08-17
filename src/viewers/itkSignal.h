#ifndef HELLOWORLD_ITKSIGNAL_H
#define HELLOWORLD_ITKSIGNAL_H

#include <itkImage.h>
#include <QRgb>
#include <vector>
#include <map>
#include <QDateTime>
#include <QRandomGenerator>
#include <QTreeWidget>
#include "itkSignalBase.h"
#include "itkImageRegionConstIteratorWithIndex.h"
#include <unordered_map>
#include "src/file_definitions/dataTypes.h"
#include "src/qtUtils/SignalTreeWidgetUtils.h"
#include "src/utils/AppLogger.h"
#include "src/utils/utils.h"
//#include "graphBase.h"

#ifdef USE_OMP
#include <omp.h>
#endif

#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
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
    }

    void releaseImage() override {
        pImage = nullptr;
        dimX = 0;
        dimY = 0;
        dimZ = 0;
    }


    void calculateImageSize() override;

    QRgb colorForLabel(std::uint64_t label) const override;

    LabelColorSnapshot labelColorSnapshot() const override;

    void setEdgeColorMode() override;

    void rebuildEdgeColorTable(
        const std::unordered_map<unsigned int, char> &labelToStatus,
        const std::unordered_map<char, QRgb> &statusToColor) override;

    bool updateEdgeColorTable(
        const std::set<unsigned int> &changedLabels,
        const std::unordered_map<unsigned int, char> &labelToStatus,
        const std::unordered_map<char, QRgb> &statusToColor) override;

    void randomizeCategoricalPalette() override;


    virtual QImage
    calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis, std::vector<quint32> *sliceBuffer) override;

    void setNorm(double lower, double upper) override;

    bool computeQuantileContrastRange(
        double lowerQuantile,
        double upperQuantile,
        bool ignoreZero,
        double &lower,
        double &upper) const override;

    void setMainColor(int r, int g, int b) override;

    void setMainColor(QColor color) override;

    void setAlpha(unsigned char alphaIn) override;

    void setContinuousColorMode() override;

    void setCategoricalColorMode() override;

    void setValueColorToBlack(std::uint64_t value) override;

    void setValueColorToTransparent(std::uint64_t value) override;

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

    bool usesCategoricalColors() const override { return isCategorical && !isEdge; }

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

    long unsigned int dimX, dimY, dimZ;

    // default values for initialization of LUTs
    QRgb mainColor = qRgba(255, 255, 255, 255);
    unsigned char alpha = 150;
    double normLower = 0.0;
    double normUpper = 0.0;

    // Selects direct continuous rendering or categorical segment colors.
    bool isCategorical = false;

    // flag if signal should be drawn or not
    bool isActive = true;

    // useLookUp for calculation
    bool isEdge = false;

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
    template<typename ValueType>
    static std::uint64_t labelColorKey(ValueType value);
    void publishColorSnapshot(LabelColorSnapshot snapshot);
    double normalizeContinuousValue(double value) const;
    QRgb makeContinuousPixel(double value) const;

    std::shared_ptr<const LabelColorSnapshot> colorSnapshotState =
        std::make_shared<const LabelColorSnapshot>();

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

}

template<typename dType>
void itkSignal<dType>::setName(QString nameIn) {
    SP_LOG_DEBUG("viewer.render", QStringLiteral("Setting signal name to %1").arg(nameIn));
    name = nameIn;
}


template<typename dType>
void itkSignal<dType>::setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) {
    auto *treeWidget = new QTreeWidgetItem(motherTreeWidget);
    treeWidget->setText(0, QString());
    treeWidget->setText(1, QString());
    treeWidget->setFlags(treeWidget->flags() & ~Qt::ItemIsUserCheckable);
    treeWidget->setData(0, signal_tree::SignalIndexRole, static_cast<qulonglong>(signalIndex));
    treeWidget->setData(0, signal_tree::RowKindRole, static_cast<int>(signal_tree::RowKind::Root));
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
template<typename ValueType>
std::uint64_t itkSignal<dType>::labelColorKey(ValueType value) {
    static_assert(std::is_integral_v<ValueType>, "Label colors require an integral voxel type");
    if constexpr (std::is_signed_v<ValueType>) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
    }
    return static_cast<std::uint64_t>(value);
}

template<typename dType>
void itkSignal<dType>::publishColorSnapshot(LabelColorSnapshot snapshot) {
    std::shared_ptr<const LabelColorSnapshot> published =
        std::make_shared<LabelColorSnapshot>(std::move(snapshot));
    std::atomic_store_explicit(&colorSnapshotState, std::move(published), std::memory_order_release);
}

template<typename dType>
LabelColorSnapshot itkSignal<dType>::labelColorSnapshot() const {
    const auto snapshot =
        std::atomic_load_explicit(&colorSnapshotState, std::memory_order_acquire);
    return snapshot != nullptr ? *snapshot : LabelColorSnapshot{};
}

template<typename dType>
QRgb itkSignal<dType>::colorForLabel(std::uint64_t label) const {
    return labelColorSnapshot().colorForLabel(label);
}

template<typename dType>
void itkSignal<dType>::setEdgeColorMode() {
    isCategorical = false;
    isEdge = true;
    auto snapshot = labelColorSnapshot();
    snapshot.mode = LabelColorSnapshot::Mode::Edge;
    snapshot.alpha = alpha;
    snapshot.edgeColors = std::make_shared<const EdgeColorTable>();
    publishColorSnapshot(std::move(snapshot));
}

template<typename dType>
void itkSignal<dType>::rebuildEdgeColorTable(
    const std::unordered_map<unsigned int, char> &labelToStatus,
    const std::unordered_map<char, QRgb> &statusToColor) {
    LabelColorSnapshot::ColorMap colors;
    colors.reserve(labelToStatus.size());

    std::size_t missingStatusCount = 0;
    int firstMissingStatus = 0;
    for (const auto &[label, status] : labelToStatus) {
        const auto statusColor = statusToColor.find(status);
        if (statusColor == statusToColor.end()) {
            if (missingStatusCount == 0) {
                firstMissingStatus = static_cast<int>(status);
            }
            ++missingStatusCount;
            colors.emplace(static_cast<std::uint64_t>(label), QRgb{});
        } else {
            colors.emplace(static_cast<std::uint64_t>(label), statusColor->second);
        }
    }

    if (missingStatusCount > 0) {
        SP_LOG_WARNING(
            "viewer.render",
            QStringLiteral("Edge color rebuild made %1 label(s) transparent; first missing status=%2")
                .arg(missingStatusCount)
                .arg(firstMissingStatus));
    }

    isCategorical = false;
    isEdge = true;
    auto snapshot = labelColorSnapshot();
    snapshot.mode = LabelColorSnapshot::Mode::Edge;
    snapshot.alpha = alpha;
    snapshot.edgeColors = std::make_shared<const EdgeColorTable>(std::move(colors));
    publishColorSnapshot(std::move(snapshot));
}

template<typename dType>
bool itkSignal<dType>::updateEdgeColorTable(
    const std::set<unsigned int> &changedLabels,
    const std::unordered_map<unsigned int, char> &labelToStatus,
    const std::unordered_map<char, QRgb> &statusToColor) {
    if (changedLabels.empty()) {
        return true;
    }

    const auto snapshot = labelColorSnapshot();
    if (snapshot.mode != LabelColorSnapshot::Mode::Edge || snapshot.edgeColors == nullptr) {
        return false;
    }

    std::vector<std::pair<std::uint64_t, QRgb>> updates;
    updates.reserve(changedLabels.size());
    std::size_t missingStatusCount = 0;
    int firstMissingStatus = 0;
    for (const unsigned int label : changedLabels) {
        const auto labelStatus = labelToStatus.find(label);
        if (labelStatus == labelToStatus.end() || !snapshot.edgeColors->contains(label)) {
            return false;
        }

        QRgb color = 0;
        const auto statusColor = statusToColor.find(labelStatus->second);
        if (statusColor == statusToColor.end()) {
            if (missingStatusCount == 0) {
                firstMissingStatus = static_cast<int>(labelStatus->second);
            }
            ++missingStatusCount;
        } else {
            color = statusColor->second;
        }
        updates.emplace_back(static_cast<std::uint64_t>(label), color);
    }

    if (missingStatusCount > 0) {
        SP_LOG_WARNING(
            "viewer.render",
            QStringLiteral("Edge color update made %1 label(s) transparent; first missing status=%2")
                .arg(missingStatusCount)
                .arg(firstMissingStatus));
    }

    for (const auto &[label, color] : updates) {
        if (!snapshot.edgeColors->updateColor(label, color)) {
            return false;
        }
    }
    return true;
}

template<typename dType>
double itkSignal<dType>::normalizeContinuousValue(double value) const {
    if (!std::isfinite(value) ||
        !std::isfinite(normLower) ||
        !std::isfinite(normUpper) ||
        normUpper < normLower) {
        return 0.0;
    }
    if (normUpper == normLower) {
        // A zero-width range is inclusive, while raw zero stays transparent.
        const bool visible = value != 0.0 && value >= normLower;
        return visible ? 255.0 : 0.0;
    }
    return std::clamp((value - normLower) * 255.0 / (normUpper - normLower), 0.0, 255.0);
}

template<typename dType>
QRgb itkSignal<dType>::makeContinuousPixel(double value) const {
    const double normedValue = normalizeContinuousValue(value);
    // Encode intensity in either RGB or alpha so compositing never applies it twice.
    if (getBlendMode() == itkSignalBase::BlendMode::Additive) {
        const auto colorR = static_cast<unsigned char>(normedValue * (qRed(mainColor) / 255.));
        const auto colorG = static_cast<unsigned char>(normedValue * (qGreen(mainColor) / 255.));
        const auto colorB = static_cast<unsigned char>(normedValue * (qBlue(mainColor) / 255.));
        return qRgba(colorR, colorG, colorB, alpha);
    }

    const auto colorA = static_cast<unsigned char>(normedValue * (alpha / 255.));
    return qRgba(qRed(mainColor), qGreen(mainColor), qBlue(mainColor), colorA);
}

template<typename dType>
void itkSignal<dType>::setMainColor(int r, int g, int b) {
    mainColor = qRgba(r, g, b, 255);
}

template<typename dType>
void itkSignal<dType>::setMainColor(QColor color) {
    mainColor = color.rgba();
}


template<typename dType>
void itkSignal<dType>::setNorm(double lower, double upper) {
    normLower = lower;
    normUpper = upper;
}

template<typename dType>
bool itkSignal<dType>::computeQuantileContrastRange(
    double lowerQuantile,
    double upperQuantile,
    bool ignoreZero,
    double &lower,
    double &upper) const {
    constexpr std::size_t histogramBinCount = 4096;
    const double imageMinimum = static_cast<double>(minimumValue);
    const double imageMaximum = static_cast<double>(maximumValue);

    lower = imageMinimum;
    upper = imageMaximum;

    if (pImage.IsNull()
        || !std::isfinite(lowerQuantile)
        || !std::isfinite(upperQuantile)
        || lowerQuantile < 0.0
        || upperQuantile > 1.0
        || lowerQuantile >= upperQuantile) {
        return false;
    }

    if (!std::isfinite(imageMinimum) || !std::isfinite(imageMaximum) || imageMaximum <= imageMinimum) {
        return true;
    }

    std::array<std::size_t, histogramBinCount> histogram{};
    itk::ImageRegionConstIteratorWithIndex<SignalImageType> iterator(pImage, pImage->GetLargestPossibleRegion());
    const double scale = static_cast<double>(histogramBinCount) / (imageMaximum - imageMinimum);
    std::size_t pixelCount = 0;
    double includedMinimum = imageMaximum;
    double includedMaximum = imageMinimum;

    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        const double value = static_cast<double>(iterator.Get());
        if (!std::isfinite(value) || (ignoreZero && value == 0.0)) {
            continue;
        }

        includedMinimum = std::min(includedMinimum, value);
        includedMaximum = std::max(includedMaximum, value);

        std::size_t histogramIndex = 0;
        if (value >= imageMaximum) {
            histogramIndex = histogramBinCount - 1;
        } else if (value > imageMinimum) {
            histogramIndex = static_cast<std::size_t>((value - imageMinimum) * scale);
            histogramIndex = std::min(histogramIndex, histogramBinCount - 1);
        }

        histogram[histogramIndex] += 1;
        pixelCount += 1;
    }

    if (pixelCount == 0) {
        return true;
    }
    if (includedMinimum == includedMaximum) {
        lower = includedMinimum;
        upper = includedMaximum;
        return true;
    }

    const auto quantileBin = [&histogram, pixelCount](double quantile) {
        const auto targetRank = static_cast<std::size_t>(
            std::floor(quantile * static_cast<double>(pixelCount - 1)));
        std::size_t cumulativeCount = 0;
        for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
            cumulativeCount += histogram[bin];
            if (cumulativeCount > targetRank) {
                return bin;
            }
        }
        return histogram.size() - 1;
    };

    const std::size_t lowerBin = quantileBin(lowerQuantile);
    const std::size_t upperBin = quantileBin(upperQuantile);
    const double binSize = (imageMaximum - imageMinimum) / static_cast<double>(histogramBinCount);
    lower = imageMinimum + static_cast<double>(lowerBin) * binSize;
    upper = std::min(
        imageMaximum,
        imageMinimum + static_cast<double>(upperBin + 1) * binSize);

    if (!std::isfinite(lower) || !std::isfinite(upper) || lower >= upper) {
        lower = imageMinimum;
        upper = imageMaximum;
    }

    return true;
}

template<typename dType>
void itkSignal<dType>::setAlpha(unsigned char alphaIn) {
    alpha = alphaIn;
    auto snapshot = labelColorSnapshot();
    snapshot.alpha = alphaIn;
    publishColorSnapshot(std::move(snapshot));
}

template<typename dType>
void itkSignal<dType>::setContinuousColorMode() {
    isCategorical = false;
    isEdge = false;
}


template<typename dType>
void itkSignal<dType>::setCategoricalColorMode() {
    isCategorical = true;
    isEdge = false;
    auto snapshot = labelColorSnapshot();
    snapshot.mode = LabelColorSnapshot::Mode::Categorical;
    snapshot.alpha = alpha;
    snapshot.edgeColors = std::make_shared<const EdgeColorTable>();
    publishColorSnapshot(std::move(snapshot));
}

template<typename dType>
void itkSignal<dType>::setValueColorToBlack(std::uint64_t value) {
    SP_LOG_INFO("viewer.render",
                QStringLiteral("Setting value %1 color to black")
                    .arg(value));
    auto snapshot = labelColorSnapshot();
    auto overrides = snapshot.overrides != nullptr
        ? std::make_shared<LabelColorSnapshot::ColorMap>(*snapshot.overrides)
        : std::make_shared<LabelColorSnapshot::ColorMap>();
    (*overrides)[value] = qRgba(0, 0, 0, 255);
    snapshot.overrides = std::move(overrides);
    publishColorSnapshot(std::move(snapshot));
}


template<typename dType>
void itkSignal<dType>::randomizeCategoricalPalette() {
    if (!isCategorical || isEdge) {
        return;
    }

    auto snapshot = labelColorSnapshot();
    std::uint64_t newSeed = snapshot.paletteSeed;
    while (newSeed == snapshot.paletteSeed) {
        newSeed = QRandomGenerator::global()->generate64();
    }
    snapshot.paletteSeed = newSeed;
    publishColorSnapshot(std::move(snapshot));
}


template<typename dType>
void itkSignal<dType>::setValueColorToTransparent(std::uint64_t value) {
    SP_LOG_INFO("viewer.render",
                QStringLiteral("Setting value %1 color to transparent")
                    .arg(value));
    auto snapshot = labelColorSnapshot();
    auto overrides = snapshot.overrides != nullptr
        ? std::make_shared<LabelColorSnapshot::ColorMap>(*snapshot.overrides)
        : std::make_shared<LabelColorSnapshot::ColorMap>();
    (*overrides)[value] = qRgba(0, 0, 0, 0);
    snapshot.overrides = std::move(overrides);
    publishColorSnapshot(std::move(snapshot));
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
    if (pImage.IsNull() || sliceBuffer == nullptr) {
        return {};
    }

    const auto dims = slice_geometry::makeDimensions(dimX, dimY, dimZ);
    if (sliceIndex >= slice_geometry::sliceLimit(sliceAxis, dims)) {
        throw std::out_of_range("sliceIndex lies outside the image");
    }

    const auto sliceWidth = static_cast<unsigned long>(slice_geometry::sliceWidth(sliceAxis, dims));
    const auto sliceHeight = static_cast<unsigned long>(slice_geometry::sliceHeight(sliceAxis, dims));
    const std::size_t requiredPixelCount =
        static_cast<std::size_t>(sliceWidth) * static_cast<std::size_t>(sliceHeight);
    if (sliceBuffer->size() < requiredPixelCount) {
        sliceBuffer->resize(requiredPixelCount);
    }

    unsigned long baseOffset = 0;
    unsigned long columnStride = 0;
    unsigned long rowStride = 0;
    switch (sliceAxis) {
        case 0:
            baseOffset = sliceIndex;
            columnStride = dimX * dimY;
            rowStride = dimX;
            break;
        case 1:
            baseOffset = sliceIndex * dimX;
            columnStride = 1;
            rowStride = dimX * dimY;
            break;
        case 2:
            baseOffset = sliceIndex * dimX * dimY;
            columnStride = 1;
            rowStride = dimX;
            break;
        default:
            throw std::logic_error("sliceAxis not implemented!");
    }

    const dType *imageBuffer = pImage->GetBufferPointer();
    quint32 *sliceBufferPtr = sliceBuffer->data();
    const bool renderLabels = isCategorical || isEdge;
    const bool renderLabelBoundaries =
        isCategorical && !isEdge &&
        getLabelRenderMode() == itkSignalBase::LabelRenderMode::Boundaries;
    const LabelColorSnapshot colorSnapshot = labelColorSnapshot();
    struct SliceColorPage {
        std::array<std::uint64_t, 4096> encodedColors{};
    };
    constexpr std::size_t maximumSliceColorPages = 32;
    std::unordered_map<std::uint64_t, SliceColorPage> sliceColorPages;
    std::unordered_map<std::uint64_t, QRgb> overflowSliceColors;
    if (renderLabels) {
        sliceColorPages.reserve(16);
    }
    std::uint64_t currentColorPageKey = 0;
    SliceColorPage *currentColorPage = nullptr;
    bool hasLastLabelColor = false;
    std::uint64_t lastLabelKey = 0;
    QRgb lastLabelColor = 0;

    const auto uncachedColorForLabelKey = [&](std::uint64_t key) -> QRgb {
        const std::uint64_t pageKey = key >> 12U;
        if (currentColorPage == nullptr || currentColorPageKey != pageKey) {
            const auto existingPage = sliceColorPages.find(pageKey);
            if (existingPage != sliceColorPages.end()) {
                currentColorPage = &existingPage->second;
            } else if (sliceColorPages.size() < maximumSliceColorPages) {
                currentColorPage = &sliceColorPages.try_emplace(pageKey).first->second;
            } else {
                currentColorPage = nullptr;
            }
            currentColorPageKey = pageKey;
        }

        if (currentColorPage != nullptr) {
            const std::size_t pageOffset = static_cast<std::size_t>(key & 0xfffU);
            const std::uint64_t encodedColor = currentColorPage->encodedColors[pageOffset];
            if (encodedColor != 0) {
                return static_cast<QRgb>(encodedColor);
            }
            const QRgb color = colorSnapshot.colorForLabel(key);
            currentColorPage->encodedColors[pageOffset] =
                (std::uint64_t{1} << 32U) | static_cast<std::uint32_t>(color);
            return color;
        }

        const auto existingColor = overflowSliceColors.find(key);
        if (existingColor != overflowSliceColors.end()) {
            return existingColor->second;
        }
        const QRgb color = colorSnapshot.colorForLabel(key);
        overflowSliceColors.emplace(key, color);
        return color;
    };

    const auto colorForLabelValue = [&](dType value) -> QRgb {
        if constexpr (std::is_integral_v<dType>) {
            const std::uint64_t key = labelColorKey(value);
            if (hasLastLabelColor && key == lastLabelKey) {
                return lastLabelColor;
            }
            const QRgb color = uncachedColorForLabelKey(key);
            lastLabelKey = key;
            lastLabelColor = color;
            hasLastLabelColor = true;
            return color;
        } else {
            return QRgb{};
        }
    };
    const auto colorForBoundaryLabelValue = [&](dType value) -> QRgb {
        if constexpr (std::is_integral_v<dType>) {
            return uncachedColorForLabelKey(labelColorKey(value));
        } else {
            return QRgb{};
        }
    };

    const QRgb transparentPixel = qRgba(0, 0, 0, 0);
    if (renderLabelBoundaries) {
        for (unsigned long row = 0; row < sliceHeight; ++row) {
            const unsigned long imageRowOffset = baseOffset + row * rowStride;
            const unsigned long sliceRowOffset = row * sliceWidth;
            for (unsigned long column = 0; column < sliceWidth; ++column) {
                const unsigned long imageOffset = imageRowOffset + column * columnStride;
                const dType value = imageBuffer[imageOffset];
                const bool hasFourInPlaneNeighbors =
                    row > 0 && row + 1 < sliceHeight &&
                    column > 0 && column + 1 < sliceWidth;
                const bool isBoundary =
                    !hasFourInPlaneNeighbors ||
                    imageBuffer[imageOffset - rowStride] != value ||
                    imageBuffer[imageOffset + rowStride] != value ||
                    imageBuffer[imageOffset - columnStride] != value ||
                    imageBuffer[imageOffset + columnStride] != value;
                sliceBufferPtr[sliceRowOffset + column] =
                    isBoundary ? colorForBoundaryLabelValue(value) : transparentPixel;
            }
        }
    } else if constexpr (!std::is_floating_point_v<dType>) {
        if (renderLabels) {
            for (unsigned long row = 0; row < sliceHeight; ++row) {
                const unsigned long imageRowOffset = baseOffset + row * rowStride;
                const unsigned long sliceRowOffset = row * sliceWidth;
                for (unsigned long column = 0; column < sliceWidth; ++column) {
                    const dType value = imageBuffer[imageRowOffset + column * columnStride];
                    sliceBufferPtr[sliceRowOffset + column] = colorForLabelValue(value);
                }
            }
        } else {
            for (unsigned long row = 0; row < sliceHeight; ++row) {
                const unsigned long imageRowOffset = baseOffset + row * rowStride;
                const unsigned long sliceRowOffset = row * sliceWidth;
                for (unsigned long column = 0; column < sliceWidth; ++column) {
                    const dType value = imageBuffer[imageRowOffset + column * columnStride];
                    sliceBufferPtr[sliceRowOffset + column] =
                        makeContinuousPixel(static_cast<double>(value));
                }
            }
        }
    } else {
        for (unsigned long row = 0; row < sliceHeight; ++row) {
            const unsigned long imageRowOffset = baseOffset + row * rowStride;
            const unsigned long sliceRowOffset = row * sliceWidth;
            for (unsigned long column = 0; column < sliceWidth; ++column) {
                const dType value = imageBuffer[imageRowOffset + column * columnStride];
                sliceBufferPtr[sliceRowOffset + column] =
                    makeContinuousPixel(static_cast<double>(value));
            }
        }
    }

    return QImage(reinterpret_cast<const unsigned char *>(sliceBuffer->data()),
                  static_cast<int>(sliceWidth),
                  static_cast<int>(sliceHeight),
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
