#pragma once

#include "ImageStorage.h"
#include <algorithm>

namespace segment_puzzler {

template<typename TPixel>
class DenseImageStorage final : public ImageStorage<TPixel> {
public:
    using Self = DenseImageStorage;
    using Superclass = ImageStorage<TPixel>;
    using Pointer = itk::SmartPointer<Self>;
    using typename Superclass::Image;
    using typename Superclass::Region;
    using typename Superclass::Index;
    using typename Superclass::RunVisitor;
    using typename Superclass::VoxelRange;
    using Superclass::GetLargestPossibleRegion;
    using Superclass::byteCount;
    using Superclass::forEachRun;
    itkNewMacro(Self);
    itkTypeMacro(DenseImageStorage, ImageStorage);

    static Pointer create(const itk::ImageBase<3> &information, TPixel value = {}) {
        auto storage = Self::New();
        storage->initialize(information);
        storage->image = Image::New();
        storage->image->CopyInformation(storage->geometry);
        storage->image->SetMetaDataDictionary(storage->geometry->GetMetaDataDictionary());
        storage->image->SetRegions(information.GetLargestPossibleRegion());
        storage->image->Allocate(false);
        storage->image->FillBuffer(value);
        return storage;
    }
    // Shares the image buffer. Route subsequent pixel access through the storage.
    static Pointer fromImage(typename Image::Pointer image) {
        if (!image) throw std::invalid_argument("Missing input image");
        Superclass::validateBufferedImage(*image);
        auto storage = Self::New();
        storage->initialize(*image);
        storage->image = std::move(image);
        return storage;
    }
    const itk::ImageBase<3> *imageInformation() const override { return image; }
    typename Image::ConstPointer readImageRegion(const Region &region) const override { validate(region); return image; }
    std::size_t storedBytes() const override { return image->GetPixelContainer()->Capacity() * sizeof(TPixel); }
    TPixel GetPixel(const Index &index) const override {
        validate(Region(index, {1, 1, 1}));
        std::shared_lock lock(mutex);
        return image->GetPixel(index);
    }
    void readRegion(const Region &region, TPixel *output, std::size_t count) const override {
        validate(region);
        if (!output || count != byteCount(region) / sizeof(TPixel)) throw std::invalid_argument("ROI output size differs");
        std::shared_lock lock(mutex);
        rows(region, [&](TPixel *row, std::size_t width) { std::copy_n(row, width, output); output += width; });
    }
    void FillBuffer(TPixel value) override { fillRegion(GetLargestPossibleRegion(), value); }
    void fillRegion(const Region &region, TPixel value) override {
        validate(region);
        std::unique_lock lock(mutex);
        rows(region, [&](TPixel *row, std::size_t width) { std::fill_n(row, width, value); });
        lock.unlock();
        this->Modified();
    }
    void setVoxelValues(const std::vector<Index> &indices, TPixel value) override {
        for (const auto &index : indices) validate(Region(index, {1, 1, 1}));
        if (indices.empty()) return;
        std::unique_lock lock(mutex);
        for (const auto &index : indices) image->SetPixel(index, value);
        lock.unlock();
        this->Modified();
    }
    void fillVoxelRanges(const std::vector<VoxelRange> &ranges) override {
        for (const auto &range : ranges) validate(Region(range.start, {range.length, 1, 1}));
        if (ranges.empty()) return;
        std::unique_lock lock(mutex);
        for (const auto &range : ranges)
            std::fill_n(image->GetBufferPointer() + image->ComputeOffset(range.start), range.length, range.value);
        lock.unlock();
        this->Modified();
    }
    void writeRegion(const Region &region, const TPixel *pixels, std::size_t count) override {
        validate(region);
        if (!pixels || count != byteCount(region) / sizeof(TPixel)) throw std::invalid_argument("ROI input size differs");
        std::unique_lock lock(mutex);
        rows(region, [&](TPixel *row, std::size_t width) { std::copy_n(pixels, width, row); pixels += width; });
        lock.unlock();
        this->Modified();
    }
    void forEachRun(const Region &region, const RunVisitor &visit) const override {
        validate(region);
        std::shared_lock lock(mutex);
        const auto size = region.GetSize();
        const auto origin = region.GetIndex();
        auto index = origin;
        for (std::size_t z = 0; z < size[2]; ++z) {
            index[2] = origin[2] + itk::IndexValueType(z);
            for (std::size_t row = 0; row < size[1]; ++row) {
                index[0] = origin[0];
                index[1] = origin[1] + itk::IndexValueType(row);
                const auto *pixels = image->GetBufferPointer() + image->ComputeOffset(index);
                for (std::size_t begin = 0; begin < size[0];) {
                    auto end = begin + 1;
                    while (end < size[0] && pixels[end] == pixels[begin]) ++end;
                    index[0] = origin[0] + itk::IndexValueType(begin);
                    visit(index, end - begin, pixels[begin]);
                    begin = end;
                }
            }
        }
    }

protected:
    DenseImageStorage() = default;

private:
    using Superclass::validate;
    using Superclass::mutex;
    template<typename Visit>
    void rows(const Region &region, Visit visit) const {
        const auto start = this->localIndex(region.GetIndex());
        const auto full = GetLargestPossibleRegion().GetSize(), size = region.GetSize();
        auto *pixels = image->GetBufferPointer();
        for (std::size_t z = start[2]; z < start[2] + size[2]; ++z)
            for (std::size_t y = start[1]; y < start[1] + size[1]; ++y)
                visit(pixels + (z * full[1] + y) * full[0] + start[0], size[0]);
    }
    typename Image::Pointer image;
};

} // namespace segment_puzzler
