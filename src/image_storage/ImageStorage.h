#pragma once

#include <itkImage.h>
#include <itkObjectFactory.h>
#include <array>
#include <functional>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>
#include <utility>
#include <algorithm>
#include <optional>
#include <unordered_map>

namespace segment_puzzler {

// Region access for dense and RLE images. denseRegion() owns its buffer.
// Write notifications run after releasing the storage lock.
template<typename TPixel>
class ImageStorage : public itk::Object {
public:
    using Self = ImageStorage;
    using Pointer = itk::SmartPointer<Self>;
    using PixelType = TPixel;
    using Image = itk::Image<TPixel, 3>;
    using Region = typename Image::RegionType;
    using Index = typename Image::IndexType;
    struct VoxelRange { Index start; std::size_t length; TPixel value; };
    struct LabelStatistics {
        std::size_t voxelCount = 0;
        Region bounds;
        void include(const Region &region, std::size_t count) {
            if (!count) return;
            auto first = region.GetIndex(), last = region.GetUpperIndex();
            if (voxelCount) for (unsigned axis = 0; axis < 3; ++axis) {
                first[axis] = std::min(first[axis], bounds.GetIndex()[axis]);
                last[axis] = std::max(last[axis], bounds.GetUpperIndex()[axis]);
            }
            bounds.SetIndex(first); bounds.SetUpperIndex(last);
            voxelCount += count;
        }
    };
    using LabelStatisticsMap = std::unordered_map<TPixel, LabelStatistics>;
    using RunVisitor = std::function<void(const Index &, std::size_t, TPixel)>;
    itkTypeMacro(ImageStorage, Object);

    virtual const itk::ImageBase<3> *imageInformation() const { return geometry; }
    itk::ImageBase<3> *imageInformation() { return const_cast<itk::ImageBase<3> *>(std::as_const(*this).imageInformation()); }
    const auto &GetSpacing() const { return imageInformation()->GetSpacing(); }
    const auto &GetOrigin() const { return imageInformation()->GetOrigin(); }
    const auto &GetDirection() const { return imageInformation()->GetDirection(); }
    const auto &GetMetaDataDictionary() const { return imageInformation()->GetMetaDataDictionary(); }
    auto &GetMetaDataDictionary() { return imageInformation()->GetMetaDataDictionary(); }
    auto ComputeOffset(const Index &index) const { return imageInformation()->ComputeOffset(index); }
    const Region &GetBufferedRegion() const { return GetLargestPossibleRegion(); }
    template<typename Point> void TransformIndexToPhysicalPoint(const Index &index, Point &point) const {
        imageInformation()->TransformIndexToPhysicalPoint(index, point);
    }
    itk::ModifiedTimeType GetMTime() const override {
        return std::max(itk::Object::GetMTime(), imageInformation()->GetMTime());
    }
    // Dense storage can share its existing buffer for a temporary read.
    virtual typename Image::ConstPointer readImageRegion(const Region &region) const { return denseRegion(region); }
    const Region &GetLargestPossibleRegion() const { return imageInformation()->GetLargestPossibleRegion(); }
    virtual std::size_t storedBytes() const = 0;
    virtual TPixel GetPixel(const Index &index) const = 0;
    virtual void readRegion(const Region &region, TPixel *output, std::size_t count) const = 0;
    virtual void FillBuffer(TPixel value) = 0;
    virtual void fillRegion(const Region &region, TPixel value) = 0;
    virtual void setVoxelValues(const std::vector<Index> &indices, TPixel value) = 0;
    // Ranges extend along X. Later entries overwrite earlier entries.
    virtual void fillVoxelRanges(const std::vector<VoxelRange> &ranges) = 0;
    virtual LabelStatisticsMap labelStatistics() const {
        LabelStatisticsMap result;
        forEachRun([&](const Index &start, std::size_t length, TPixel value) {
            result[value].include(Region(start, {length, 1, 1}), length);
        });
        return result;
    }
    virtual std::optional<LabelStatistics> labelStatistics(TPixel value) const {
        LabelStatistics result;
        forEachRun([&](const Index &start, std::size_t length, TPixel label) {
            if (label == value) result.include(Region(start, {length, 1, 1}), length);
        });
        return result.voxelCount ? std::optional<LabelStatistics>(result) : std::nullopt;
    }
    virtual void writeRegion(const Region &region, const TPixel *pixels, std::size_t count) = 0;
    // Visits disjoint X runs in Z/Y/X order. Callbacks hold the read lock and must not access this storage.
    void forEachRun(const RunVisitor &visit) const { forEachRun(GetLargestPossibleRegion(), visit); }
    virtual void forEachRun(const Region &region, const RunVisitor &visit) const = 0;

