#include "src/image_storage/StreamVByteImageStorage.h"
#include "src/image_storage/DenseImageStorage.h"
#include <itkMetaDataObject.h>
#include <itkCommand.h>
#include <atomic>
#include <iostream>
#include <random>
#include <thread>

#define require(condition) do { if (!(condition)) throw std::runtime_error("RLE test failed at line " + std::to_string(__LINE__)); } while (false)
template<typename Function> void mustThrow(Function function) {
    bool threw = false;
    try { function(); } catch (const std::exception &) { threw = true; }
    require(threw);
}

template<typename Storage>
typename Storage::Pointer buildStorage(const typename Storage::Image &image) {
    if constexpr (std::is_same_v<Storage, segment_puzzler::DenseImageStorage<typename Storage::PixelType>>) {
        auto storage = Storage::create(image);
        const auto region = image.GetLargestPossibleRegion();
        storage->writeRegion(region, image.GetBufferPointer(), region.GetNumberOfPixels());
        return storage;
    } else return Storage::fromImage(image, 3);
}

template<typename Storage> void checkStorage() {
    using Pixel = typename Storage::PixelType;
    using Image = typename Storage::Image;
    using Index = typename Image::IndexType;
    using Region = typename Image::RegionType;
    auto image = Image::New();
    const Region region(Index{-13, 21, -5}, {67, 39, 11});
    image->SetRegions(region);
    const double spacing[]{0.4, 0.7, 1.3}, origin[]{4.0, -7.0, 2.5};
    image->SetSpacing(spacing);
    image->SetOrigin(origin);
    typename Image::DirectionType direction;
    direction.SetIdentity();
    direction[0][0] = 0; direction[1][1] = 0; direction[0][1] = -1; direction[1][0] = 1;
    image->SetDirection(direction);
    itk::EncapsulateMetaData<std::string>(image->GetMetaDataDictionary(), "storage-test", "label geometry");
    image->Allocate(false);
    std::mt19937 random(17);
    for (std::size_t i = 0; i < region.GetNumberOfPixels(); ++i)
        image->GetBufferPointer()[i] = i % 17 < 13 ? Pixel(i / 97) : Pixel(random());
    image->GetBufferPointer()[0] = std::numeric_limits<Pixel>::lowest();
    image->GetBufferPointer()[region.GetNumberOfPixels() - 1] = std::numeric_limits<Pixel>::max();
    typename segment_puzzler::ImageStorage<typename Storage::PixelType>::Pointer storage = buildStorage<Storage>(*image);
    const auto check = [&] {
        auto dense = storage->denseRegion(region);
        require(dense->GetLargestPossibleRegion() == region);
        require(dense->GetSpacing() == image->GetSpacing() && dense->GetOrigin() == image->GetOrigin());
        require(dense->GetDirection() == image->GetDirection());
        std::string metadata;
        require(itk::ExposeMetaData<std::string>(dense->GetMetaDataDictionary(), "storage-test", metadata));
        require(metadata == "label geometry");
        dense->SetPixel(region.GetIndex(), Pixel(11));
        require(storage->GetPixel(region.GetIndex()) == image->GetPixel(region.GetIndex()));
        dense->SetPixel(region.GetIndex(), image->GetPixel(region.GetIndex()));
        require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + region.GetNumberOfPixels(), dense->GetBufferPointer()));
        for (unsigned axis = 0; axis < 3; ++axis)
            for (std::size_t slice = 0; slice < region.GetSize()[axis]; ++slice) {
                std::vector<Pixel> actual;
                storage->readSlice(axis, region.GetIndex()[axis] + slice, actual);
                std::size_t offset = 0;
                for (std::size_t z = 0; z < region.GetSize()[2]; ++z)
                    for (std::size_t y = 0; y < region.GetSize()[1]; ++y)
                        for (std::size_t x = 0; x < region.GetSize()[0]; ++x) {
                            const std::array<std::size_t, 3> point{x, y, z};
                            if (point[axis] == slice) require(actual[offset++] == image->GetPixel(Index{-13 + long(x), 21 + long(y), -5 + long(z)}));
                        }
                require(offset == actual.size());
            }
        typename Storage::LabelStatisticsMap expected;
        for (std::size_t z = 0; z < region.GetSize()[2]; ++z)
            for (std::size_t y = 0; y < region.GetSize()[1]; ++y)
                for (std::size_t x = 0; x < region.GetSize()[0]; ++x) {
                    const Index index{-13 + long(x), 21 + long(y), -5 + long(z)};
                    expected[image->GetPixel(index)].include(Region(index, {1, 1, 1}), 1);
                }
        const auto statistics = storage->labelStatistics();
        require(statistics.size() == expected.size());
        unsigned queried = 0;
        for (const auto &[label, entry] : expected) {
            if (queried++ < 16) {
                const auto actual = storage->labelStatistics(label);
                require(actual && actual->voxelCount == entry.voxelCount && actual->bounds == entry.bounds);
            }
            require(statistics.at(label).voxelCount == entry.voxelCount && statistics.at(label).bounds == entry.bounds);
        }
        std::size_t count = 0;
        storage->forEachRun([&](const Index &start, std::size_t length, Pixel label) {
            auto point = start;
            for (std::size_t x = 0; x < length; ++x) { require(image->GetPixel(point) == label); ++point[0]; }
            count += length;
        });
        require(count == region.GetNumberOfPixels());
    };
    check();
    for (unsigned step = 0; step < 8; ++step) {
        const Region patch(Index{-13 + long(random() % 48), 21 + long(random() % 30), -5 + long(random() % 7)}, {19, 9, 4});
        const auto before = storage->GetMTime();
        const Pixel value = step % 2 ? std::numeric_limits<Pixel>::max() : Pixel{};
        if (step % 3 == 0) {
            storage->fillRegion(patch, value);
            for (long z = patch.GetIndex()[2]; z < patch.GetIndex()[2] + 4; ++z)
                for (long y = patch.GetIndex()[1]; y < patch.GetIndex()[1] + 9; ++y)
                    for (long x = patch.GetIndex()[0]; x < patch.GetIndex()[0] + 19; ++x) image->SetPixel({x, y, z}, value);
        } else {
            std::vector<Pixel> pixels(patch.GetNumberOfPixels());
            for (auto &pixel : pixels) pixel = Pixel(random());
            storage->writeRegion(patch, pixels.data(), pixels.size());
            std::size_t offset = 0;
            for (long z = patch.GetIndex()[2]; z < patch.GetIndex()[2] + 4; ++z)
                for (long y = patch.GetIndex()[1]; y < patch.GetIndex()[1] + 9; ++y)
                    for (long x = patch.GetIndex()[0]; x < patch.GetIndex()[0] + 19; ++x) image->SetPixel({x, y, z}, pixels[offset++]);
        }
        require(storage->GetMTime() > before);
        std::vector<Index> points;
        for (unsigned i = 0; i < 100; ++i) points.push_back({-13 + long(random() % 67), 21 + long(random() % 39), -5 + long(random() % 11)});
        for (unsigned i = 0; i < 10; ++i) points.push_back(points[i]);
        storage->setVoxelValues(points, value);
        for (const auto &point : points) { image->SetPixel(point, value); require(storage->GetPixel(point) == value); }
        check();
    }
    const std::vector<typename Storage::VoxelRange> ranges{
        {{-12, 22, -4}, 20, Pixel(17)}, {{-8, 22, -4}, 12, Pixel(18)},
        {{0, 23, 2}, 40, Pixel(19)}, {{-13, 21, -5}, 67, Pixel(20)}};
    storage->fillVoxelRanges(ranges);
    for (const auto &range : ranges)
        for (std::size_t x = 0; x < range.length; ++x) {
            auto index = range.start; index[0] += x; image->SetPixel(index, range.value);
        }
    check();
    const auto rangeMTime = storage->GetMTime();
    mustThrow([&] { storage->fillVoxelRanges({{{0, 23, 2}, 1, Pixel(0)}, {{999, 0, 0}, 1, Pixel(1)}}); });
    mustThrow([&] { storage->fillVoxelRanges({{{0, 23, 2}, 0, Pixel(0)}}); });
    storage->fillVoxelRanges({});
    require(storage->GetMTime() == rangeMTime);
    check();
    const Region adapterRegion(Index{-9, 24, -3}, {13, 7, 3});
    std::size_t covered = 0;
    storage->forEachRun(adapterRegion, [&](const Index &start, std::size_t length, Pixel value) {
        require(adapterRegion.IsInside(start));
        auto end = start; end[0] += length - 1;
        require(adapterRegion.IsInside(end));
        for (std::size_t x = 0; x < length; ++x) {
            auto point = start; point[0] += x;
            require(image->GetPixel(point) == value);
        }
        covered += length;
    });
    require(covered == adapterRegion.GetNumberOfPixels());
    mustThrow([&] { storage->editImageRegion(adapterRegion, [&](auto &edit) {
        edit.FillBuffer(Pixel(3));
        throw std::runtime_error("aborted edit");
    }); });
    check();
    storage->editImageRegion(adapterRegion, [&](auto &edit) {
        require(edit.GetBufferedRegion() == adapterRegion);
        edit.SetPixel(adapterRegion.GetIndex(), Pixel(5));
    });
    image->SetPixel(adapterRegion.GetIndex(), Pixel(5));
    check();
    image->SetPixel(region.GetIndex(), Pixel(21));
    image->GetBufferPointer()[region.GetNumberOfPixels() - 1] = Pixel(22);
    storage->writeRegion(region, image->GetBufferPointer(), region.GetNumberOfPixels());
    check();
    auto planes = region;
    planes.SetIndex(2, region.GetIndex()[2] + 2);
    planes.SetSize(2, 3);
    image->SetPixel(planes.GetIndex(), Pixel(23));
    storage->writeRegion(planes, image->GetBufferPointer() + image->ComputeOffset(planes.GetIndex()), planes.GetNumberOfPixels());
    check();
    const auto beforeInvalid = storage->GetMTime();
    mustThrow([&] { storage->setVoxelValues({Index{-13, 21, -5}, Index{999, 0, 0}}, Pixel(8)); });
    mustThrow([&] { storage->fillRegion(Region(Index{-13, 21, -5}, {68, 39, 11}), Pixel(9)); });
    mustThrow([&] { storage->denseRegion(Region(Index{std::numeric_limits<itk::IndexValueType>::max(), 0, 0}, {2, 1, 1})); });
    mustThrow([&] { storage->writeRegion(region, image->GetBufferPointer(), 1); });
    mustThrow([&] { storage->writeRegion(region, nullptr, region.GetNumberOfPixels()); });
    mustThrow([&] { storage->readRegion(region, nullptr, region.GetNumberOfPixels()); });
    mustThrow([&] { storage->fillRegion(Region(region.GetIndex(), {0, 1, 1}), Pixel(4)); });
    mustThrow([&] { std::vector<Pixel> slice; storage->readSlice(3, 0, slice); });
    storage->setVoxelValues({}, Pixel(8));
    require(storage->GetMTime() == beforeInvalid);
    check();
    storage->FillBuffer(Pixel(7)); image->FillBuffer(Pixel(7));
    if constexpr (!std::is_same_v<Storage, segment_puzzler::DenseImageStorage<Pixel>>)
        require(storage->storedBytes() < 4096);
    storage->SetPixel({-13, 21, -5}, Pixel(9)); image->SetPixel({-13, 21, -5}, Pixel(9));
    check();
    storage->SetPixel({-13, 21, -5}, Pixel(7)); image->SetPixel({-13, 21, -5}, Pixel(7));
    if constexpr (!std::is_same_v<Storage, segment_puzzler::DenseImageStorage<Pixel>>)
        require(storage->storedBytes() < 4096);
    check();
}

