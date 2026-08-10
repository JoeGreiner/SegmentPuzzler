#include "src/utils/BlockwiseFastMarkerWatershed3D.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

using CostImage = segment_puzzler::FastMarkerWatershedCostImage;
using LabelImage = segment_puzzler::FastMarkerWatershedLabelImage;

struct Images {
    CostImage::Pointer cost;
    LabelImage::Pointer markers;
};

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Images makeImages(const CostImage::SizeType &size, int blockEdge, bool seedEveryBlock) {
    CostImage::IndexType index;
    index[0] = 3;
    index[1] = -2;
    index[2] = 5;
    const CostImage::RegionType region(index, size);

    Images images{CostImage::New(), LabelImage::New()};
    images.cost->SetRegions(region);
    images.cost->Allocate();
    images.markers->SetRegions(region);
    images.markers->Allocate(true);

    float *cost = images.cost->GetBufferPointer();
    const std::size_t voxelCount = region.GetNumberOfPixels();
    for (std::size_t voxel = 0; voxel < voxelCount; ++voxel) {
        cost[voxel] = static_cast<float>((voxel * 17 + voxel / 23) % 97);
    }

    unsigned int nextLabel = 1;
    const std::size_t blockCountX = (size[0] + blockEdge - 1) / blockEdge;
    const std::size_t blockCountY = (size[1] + blockEdge - 1) / blockEdge;
    const std::size_t blockCountZ = (size[2] + blockEdge - 1) / blockEdge;
    for (std::size_t bz = 0; bz < blockCountZ; ++bz) {
        for (std::size_t by = 0; by < blockCountY; ++by) {
            for (std::size_t bx = 0; bx < blockCountX; ++bx) {
                if (!seedEveryBlock && (bx != 0 || by != 0 || bz != 0)) {
                    continue;
                }
                LabelImage::IndexType markerIndex;
                markerIndex[0] = index[0] + static_cast<LabelImage::IndexValueType>(
                    std::min<std::size_t>(bx * blockEdge + blockEdge / 2, size[0] - 1));
                markerIndex[1] = index[1] + static_cast<LabelImage::IndexValueType>(
                    std::min<std::size_t>(by * blockEdge + blockEdge / 2, size[1] - 1));
                markerIndex[2] = index[2] + static_cast<LabelImage::IndexValueType>(
                    std::min<std::size_t>(bz * blockEdge + blockEdge / 2, size[2] - 1));
                images.markers->SetPixel(markerIndex, nextLabel++);
            }
        }
    }
    return images;
}

bool imagesEqual(LabelImage::Pointer left, LabelImage::Pointer right) {
    if (left->GetLargestPossibleRegion() != right->GetLargestPossibleRegion()) {
        return false;
    }
    const std::size_t voxelCount = left->GetLargestPossibleRegion().GetNumberOfPixels();
    return std::equal(
        left->GetBufferPointer(), left->GetBufferPointer() + voxelCount, right->GetBufferPointer());
}

void testAutomaticBlockEdge() {
    CostImage::SizeType size;
    size[0] = 672;
    size[1] = 402;
    size[2] = 501;
    require(segment_puzzler::automaticWatershedBlockEdge(size, 16) == 160,
            "Expected a 160-voxel block edge for the lightsheet stack.");

    size.Fill(32);
    require(segment_puzzler::automaticWatershedBlockEdge(size, 64) == 128,
            "Automatic block edge must respect the 128 minimum.");
    size.Fill(4096);
    require(segment_puzzler::automaticWatershedBlockEdge(size, 1) == 256,
            "Automatic block edge must respect the 256 maximum.");
}