    void SetPixel(const Index &index, TPixel value) { fillRegion(Region(index, {1, 1, 1}), value); }
    void readSlice(unsigned normal, itk::IndexValueType index, std::vector<TPixel> &output) const {
        if (normal > 2) throw std::out_of_range("Invalid slice axis");
        auto region = GetLargestPossibleRegion();
        region.SetIndex(normal, index);
        region.SetSize(normal, 1);
        validate(region);
        output.resize(byteCount(region) / sizeof(TPixel));
        readRegion(region, output.data(), output.size());
    }
    typename Image::Pointer denseRegion(const Region &region) const {
        validate(region);
        auto image = Image::New();
        image->CopyInformation(imageInformation());
        image->SetMetaDataDictionary(imageInformation()->GetMetaDataDictionary());
        image->SetRegions(region);
        image->Allocate(false);
        readRegion(region, image->GetBufferPointer(), byteCount(region) / sizeof(TPixel));
        return image;
    }
    // Changes are committed after the callback completes successfully.
    template<typename Edit>
    void editImageRegion(const Region &region, Edit &&edit) {
        auto image = denseRegion(region);
        edit(*image);
        writeRegion(region, image->GetBufferPointer(), region.GetNumberOfPixels());
    }
    static std::size_t byteCount(const Region &region) {
        std::size_t bytes = sizeof(TPixel);
        for (auto length : region.GetSize()) {
            if (length && bytes > std::numeric_limits<std::size_t>::max() / length)
                throw std::overflow_error("Image byte count overflow");
            bytes *= length;
        }
        return bytes;
    }

protected:
    void initialize(const itk::ImageBase<3> &information) {
        const auto region = information.GetLargestPossibleRegion();
        const auto size = region.GetSize();
        byteCount(region);
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (size[axis] == 0 || size[axis] - 1 > std::size_t(std::numeric_limits<itk::IndexValueType>::max()) ||
                region.GetIndex()[axis] > std::numeric_limits<itk::IndexValueType>::max() - itk::IndexValueType(size[axis] - 1))
                throw std::invalid_argument("Invalid image extent");
        }
        geometry = itk::ImageBase<3>::New();
        geometry->CopyInformation(&information);
        geometry->SetMetaDataDictionary(information.GetMetaDataDictionary());
        geometry->SetRegions(region);
    }
    static void validateBufferedImage(const Image &image) {
        const auto region = image.GetLargestPossibleRegion();
        byteCount(region);
        if (image.GetBufferedRegion() != region || !image.GetBufferPointer() ||
            image.GetPixelContainer()->Size() < region.GetNumberOfPixels())
            throw std::invalid_argument("Input image must be fully buffered");
    }
    void validate(const Region &region) const {
        const auto &full = GetLargestPossibleRegion();
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto index = region.GetIndex()[axis], origin = full.GetIndex()[axis];
            if (!region.GetSize()[axis] || index < origin ||
                index > origin + itk::IndexValueType(full.GetSize()[axis] - 1) ||
                region.GetSize()[axis] > full.GetSize()[axis] - std::size_t(index - origin))
                throw std::out_of_range("ROI lies outside image storage");
        }
    }
    std::array<std::size_t, 3> localIndex(const Index &index) const {
        const auto origin = GetLargestPossibleRegion().GetIndex();
        return {std::size_t(index[0] - origin[0]), std::size_t(index[1] - origin[1]), std::size_t(index[2] - origin[2])};
    }
    itk::ImageBase<3>::Pointer geometry;
    mutable std::shared_mutex mutex;
};

} // namespace segment_puzzler