template<typename Storage> void checkLongRuns(itk::SizeValueType length) {
    using Image = typename Storage::Image;
    for (const auto size : {typename Image::SizeType{length, 1, 1}, typename Image::SizeType{1, length, 1}}) {
        auto image = Image::New();
        image->SetRegions(size);
        image->Allocate(false);
        image->FillBuffer(17);
        image->GetBufferPointer()[0] = UINT32_MAX;
        image->GetBufferPointer()[length / 2] = 23;
        image->GetBufferPointer()[length - 1] = 42;
        typename segment_puzzler::ImageStorage<typename Storage::PixelType>::Pointer storage = buildStorage<Storage>(*image);
        auto dense = storage->denseRegion(image->GetLargestPossibleRegion());
        require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + length, dense->GetBufferPointer()));
        std::vector<std::uint32_t> slice;
        storage->readSlice(0, 0, slice);
        require(slice.front() == UINT32_MAX);
        storage->readSlice(0, size[0] - 1, slice);
        require(slice.back() == 42);
    }
}

template<typename Storage> void checkEmptyAndConcurrent() {
    auto information = itk::ImageBase<3>::New();
    information->SetRegions(typename Storage::Region(typename Storage::Index{0, 0, 0}, {2048, 2048, 5000}));
    auto empty = Storage::create(*information);
    require(empty->storedBytes() < 2 * 1024 * 1024);
    unsigned notifications = 0;
    auto observer = itk::FunctionCommand::New();
    observer->SetCallback([&](const itk::EventObject &) {
        require(empty->GetPixel({0, 0, 0}) == 0);
        ++notifications;
    });
    empty->AddObserver(itk::ModifiedEvent(), observer);
    empty->SetPixel({2047, 2047, 4999}, UINT32_MAX);
    require(empty->GetPixel({2047, 2047, 4999}) == UINT32_MAX);
    require(empty->GetPixel({0, 0, 0}) == 0);
    empty->FillBuffer(0);
    require(notifications == 2);
    information->SetRegions(typename Storage::Region(typename Storage::Index{0, 0, 0}, {65536, 1, 1}));
    auto wide = Storage::create(*information);
    wide->SetPixel({65535, 0, 0}, 18);
    require(wide->GetPixel({65535, 0, 0}) == 18);
    std::atomic<bool> failed{false};
    std::thread reader([&] {
        for (unsigned i = 0; i < 200; ++i) {
            std::vector<std::uint32_t> slice; wide->readSlice(2, 0, slice);
            if (slice.back() != 18 && slice.back() != 19) failed = true;
        }
    });
    for (unsigned i = 0; i < 100; ++i) wide->SetPixel({65535, 0, 0}, 18 + i % 2);
    reader.join();
    require(!failed);
}

