#ifndef HELLOWORLD_ITKSIGNALBASE_H
#define HELLOWORLD_ITKSIGNALBASE_H

#include <itkImage.h>
#include <itkImageBase.h>
#include <QRgb>
#include <QColor>
#include <atomic>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <set>
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


class EdgeColorTable {
public:
    using LabelKey = std::uint64_t;
    using ColorMap = std::unordered_map<LabelKey, QRgb>;

    EdgeColorTable() = default;

    explicit EdgeColorTable(ColorMap colors) {
        atomicColors.reserve(colors.size());
        for (const auto &[label, color] : colors) {
            atomicColors.try_emplace(label, color);
        }
    }

    bool contains(LabelKey label) const noexcept {
        return atomicColors.find(label) != atomicColors.end();
    }

    bool colorForLabel(LabelKey label, QRgb &color) const noexcept {
        const auto entry = atomicColors.find(label);
        if (entry == atomicColors.end()) {
            return false;
        }
        color = entry->second.load();
        return true;
    }

    bool updateColor(LabelKey label, QRgb color) const noexcept {
        const auto entry = atomicColors.find(label);
        if (entry == atomicColors.end()) {
            return false;
        }
        entry->second.store(color);
        return true;
    }

    std::size_t size() const noexcept {
        return atomicColors.size();
    }

private:
    class AtomicColor {
    public:
        explicit AtomicColor(QRgb color) noexcept : value(color) {}

        QRgb load() const noexcept {
            return value.load(std::memory_order_relaxed);
        }

        void store(QRgb color) const noexcept {
            value.store(color, std::memory_order_relaxed);
        }

    private:
        mutable std::atomic<QRgb> value;
    };

    std::unordered_map<LabelKey, AtomicColor> atomicColors;
};


class LabelColorSnapshot {
public:
    enum class Mode {
        Categorical,
        Edge
    };

    using LabelKey = EdgeColorTable::LabelKey;
    using ColorMap = EdgeColorTable::ColorMap;

    static constexpr std::uint64_t DefaultPaletteSeed = 0x6a09e667f3bcc909ULL;

    Mode mode = Mode::Categorical;
    std::uint64_t paletteSeed = DefaultPaletteSeed;
    unsigned char alpha = 150;
    std::shared_ptr<const EdgeColorTable> edgeColors = std::make_shared<const EdgeColorTable>();
    std::shared_ptr<const ColorMap> overrides = std::make_shared<const ColorMap>();

    QRgb colorForLabel(LabelKey label) const noexcept {
        if (overrides != nullptr && !overrides->empty()) {
            const auto overrideColor = overrides->find(label);
            if (overrideColor != overrides->end()) {
                return overrideColor->second;
            }
        }
        if (mode == Mode::Edge) {
            QRgb edgeColor = 0;
            if (edgeColors != nullptr && edgeColors->colorForLabel(label, edgeColor)) {
                if (edgeColor != 0) {
                    return qRgba(qRed(edgeColor),
                                 qGreen(edgeColor),
                                 qBlue(edgeColor),
                                 alpha);
                }
            }
            return qRgba(0, 0, 0, 0);
        }

        const std::uint64_t mixed = splitMix64(label ^ paletteSeed);
        const unsigned int hue = static_cast<unsigned int>(mixed % 360ULL);
        const unsigned int saturation = 176U + static_cast<unsigned int>((mixed >> 9U) % 80ULL);
        const unsigned int value = 192U + static_cast<unsigned int>((mixed >> 17U) % 64ULL);
        const unsigned int sector = hue / 60U;
        const unsigned int fraction = ((hue % 60U) * 255U) / 60U;
        const unsigned int low = (value * (255U - saturation)) / 255U;
        const unsigned int falling =
            (value * (255U - (saturation * fraction) / 255U)) / 255U;
        const unsigned int rising =
            (value * (255U - (saturation * (255U - fraction)) / 255U)) / 255U;

        switch (sector) {
            case 0: return qRgba(value, rising, low, alpha);
            case 1: return qRgba(falling, value, low, alpha);
            case 2: return qRgba(low, value, rising, alpha);
            case 3: return qRgba(low, falling, value, alpha);
            case 4: return qRgba(rising, low, value, alpha);
            default: return qRgba(value, low, falling, alpha);
        }
    }

private:
    static std::uint64_t splitMix64(std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }
};


