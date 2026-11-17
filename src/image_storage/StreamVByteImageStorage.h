#pragma once

#include "ImageStorage.h"
#include "StreamVBytePlane.h"
#include <itkMultiThreaderBase.h>
#include <itkObjectFactory.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace segment_puzzler {

// X/Y runs with constant values for uniform Z planes. Batch writes rebuild affected planes.
template<typename TPixel>
class StreamVByteImageStorage final : public ImageStorage<TPixel> {
public:
    using Self = StreamVByteImageStorage;
    using Pointer = itk::SmartPointer<Self>;
    using Superclass = ImageStorage<TPixel>;
    using typename Superclass::Image;
    using typename Superclass::Region;
    using typename Superclass::Index;
    using typename Superclass::RunVisitor;
    using typename Superclass::VoxelRange;
    using typename Superclass::LabelStatistics;
    using typename Superclass::LabelStatisticsMap;
    using Superclass::GetLargestPossibleRegion;
    using Superclass::byteCount;
    using Superclass::forEachRun;
    itkNewMacro(Self);
    itkTypeMacro(StreamVByteImageStorage, ImageStorage);

    static Pointer create(const itk::ImageBase<3> &information, TPixel value = {}) {
        const auto region = information.GetLargestPossibleRegion();
        const auto size = region.GetSize();
        auto storage = Self::New();
        storage->initialize(information);
        if (size[0] > std::numeric_limits<std::uint32_t>::max() || size[1] > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("Image width exceeds the RLE position type");
        storage->slices.resize(size[2], uniformSlice(value, size[0], size[1]));
        return storage;
    }

    // Builds an independent compressed copy; release the source image to reclaim its buffer.
    static Pointer fromImage(const Image &image, unsigned threads = itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads()) {
        const auto region = image.GetLargestPossibleRegion();
        Superclass::validateBufferedImage(image);
        auto storage = create(image);
        const auto size = region.GetSize();
        auto parallel = itk::MultiThreaderBase::New();
        parallel->SetMaximumNumberOfThreads(std::max(1U, threads));
        parallel->SetNumberOfWorkUnits(std::max(1U, threads));
        parallel->ParallelizeArray(0, size[2], [&](itk::SizeValueType z) {
            const auto *pixels = image.GetBufferPointer() + z * size[0] * size[1];
            storage->slices[z] = buildSlice(pixels, size[0], size[1]);
        }, nullptr);
        return storage;
    }

    std::size_t storedBytes() const override {
        std::shared_lock lock(mutex);
        std::size_t bytes = slices.capacity() * sizeof(Slice);
        for (const auto &slice : slices) {
            for (const auto &plane : slice.directions) bytes += plane.bytes();
            bytes += slice.labels.capacity() * sizeof(PlaneLabel);
        }
        return bytes;
    }
    TPixel GetPixel(const Index &index) const override {
        validate(Region(index, {1, 1, 1}));
        const auto local = localIndex(index);
        std::shared_lock lock(mutex);
        std::vector<std::uint32_t> scratch;
        TPixel value{};
        slices[local[2]].directions[0].visit(local[1], local[0], local[0] + 1,
            [&](TPixel label, auto, auto) { value = label; }, scratch);
        return value;
    }

    void readRegion(const Region &region, TPixel *output, std::size_t count) const override {
        validate(region);
        if (!output || count != byteCount(region) / sizeof(TPixel))
            throw std::invalid_argument("ROI output size differs");
        const auto start = localIndex(region.GetIndex());
        const auto size = region.GetSize();
        std::shared_lock lock(mutex);
        std::vector<std::uint32_t> scratch;
        const unsigned along = size[0] == 1 ? 1 : 0;
        const unsigned across = 1 - along;
        for (std::size_t z = start[2]; z < start[2] + size[2]; ++z)
            for (std::size_t row = start[across]; row < start[across] + size[across]; ++row) {
                slices[z].directions[along].visit(row, start[along], start[along] + size[along],
                    [&](TPixel label, auto begin, auto end) {
                        std::fill_n(output, end - begin, label);
                        output += end - begin;
                    }, scratch);
            }
    }
    void forEachRun(const Region &region, const RunVisitor &visit) const override {
        validate(region);
        std::shared_lock lock(mutex);
        const auto size = region.GetSize();
        const auto start = localIndex(region.GetIndex());
        const auto origin = GetLargestPossibleRegion().GetIndex();
        std::vector<std::uint32_t> scratch;
        for (std::size_t z = start[2]; z < start[2] + size[2]; ++z)
            for (std::size_t y = start[1]; y < start[1] + size[1]; ++y)
                slices[z].directions[0].visit(y, start[0], start[0] + size[0], [&](TPixel label, auto begin, auto end) {
                    visit(Index{origin[0] + itk::IndexValueType(begin), origin[1] + itk::IndexValueType(y),
                                origin[2] + itk::IndexValueType(z)}, end - begin, label);
                }, scratch);
    }

    void FillBuffer(TPixel value) override {
        const auto size = GetLargestPossibleRegion().GetSize();
        std::vector<Slice> replacement(size[2], uniformSlice(value, size[0], size[1]));
        std::unique_lock lock(mutex);
        slices.swap(replacement);
        lock.unlock();
        this->Modified();
    }
    void fillRegion(const Region &region, TPixel value) override {
        validate(region);
        if (region == GetLargestPossibleRegion()) { FillBuffer(value); return; }
        const auto start = localIndex(region.GetIndex());
        const auto size = region.GetSize();
        std::array<std::vector<RowEdit>, 2> edits;
        for (unsigned along = 0; along < 2; ++along) {
            const unsigned across = 1 - along;
            for (std::size_t z = start[2]; z < start[2] + size[2]; ++z)
                for (std::size_t row = start[across]; row < start[across] + size[across]; ++row)
                    edits[along].push_back({z, row, start[along], start[along] + size[along], value});
        }
        apply(edits);
    }
    void setVoxelValues(const std::vector<Index> &indices, TPixel value) override {
        std::array<std::vector<RowEdit>, 2> edits;
        for (auto &direction : edits) direction.reserve(indices.size());
        for (const auto &index : indices) {
            validate(Region(index, {1, 1, 1}));
            const auto local = localIndex(index);
            for (unsigned along = 0; along < 2; ++along)
                edits[along].push_back({local[2], local[1 - along], local[along], local[along] + 1, value});
        }
        for (auto &direction : edits) {
            std::sort(direction.begin(), direction.end(), [](const RowEdit &a, const RowEdit &b) {
                return std::tie(a.z, a.row, a.begin) < std::tie(b.z, b.row, b.begin);
            });
            auto output = direction.begin();
            for (const auto &edit : direction) {
                if (output != direction.begin() && (output - 1)->z == edit.z && (output - 1)->row == edit.row &&
                    (output - 1)->end >= edit.begin) (output - 1)->end = std::max((output - 1)->end, edit.end);
                else *output++ = edit;
            }
            direction.erase(output, direction.end());
        }
        apply(edits);
    }
    void fillVoxelRanges(const std::vector<VoxelRange> &ranges) override {
        for (const auto &range : ranges) validate(Region(range.start, {range.length, 1, 1}));
        if (ranges.empty()) return;
        auto ordered = ranges;
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.start[2] < b.start[2]; });
        std::vector<std::size_t> offsets{0};
        for (std::size_t i = 1; i < ordered.size(); ++i)
            if (ordered[i].start[2] != ordered[i - 1].start[2]) offsets.push_back(i);
        offsets.push_back(ordered.size());
        std::vector<Slice> replacement(offsets.size() - 1);
        std::unique_lock lock(mutex);
        itk::MultiThreaderBase::New()->ParallelizeArray(0, replacement.size(), [&](itk::SizeValueType i) {
            auto first = ordered[offsets[i]].start, last = first;
            for (auto j = offsets[i]; j < offsets[i + 1]; ++j) {
                const auto &range = ordered[j];
                first[0] = std::min(first[0], range.start[0]); first[1] = std::min(first[1], range.start[1]);
                last[0] = std::max(last[0], range.start[0] + itk::IndexValueType(range.length - 1));
                last[1] = std::max(last[1], range.start[1]);
            }
            Region region; region.SetIndex(first); region.SetUpperIndex(last);
            const auto start = localIndex(first);
            const auto width = region.GetSize()[0], height = region.GetSize()[1];
            const auto &slice = slices[start[2]];
            std::vector<TPixel> pixels(width * height);
            std::vector<std::uint32_t> scratch;
            for (std::size_t y = 0; y < height; ++y)
                slice.directions[0].visit(start[1] + y, start[0], start[0] + width,
                    [&](TPixel value, auto begin, auto end) {
                        std::fill_n(pixels.data() + (y * width + begin - start[0]), end - begin, value);
                    }, scratch);
            for (auto j = offsets[i]; j < offsets[i + 1]; ++j) {
                const auto &range = ordered[j];
                std::fill_n(pixels.data() + ((range.start[1] - first[1]) * width + (range.start[0] - first[0])),
                            range.length, range.value);
            }
            replacement[i] = editedSlice(slice, pixels.data(), region);
        }, nullptr);
        for (std::size_t i = 0; i < replacement.size(); ++i)
            slices[localIndex(ordered[offsets[i]].start)[2]] = std::move(replacement[i]);
        lock.unlock();
        this->Modified();
    }
    LabelStatisticsMap labelStatistics() const override {
        std::shared_lock lock(mutex);
        LabelStatisticsMap result;
        for (std::size_t z = 0; z < slices.size(); ++z)
            for (const auto &label : slices[z].labels)
                result[label.value].include(labelRegion(label, z), label.count);
        return result;
    }
    std::optional<LabelStatistics> labelStatistics(TPixel value) const override {
        std::shared_lock lock(mutex);
        LabelStatistics result;
        for (std::size_t z = 0; z < slices.size(); ++z) {
            const auto &labels = slices[z].labels;
            const auto found = std::lower_bound(labels.begin(), labels.end(), value,
                [](const auto &label, TPixel target) { return label.value < target; });
            if (found != labels.end() && found->value == value)
                result.include(labelRegion(*found, z), found->count);
        }
        return result.voxelCount ? std::optional<LabelStatistics>(result) : std::nullopt;
    }
    void writeRegion(const Region &region, const TPixel *pixels, std::size_t count) override {
        validate(region);
        if (!pixels || count != byteCount(region) / sizeof(TPixel)) throw std::invalid_argument("ROI input size differs");
        const auto start = localIndex(region.GetIndex());
        const auto size = region.GetSize();
        std::vector<Slice> replacement(size[2]);
        std::unique_lock lock(mutex);
        itk::MultiThreaderBase::New()->ParallelizeArray(0, size[2], [&](itk::SizeValueType z) {
            auto planeRegion = region;
            planeRegion.SetIndex(2, region.GetIndex()[2] + z); planeRegion.SetSize(2, 1);
            replacement[z] = editedSlice(slices[start[2] + z], pixels + z * size[0] * size[1], planeRegion);
        }, nullptr);
        for (std::size_t z = 0; z < size[2]; ++z) slices[start[2] + z] = std::move(replacement[z]);
        lock.unlock();
        this->Modified();
    }