void testDeterministicCoverageAndMarkers() {
    CostImage::SizeType size;
    size[0] = 57;
    size[1] = 43;
    size[2] = 35;
    constexpr int blockEdge = 20;
    auto images = makeImages(size, blockEdge, true);

    segment_puzzler::BlockwiseFastMarkerWatershedOptions options;
    options.blockEdge = blockEdge;
    options.halo = 4;
    options.threadCount = 1;
    segment_puzzler::BlockwiseFastMarkerWatershedMetrics singleThreadMetrics;
    auto singleThread = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
        images.cost, images.markers, options, &singleThreadMetrics);
    require(!singleThreadMetrics.usedGlobalFallback, "Dense markers should not use the global fallback.");

    options.threadCount = 4;
    segment_puzzler::BlockwiseFastMarkerWatershedMetrics parallelMetrics;
    auto parallel = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
        images.cost, images.markers, options, &parallelMetrics);
    require(!parallelMetrics.usedGlobalFallback, "Parallel dense run unexpectedly used the global fallback.");
    require(parallelMetrics.passes.size() == parallelMetrics.passCount,
            "Every blockwise pass must expose profiling metrics.");
    for (const auto &pass : parallelMetrics.passes) {
        require(pass.scheduledBlockCount == pass.completedBlockCount + pass.deferredBlockCount,
                "Blockwise pass counters must be internally consistent.");
        require(pass.scheduledBlockCount == 0 || pass.threadsUsed > 0,
                "A non-empty blockwise pass must report at least one worker thread.");
    }
    require(imagesEqual(singleThread, parallel), "Blockwise result must not depend on scheduling.");

    options.watershed.compactness =
        segment_puzzler::kDefaultFastMarkerWatershedCompactness;
    options.threadCount = 1;
    auto compactSingleThread = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
        images.cost, images.markers, options);
    options.threadCount = 4;
    auto compactParallel = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
        images.cost, images.markers, options);
    require(imagesEqual(compactSingleThread, compactParallel),
            "Compact blockwise result must not depend on scheduling.");
    require(!imagesEqual(singleThread, compactSingleThread),
            "Compactness should affect the blockwise result.");

    const std::size_t voxelCount = singleThread->GetLargestPossibleRegion().GetNumberOfPixels();
    require(std::none_of(singleThread->GetBufferPointer(), singleThread->GetBufferPointer() + voxelCount,
                         [](unsigned int label) { return label == 0; }),
            "Every voxel must be covered.");
    for (std::size_t voxel = 0; voxel < voxelCount; ++voxel) {
        if (images.markers->GetBufferPointer()[voxel] != 0) {
            require(singleThread->GetBufferPointer()[voxel] == images.markers->GetBufferPointer()[voxel],
                    "Original marker labels must be preserved.");
        }
    }
}

void testSparseMarkerPropagationAndFallback() {
    CostImage::SizeType size;
    size.Fill(48);
    auto images = makeImages(size, 16, false);

    segment_puzzler::BlockwiseFastMarkerWatershedOptions options;
    options.threadCount = 4;
    options.blockEdge = 16;
    options.halo = 2;
    segment_puzzler::BlockwiseFastMarkerWatershedMetrics metrics;
    auto blockwise = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
        images.cost, images.markers, options, &metrics);
    const std::size_t voxelCount = blockwise->GetLargestPossibleRegion().GetNumberOfPixels();
    require(!metrics.usedGlobalFallback, "A sparse marker must propagate across deferred blocks.");
    require(std::none_of(blockwise->GetBufferPointer(), blockwise->GetBufferPointer() + voxelCount,
                         [](unsigned int label) { return label == 0; }),
            "Sparse marker propagation must cover the full image.");

    std::fill_n(images.markers->GetBufferPointer(), voxelCount, 0u);
    auto fallback = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
        images.cost, images.markers, options, &metrics);
    auto global = segment_puzzler::runFastMarkerWatershed3D(images.cost, images.markers);
    require(metrics.usedGlobalFallback, "No-marker input must trigger the safe global fallback.");
    require(imagesEqual(fallback, global), "Fallback result must equal the global fast marker result.");
}

} // namespace

int main() {
    try {
        testAutomaticBlockEdge();
        testDeterministicCoverageAndMarkers();
        testSparseMarkerPropagationAndFallback();
        std::cout << "Blockwise fast marker watershed tests passed.\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "Blockwise fast marker watershed test failed: " << exception.what() << '\n';
        return 1;
    }
}
