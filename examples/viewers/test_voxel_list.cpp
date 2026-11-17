#include "src/utils/voxel.h"
#include "src/utils/roi.h"
#include "src/image_storage/LabelImageStorage.h"
#include <random>

#define require(condition) do { if (!(condition)) throw std::runtime_error("Voxel list test failed at line " + std::to_string(__LINE__)); } while (false)

template<typename Function> void mustThrow(Function function) {
    bool threw = false;
    try { function(); } catch (const std::exception &) { threw = true; }
    require(threw);
}

void checkSequence() {
    VoxelList list;
    require(list.empty() && list.begin() == list.end());
    std::vector<Voxel> reference;
    std::mt19937 random(73);
    for (int i = 0; i < 2000; ++i) {
        const Voxel start(int(random() % 100) - 50, int(random() % 31) - 15, int(random() % 11) - 5);
        const auto length = random() % 50;
        list.appendRun(start, length);
        for (unsigned x = 0; x < length; ++x) reference.emplace_back(start.x + x, start.y, start.z);
        if (i % 7 == 0) { list.push_back(start); reference.push_back(start); }
    }
    require(list.size() == reference.size());
    require(std::vector<Voxel>(list.begin(), list.end()) == reference);
    require(list.front() == reference.front());
    auto iterator = list.begin();
    require(*iterator++ == reference[0] && *iterator == reference[1]);
    auto copied = list;
    auto moved = std::move(copied);
    require(copied.empty() && copied.begin() == copied.end());
    require(std::vector<Voxel>(moved.begin(), moved.end()) == reference);
    VoxelList assigned;
    assigned = std::move(moved);
    require(moved.empty());
    assigned.push_back({-3, 8, 2});
    require(list.size() == reference.size());
    Roi denseRoi, runRoi;
    denseRoi.updateBoundingRoi(reference);
    runRoi.updateBoundingRoi(list);
    require(denseRoi.minXVoxel == runRoi.minXVoxel && denseRoi.maxXVoxel == runRoi.maxXVoxel);
    require(denseRoi.minYVoxel == runRoi.minYVoxel && denseRoi.maxYVoxel == runRoi.maxYVoxel);
    require(denseRoi.minZVoxel == runRoi.minZVoxel && denseRoi.maxZVoxel == runRoi.maxZVoxel);
}

void checkLimits() {
    VoxelList list;
    list.appendRun({-5, 2, 3}, 7);
    list.appendRun({2, 2, 3}, 4);
    list.push_back({6, 2, 3});
    require(list.size() == 12 && list.getRuns().size() == 1);
    list.push_back({6, 2, 3});
    list.push_back({7, 3, 3});
    list.push_back({8, 3, 4});
    require(list.getRuns().size() == 4);
    list.appendRun({std::numeric_limits<int>::max(), 0, 0}, 1);
    mustThrow([&] { list.appendRun({std::numeric_limits<int>::max(), 0, 0}, 2); });
    mustThrow([&] { list.appendRun({0, 0, 0}, std::numeric_limits<std::size_t>::max()); });
    require(list.size() == 16);
    VoxelList large;
    large.appendRun({std::numeric_limits<int>::min(), 0, 0}, std::numeric_limits<std::uint32_t>::max());
    large.push_back({std::numeric_limits<int>::max(), 0, 0});
    require(large.getRuns().size() == 2);
    require(large.size() == std::uint64_t(std::numeric_limits<std::uint32_t>::max()) + 1);
    require(large.storedBytes() < 1024);
}

void checkImageEdits() {
    using namespace segment_puzzler;
    using Image = dataType::SegmentsImageType;
    auto image = Image::New();
    const Image::RegionType region({-12, 8, -3}, {41, 27, 9});
    image->SetRegions(region);
    image->Allocate(true);
    auto expected = Image::New();
    expected->SetRegions(region);
    expected->Allocate(true);
    auto dense = DenseImageStorage<dataType::SegmentIdType>::fromImage(image);
    auto rle = StreamVByteImageStorage<dataType::SegmentIdType>::fromImage(*image);
    std::mt19937 random(91);
    for (unsigned pass = 0; pass < 12; ++pass) {
        VoxelList first, second;
        for (unsigned i = 0; i < 70; ++i) {
            const int x = int(random() % 30) - 12, y = int(random() % 27) + 8, z = int(random() % 9) - 3;
            const unsigned length = random() % 11 + 1;
            (i % 2 ? first : second).appendRun({x, y, z}, length);
        }
        for (const auto *list : VoxelLists{&first, &second}) for (const auto &voxel : *list)
            expected->SetPixel({voxel.x, voxel.y, voxel.z}, pass);
        setVoxelLists(*dense, {&first, &second}, pass);
        setVoxelLists(*rle, {&first, &second}, pass);
        auto actual = rle->denseRegion(region);
        require(std::equal(expected->GetBufferPointer(), expected->GetBufferPointer() + region.GetNumberOfPixels(), actual->GetBufferPointer()));
        require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + region.GetNumberOfPixels(), actual->GetBufferPointer()));
        auto read = readVoxelRegion(*rle, {&first, &second});
        for (const auto *list : VoxelLists{&first, &second}) for (const auto &voxel : *list)
            require(read->GetPixel({voxel.x, voxel.y, voxel.z}) == pass);
    }
    VoxelList empty;
    require(!readVoxelRegion(*rle, {&empty}));
    const auto modified = rle->GetMTime();
    setVoxelLists(*rle, {&empty}, 8);
    require(rle->GetMTime() == modified);
    VoxelList invalid;
    invalid.push_back({-12, 8, -3});
    invalid.push_back({200, 8, 5});
    mustThrow([&] { setVoxelLists(*rle, {&invalid}, 123); });
    require(rle->GetMTime() == modified);
    auto unchanged = rle->denseRegion(region);
    require(std::equal(expected->GetBufferPointer(), expected->GetBufferPointer() + region.GetNumberOfPixels(), unchanged->GetBufferPointer()));
}

int main() {
    try {
        checkSequence();
        checkLimits();
        checkImageEdits();
        std::cout << "Voxel list and image adapter tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