void checkWidePlaneStatistics() {
    if constexpr (std::numeric_limits<itk::IndexValueType>::digits > 32 && sizeof(std::size_t) > 4) {
        using Storage = segment_puzzler::StreamVByteImageStorage<std::uint32_t>;
        auto information = itk::ImageBase<3>::New();
        const auto tooWide = static_cast<itk::SizeValueType>(UINT32_MAX) + 1;
        information->SetRegions(Storage::Region({0, 0, 0}, {tooWide, 1, 1}));
        mustThrow([&] { Storage::create(*information); });
        information->SetRegions(Storage::Region({0, 0, 0}, {65536, 65536, 1}));
        const auto storage = Storage::create(*information, 7);
        const auto statistics = storage->labelStatistics(7);
        require(statistics && statistics->voxelCount == (std::uint64_t{1} << 32));
        require(statistics->bounds == information->GetLargestPossibleRegion());
    }
}

void checkDenseOwnership() {
    using Storage = segment_puzzler::DenseImageStorage<std::uint32_t>;
    auto image = Storage::Image::New();
    image->SetRegions({9, 7, 3});
    image->Allocate();
    image->FillBuffer(42);
    auto storage = Storage::fromImage(image);
    image = nullptr;
    require(storage->GetPixel({0, 0, 0}) == 42);
    unsigned notifications = 0;
    auto observer = itk::FunctionCommand::New();
    observer->SetCallback([&](const itk::EventObject &) {
        require(storage->GetPixel({0, 0, 0}) == 17);
        ++notifications;
    });
    storage->AddObserver(itk::ModifiedEvent(), observer);
    storage->SetPixel({0, 0, 0}, 17);
    require(notifications == 1);
    require(storage->storedBytes() == 9 * 7 * 3 * sizeof(std::uint32_t));
    auto partial = Storage::Image::New();
    partial->SetRegions({9, 7, 3});
    mustThrow([&] { Storage::fromImage(partial); });
    mustThrow([&] { Storage::fromImage(nullptr); });
}

int main() {
    try {
        checkStorage<segment_puzzler::StreamVByteImageStorage<std::uint8_t>>();
        checkStorage<segment_puzzler::StreamVByteImageStorage<std::uint16_t>>();
        checkStorage<segment_puzzler::StreamVByteImageStorage<std::uint32_t>>();
        checkStorage<segment_puzzler::DenseImageStorage<std::uint32_t>>();
        checkStorage<segment_puzzler::DenseImageStorage<std::int64_t>>();
        checkStorage<segment_puzzler::DenseImageStorage<float>>();
        checkStorage<segment_puzzler::DenseImageStorage<double>>();
        checkLongRuns<segment_puzzler::StreamVByteImageStorage<std::uint32_t>>(65536);
        checkLongRuns<segment_puzzler::DenseImageStorage<std::uint32_t>>(65536);
        checkEmptyAndConcurrent<segment_puzzler::StreamVByteImageStorage<std::uint32_t>>();
        checkWidePlaneStatistics();
        checkDenseOwnership();
        std::cout << "Image storage: geometry, slices, ROIs, batches, invalid writes, wide IDs, compact empty volumes and concurrent reads passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