// abstract class for viewers of itk signals
class itkSignalBase {

public:
    enum class BlendMode {
        SourceOver,
        Additive
    };

    enum class LabelRenderMode {
        Filled,
        Boundaries
    };

    enum class LayerRole {
        Overlay,
        SourceImage
    };

    virtual ~itkSignalBase() {};

    virtual itk::ImageBase<3>::Pointer getImageBase() const = 0;

    virtual void updateImage(itk::ImageBase<3>::Pointer newImage) = 0;

    virtual void releaseImage() = 0;

    virtual void setupTreeWidget(QTreeWidget *motherTreeWidget, size_t signalIndex) = 0;


    virtual void calculateImageSize() = 0;

    virtual QRgb colorForLabel(std::uint64_t label) const = 0;

    virtual LabelColorSnapshot labelColorSnapshot() const = 0;

    virtual void setEdgeColorMode() = 0;

    virtual void rebuildEdgeColorTable(
        const std::unordered_map<unsigned int, char> &labelToStatus,
        const std::unordered_map<char, QRgb> &statusToColor) = 0;

    virtual bool updateEdgeColorTable(
        const std::set<unsigned int> &changedLabels,
        const std::unordered_map<unsigned int, char> &labelToStatus,
        const std::unordered_map<char, QRgb> &statusToColor) = 0;

    virtual void setNorm(double lower, double upper) = 0;

    virtual bool computeQuantileContrastRange(
        double lowerQuantile,
        double upperQuantile,
        bool ignoreZero,
        double &lower,
        double &upper) const = 0;

    virtual void setMainColor(int r, int g, int b) = 0;

    virtual void setMainColor(QColor color) = 0;

    virtual void setAlpha(unsigned char alphaIn) = 0;

    virtual void setContinuousColorMode() = 0;

    virtual void setCategoricalColorMode() = 0;

    virtual void setValueColorToBlack(std::uint64_t value) = 0;

    virtual void setValueColorToTransparent(std::uint64_t value) = 0;

    virtual void randomizeCategoricalPalette() = 0;

    virtual void setIsActive(bool isActiveIn) = 0;

    virtual QImage
    calculateSliceQImage(unsigned int sliceIndex, unsigned int sliceAxis, std::vector<quint32> *sliceBuffer) = 0;

    virtual QString getNumberOfXYZAsString(int x, int y, int z) = 0;
    // todo: make function return string? or double for xyz position for printout
    // or both? string better for printing, double would give universal useability, independent of data type

    virtual double getNormLower() = 0;

    virtual double getNormUpper() = 0;

    virtual double getMinimumValueAsDouble() const = 0;

    virtual double getMaximumValueAsDouble() const = 0;

    virtual unsigned int getAlpha() = 0;

    virtual QRgb getColor() = 0;

    virtual QString getDisplayDataTypeName() const = 0;

    virtual bool supportsNormControl() const = 0;

    virtual bool usesCategoricalColors() const = 0;

    virtual bool usesEdgeStatusColors() const = 0;

    virtual unsigned long getDimX() = 0;

    virtual unsigned long getDimY() = 0;

    virtual unsigned long getDimZ() = 0;

    virtual bool getIsActive() = 0;

    BlendMode getBlendMode() const {
        return blendMode;
    }

    void setBlendMode(BlendMode blendModeIn) {
        if (blendMode == blendModeIn) {
            return;
        }
        blendMode = blendModeIn;
    }

    LabelRenderMode getLabelRenderMode() const {
        return labelRenderMode;
    }

    void setLabelRenderMode(LabelRenderMode labelRenderModeIn) {
        labelRenderMode = labelRenderModeIn;
    }

    LayerRole getLayerRole() const {
        return layerRole;
    }

    void setLayerRole(LayerRole layerRoleIn) {
        layerRole = layerRoleIn;
    }

//    virtual void isContinuous() = 0;

    virtual void setName(QString name) = 0;

    QString name;
    QString sourceFilePath;

private:
    BlendMode blendMode = BlendMode::SourceOver;
    LabelRenderMode labelRenderMode = LabelRenderMode::Filled;
    LayerRole layerRole = LayerRole::Overlay;
};


#endif //HELLOWORLD_ITKSIGNALBASE_H