protected:
    StreamVByteImageStorage() = default;

private:
    using Plane = detail::StreamVBytePlane<TPixel>;
    using RowEdit = typename Plane::RowEdit;
    using Superclass::validate;
    using Superclass::localIndex;
    using Superclass::mutex;
    struct PlaneLabel { TPixel value; std::uint32_t minX, maxX, minY, maxY; std::size_t count; };
    struct Slice { std::array<Plane, 2> directions; std::vector<PlaneLabel> labels; };
    static Slice uniformSlice(TPixel value, std::size_t width, std::size_t height) {
        return {{Plane(value), Plane(value)}, {{value, 0, static_cast<std::uint32_t>(width - 1),
            0, static_cast<std::uint32_t>(height - 1), width * height}}};
    }
    static std::vector<PlaneLabel> summarize(const Plane &plane, std::size_t width, std::size_t height) {
        if (plane.uniform()) return uniformSlice(plane.constant, width, height).labels;
        std::unordered_map<TPixel, PlaneLabel> labels;
        std::vector<std::uint32_t> scratch;
        for (std::size_t y = 0; y < height; ++y)
            plane.visit(y, 0, width, [&](TPixel value, auto begin, auto end) {
                auto [entry, added] = labels.try_emplace(value, PlaneLabel{value, static_cast<std::uint32_t>(begin),
                    static_cast<std::uint32_t>(end - 1), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(y), 0});
                auto &label = entry->second;
                label.minX = std::min(label.minX, static_cast<std::uint32_t>(begin));
                label.maxX = std::max(label.maxX, static_cast<std::uint32_t>(end - 1));
                label.maxY = static_cast<std::uint32_t>(y);
                label.count += end - begin;
            }, scratch);
        std::vector<PlaneLabel> result;
        result.reserve(labels.size());
        for (const auto &entry : labels) result.push_back(entry.second);
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.value < b.value; });
        return result;
    }
    static Slice buildSlice(const TPixel *pixels, std::size_t width, std::size_t height) {
        Slice slice;
        slice.directions[0] = Plane::build(pixels, width, height);
        slice.directions[1] = slice.directions[0].uniform() ? Plane(pixels[0]) : Plane::buildColumns(pixels, width, height);
        slice.labels = summarize(slice.directions[0], width, height);
        return slice;
    }
    Slice editedSlice(const Slice &previous, const TPixel *pixels, const Region &region) const {
        const auto size = region.GetSize(), fullSize = GetLargestPossibleRegion().GetSize();
        if (size[0] == fullSize[0] && size[1] == fullSize[1]) return buildSlice(pixels, size[0], size[1]);
        const auto start = localIndex(region.GetIndex());
        Slice result;
        for (unsigned along = 0; along < 2; ++along) {
            std::vector<RowEdit> edits;
            for (std::size_t row = 0; row < size[1 - along]; ++row) {
                const auto at = [&](std::size_t position) {
                    return pixels[along == 0 ? position + row * size[0] : row + position * size[0]];
                };
                for (std::size_t begin = 0; begin < size[along];) {
                    auto end = begin + 1;
                    while (end < size[along] && at(end) == at(begin)) ++end;
                    edits.push_back({start[2], start[1 - along] + row, start[along] + begin, start[along] + end, at(begin)});
                    begin = end;
                }
            }
            result.directions[along] = previous.directions[along].edited(fullSize[along], fullSize[1 - along],
                edits.data(), edits.data() + edits.size());
        }
        result.labels = summarize(result.directions[0], fullSize[0], fullSize[1]);
        return result;
    }
    Region labelRegion(const PlaneLabel &label, std::size_t z) const {
        auto start = GetLargestPossibleRegion().GetIndex();
        start[0] += label.minX; start[1] += label.minY; start[2] += z;
        return Region(start, {std::size_t(label.maxX) - label.minX + 1, std::size_t(label.maxY) - label.minY + 1, 1});
    }
    void apply(const std::array<std::vector<RowEdit>, 2> &edits) {
        if (edits[0].empty()) return;
        const auto size = GetLargestPossibleRegion().GetSize();
        std::unique_lock lock(mutex);
        struct Group { std::size_t z; std::array<std::size_t, 2> begin, end; };
        std::vector<Group> groups;
        std::array<std::size_t, 2> positions{};
        while (positions[0] < edits[0].size()) {
            const auto z = edits[0][positions[0]].z;
            const auto begin = positions;
            for (unsigned along = 0; along < 2; ++along)
                while (positions[along] < edits[along].size() && edits[along][positions[along]].z == z) ++positions[along];
            groups.push_back({z, begin, positions});
        }
        std::vector<Slice> replacement(groups.size());
        itk::MultiThreaderBase::New()->ParallelizeArray(0, groups.size(), [&](itk::SizeValueType i) {
            const auto &group = groups[i];
            auto &slice = replacement[i];
            for (unsigned along = 0; along < 2; ++along)
                slice.directions[along] = slices[group.z].directions[along].edited(size[along], size[1 - along],
                    edits[along].data() + group.begin[along], edits[along].data() + group.end[along]);
            slice.labels = summarize(slice.directions[0], size[0], size[1]);
        }, nullptr);
        for (std::size_t i = 0; i < groups.size(); ++i) slices[groups[i].z] = std::move(replacement[i]);
        lock.unlock();
        this->Modified();
    }
    std::vector<Slice> slices;
};

} // namespace segment_puzzler
