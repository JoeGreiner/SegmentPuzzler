#include "itkWatershedHelpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

#include "itkBinaryThresholdImageFilter.h"
#include <itkCastImageFilter.h>
#include <itkChangeLabelImageFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkDiscreteGaussianImageFilter.h>
#include <itkImageRegionIterator.h>
#include <itkInvertIntensityImageFilter.h>
#include <itkLabelImageToShapeLabelMapFilter.h>
#include <itkMorphologicalWatershedFromMarkersImageFilter.h>
#include <itkSignedMaurerDistanceMapImageFilter.h>
#include <itkStatisticsImageFilter.h>

#include "src/utils/DistanceMapFH3D.h"
#include "src/utils/FastMarkerWatershed3D.h"

namespace {

using SegmentsImageType = dataType::SegmentsImageType;
using SegmentIdType = dataType::SegmentIdType;

std::mutex &watershedLogMutex() {
    static std::mutex mutex;
    return mutex;
}

WatershedLogSink &watershedLogSinkStorage() {
    static WatershedLogSink sink;
    return sink;
}

void emitWatershedLog(const std::string &message) {
    std::lock_guard<std::mutex> guard(watershedLogMutex());
    if (const auto &sink = watershedLogSinkStorage()) {
        sink(message);
        return;
    }
    std::cout << message << std::endl;
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

SegmentsImageType::Pointer cloneSegmentsImageMetadata(SegmentsImageType::Pointer source) {
    auto image = SegmentsImageType::New();
    image->SetRegions(source->GetLargestPossibleRegion());
    image->SetSpacing(source->GetSpacing());
    image->SetOrigin(source->GetOrigin());
    image->SetDirection(source->GetDirection());
    image->Allocate();
    return image;
}

SegmentsImageType::Pointer copySegmentsImage(SegmentsImageType::Pointer source) {
    auto image = cloneSegmentsImageMetadata(source);
    std::copy(source->GetBufferPointer(),
              source->GetBufferPointer() + source->GetLargestPossibleRegion().GetNumberOfPixels(),
              image->GetBufferPointer());
    return image;
}

struct BoundaryComponentRecord {
    SegmentIdType id = 0;
    SegmentIdType originalLabel = 0;
};

class BoundaryComponentDisjointSet {
public:
    explicit BoundaryComponentDisjointSet(std::size_t componentCount)
        : parents(componentCount + 1) {
        std::iota(parents.begin(), parents.end(), SegmentIdType{0});
    }

    SegmentIdType find(SegmentIdType componentId) {
        SegmentIdType root = componentId;
        while (parents[root] != root) {
            root = parents[root];
        }
        while (parents[componentId] != componentId) {
            const SegmentIdType parent = parents[componentId];
            parents[componentId] = root;
            componentId = parent;
        }
        return root;
    }

    void unite(SegmentIdType first, SegmentIdType second) {
        first = find(first);
        second = find(second);
        if (first == second) {
            return;
        }
        if (second < first) {
            std::swap(first, second);
        }
        parents[second] = first;
    }

    void flatten() {
        for (std::size_t componentId = 1; componentId < parents.size(); ++componentId) {
            parents[componentId] = find(static_cast<SegmentIdType>(componentId));
        }
    }

    SegmentIdType representative(SegmentIdType componentId) const {
        return parents[componentId];
    }

private:
    std::vector<SegmentIdType> parents;
};

std::vector<BoundaryComponentRecord> labelBoundaryComponentSlab(
    const SegmentIdType *displayBuffer,
    SegmentIdType *componentBuffer,
    std::size_t dimX,
    std::size_t dimY,
    std::size_t zBegin,
    std::size_t zEnd,
    std::atomic<std::uint64_t> &nextComponentId) {
    const std::size_t planeXY = dimX * dimY;
    const std::size_t firstIndex = zBegin * planeXY;
    const std::size_t endIndex = zEnd * planeXY;

    std::vector<BoundaryComponentRecord> records;
    std::vector<std::size_t> stack;
    records.reserve(1024);
    stack.reserve(1024);

    for (std::size_t seed = firstIndex; seed < endIndex; ++seed) {
        const SegmentIdType originalLabel = displayBuffer[seed];
        if (originalLabel == 0 || componentBuffer[seed] != 0) {
            continue;
        }

        const std::uint64_t componentId64 = nextComponentId.fetch_add(1, std::memory_order_relaxed) + 1;
        if (componentId64 > std::numeric_limits<SegmentIdType>::max()) {
            throw std::overflow_error("Boundary component count exceeds the segment label range.");
        }
        const SegmentIdType componentId = static_cast<SegmentIdType>(componentId64);
        records.push_back({componentId, originalLabel});

        componentBuffer[seed] = componentId;
        stack.clear();
        stack.push_back(seed);

        while (!stack.empty()) {
            const std::size_t current = stack.back();
            stack.pop_back();
            const std::size_t z = current / planeXY;
            const std::size_t remainder = current % planeXY;
            const std::size_t y = remainder / dimX;
            const std::size_t x = remainder % dimX;

            const auto addNeighbor = [&](std::size_t neighbor) {
                if (componentBuffer[neighbor] == 0 && displayBuffer[neighbor] == originalLabel) {
                    componentBuffer[neighbor] = componentId;
                    stack.push_back(neighbor);
                }
            };

            if (x > 0) addNeighbor(current - 1);
            if (x + 1 < dimX) addNeighbor(current + 1);
            if (y > 0) addNeighbor(current - dimX);
            if (y + 1 < dimY) addNeighbor(current + dimX);
            if (z > zBegin) addNeighbor(current - planeXY);
            if (z + 1 < zEnd) addNeighbor(current + planeXY);
        }
    }

    return records;
}

void relabelInjectedBoundaryComponents(
    SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    SegmentsImageType::Pointer &displayLabels,
    BoundaryConsistentPartitionResult::SplitComponentMap &splitComponentIds,
    int threadCount) {
    const auto totalStarted = std::chrono::steady_clock::now();
    auto stageStarted = totalStarted;
    displayLabels = copySegmentsImage(labels);
    splitComponentIds.clear();
    emitWatershedLog("Boundary relabel: label copy_ms=" + std::to_string(elapsedMilliseconds(stageStarted)));

    if (thresholdedBoundaries.IsNull()) {
        emitWatershedLog("Boundary relabel: skipped component split (no boundary image), total_ms=" +
                         std::to_string(elapsedMilliseconds(totalStarted)));
        return;
    }

    if (labels->GetLargestPossibleRegion().GetSize() !=
        thresholdedBoundaries->GetLargestPossibleRegion().GetSize()) {
        throw std::invalid_argument("Boundary relabeling requires matching label and boundary shapes.");
    }

    const auto size = labels->GetLargestPossibleRegion().GetSize();
    const std::size_t dimX = size[0];
    const std::size_t dimY = size[1];
    const std::size_t dimZ = size[2];
    const std::size_t planeXY = dimX * dimY;
    const std::size_t total = planeXY * dimZ;
    const int requestedThreadCount = std::max(1, threadCount);
    int executionThreadCount = 1;
#ifdef USE_OMP
    executionThreadCount = std::min(requestedThreadCount, std::max(1, omp_get_max_threads()));
#endif

    SegmentIdType *displayBuffer = displayLabels->GetBufferPointer();
    const unsigned char *thresholdBuffer = thresholdedBoundaries->GetBufferPointer();
    stageStarted = std::chrono::steady_clock::now();
#ifdef USE_OMP
#pragma omp parallel for num_threads(executionThreadCount)
#endif
    for (long long index = 0; index < static_cast<long long>(total); ++index) {
        if (thresholdBuffer[index] != 0) {
            displayBuffer[index] = 0;
        }
    }
    emitWatershedLog("Boundary relabel: inject boundaries_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)));

    stageStarted = std::chrono::steady_clock::now();
    auto componentImage = cloneSegmentsImageMetadata(labels);
    componentImage->FillBuffer(0);
    SegmentIdType *componentBuffer = componentImage->GetBufferPointer();
    emitWatershedLog("Boundary relabel: workspace allocation_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)));

    if (total == 0) {
        emitWatershedLog("Boundary relabel: empty image, total_ms=" +
                         std::to_string(elapsedMilliseconds(totalStarted)));
        return;
    }

    const int slabCount = static_cast<int>(
        std::min<std::size_t>(static_cast<std::size_t>(executionThreadCount), dimZ));
    // Label disjoint z-slabs independently; only their shared faces need a later merge.
    std::vector<std::vector<BoundaryComponentRecord>> recordsBySlab(static_cast<std::size_t>(slabCount));
    std::atomic<std::uint64_t> nextComponentId{0};
    std::exception_ptr firstException;
    std::mutex exceptionMutex;
    std::atomic<bool> stopRequested{false};
    int usedThreadCount = 1;

    const auto labelSlab = [&](int slabIndex) {
        if (stopRequested.load(std::memory_order_relaxed)) {
            return;
        }
        try {
            const std::size_t zBegin = dimZ * static_cast<std::size_t>(slabIndex) /
                                       static_cast<std::size_t>(slabCount);
            const std::size_t zEnd = dimZ * static_cast<std::size_t>(slabIndex + 1) /
                                     static_cast<std::size_t>(slabCount);
            recordsBySlab[static_cast<std::size_t>(slabIndex)] = labelBoundaryComponentSlab(
                displayBuffer, componentBuffer, dimX, dimY, zBegin, zEnd, nextComponentId);
        } catch (...) {
            std::lock_guard<std::mutex> guard(exceptionMutex);
            if (!firstException) {
                firstException = std::current_exception();
            }
            stopRequested.store(true, std::memory_order_relaxed);
        }
    };

    stageStarted = std::chrono::steady_clock::now();
#ifdef USE_OMP
    if (slabCount > 1) {
#pragma omp parallel num_threads(slabCount)
        {
#pragma omp single
            usedThreadCount = omp_get_num_threads();
#pragma omp for schedule(static)
            for (int slabIndex = 0; slabIndex < slabCount; ++slabIndex) {
                labelSlab(slabIndex);
            }
        }
    } else
#endif
    {
        labelSlab(0);
    }

    if (firstException) {
        std::rethrow_exception(firstException);
    }

    const std::size_t localComponentCount = static_cast<std::size_t>(nextComponentId.load());
    const double localLabelingMs = elapsedMilliseconds(stageStarted);

    const auto mergeStarted = std::chrono::steady_clock::now();
    BoundaryComponentDisjointSet componentSets(localComponentCount);
    // Equal-label face neighbors are the only components that can connect across slabs.
    for (int slabIndex = 1; slabIndex < slabCount; ++slabIndex) {
        const std::size_t z = dimZ * static_cast<std::size_t>(slabIndex) /
                              static_cast<std::size_t>(slabCount);
        const std::size_t lowerOffset = (z - 1) * planeXY;
        const std::size_t upperOffset = z * planeXY;
        for (std::size_t offset = 0; offset < planeXY; ++offset) {
            const SegmentIdType label = displayBuffer[lowerOffset + offset];
            if (label != 0 && label == displayBuffer[upperOffset + offset]) {
                componentSets.unite(
                    componentBuffer[lowerOffset + offset],
                    componentBuffer[upperOffset + offset]);
            }
        }
    }
    const double boundaryMergeMs = elapsedMilliseconds(mergeStarted);

    std::unordered_map<SegmentIdType, std::vector<SegmentIdType>> componentsByOriginalLabel;
    std::vector<unsigned char> rootSeen(localComponentCount + 1, 0);
    std::size_t globalComponentCount = 0;
    for (const auto &slabRecords : recordsBySlab) {
        for (const BoundaryComponentRecord &record : slabRecords) {
            const SegmentIdType root = componentSets.find(record.id);
            if (rootSeen[root] != 0) {
                continue;
            }
            rootSeen[root] = 1;
            componentsByOriginalLabel[record.originalLabel].push_back(root);
            ++globalComponentCount;
        }
    }
    componentSets.flatten();

    emitWatershedLog("Boundary relabel: component search_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)) +
                     ", local_ms=" + std::to_string(localLabelingMs) +
                     ", merge_ms=" + std::to_string(boundaryMergeMs) +
                     ", components=" + std::to_string(globalComponentCount) +
                     ", local_components=" + std::to_string(localComponentCount) +
                     ", slabs=" + std::to_string(slabCount) +
                     ", requested_threads=" + std::to_string(requestedThreadCount) +
                     ", threads=" + std::to_string(usedThreadCount));

    stageStarted = std::chrono::steady_clock::now();
    std::uint64_t nextFreshLabel = static_cast<std::uint64_t>(getMaximumOfUIntImage(labels)) + 1;
    std::vector<SegmentIdType> rootToFinalLabel(localComponentCount + 1, 0);
    for (auto &entry : componentsByOriginalLabel) {
        const SegmentIdType originalLabel = entry.first;
        auto &componentRoots = entry.second;
        if (componentRoots.size() == 1) {
            rootToFinalLabel[componentRoots.front()] = originalLabel;
            continue;
        }

        auto &newLabels = splitComponentIds[originalLabel];
        newLabels.reserve(componentRoots.size());
        for (SegmentIdType componentRoot : componentRoots) {
            if (nextFreshLabel > std::numeric_limits<SegmentIdType>::max()) {
                throw std::overflow_error("Boundary split labels exceed the segment label range.");
            }
            const SegmentIdType newLabel = static_cast<SegmentIdType>(nextFreshLabel++);
            rootToFinalLabel[componentRoot] = newLabel;
            newLabels.push_back(newLabel);
        }
    }
    emitWatershedLog("Boundary relabel: component mapping_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)) +
                     ", split_labels=" + std::to_string(splitComponentIds.size()));

    stageStarted = std::chrono::steady_clock::now();
#ifdef USE_OMP
#pragma omp parallel for num_threads(executionThreadCount)
#endif
    for (long long index = 0; index < static_cast<long long>(total); ++index) {
        const SegmentIdType componentId = componentBuffer[index];
        displayBuffer[index] = componentId == 0
            ? 0
            : rootToFinalLabel[componentSets.representative(componentId)];
    }
    emitWatershedLog("Boundary relabel: writeback_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)) +
                     ", total_ms=" + std::to_string(elapsedMilliseconds(totalStarted)));
}

SegmentsImageType::Pointer repairSplitLabelsWithWatershed(
    SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    SegmentsImageType::Pointer displayLabels,
    const BoundaryConsistentPartitionResult::SplitComponentMap &splitComponentIds,
    const WatershedRunOptions &repairOptions,
    DistanceMapAlgorithm distanceMapAlgorithm,
    int threadCount) {
    const auto totalStarted = std::chrono::steady_clock::now();
    auto stageStarted = totalStarted;
    auto canonicalLabels = copySegmentsImage(labels);
    emitWatershedLog("Boundary repair: canonical copy_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)));
    if (thresholdedBoundaries.IsNull() || splitComponentIds.empty()) {
        emitWatershedLog("Boundary repair: skipped watershed, total_ms=" +
                         std::to_string(elapsedMilliseconds(totalStarted)));
        return canonicalLabels;
    }

    stageStarted = std::chrono::steady_clock::now();
    auto seeds = cloneSegmentsImageMetadata(labels);
    SegmentIdType *seedBuffer = seeds->GetBufferPointer();
    const SegmentIdType *labelBuffer = labels->GetBufferPointer();
    const SegmentIdType *displayBuffer = displayLabels->GetBufferPointer();
    const size_t voxelCount = labels->GetLargestPossibleRegion().GetNumberOfPixels();

    std::unordered_set<SegmentIdType> splitLabelSet;
    splitLabelSet.reserve(splitComponentIds.size());
    for (const auto &entry : splitComponentIds) {
        splitLabelSet.insert(entry.first);
    }

    for (size_t index = 0; index < voxelCount; ++index) {
        const SegmentIdType originalLabel = labelBuffer[index];
        if (originalLabel == 0) {
            seedBuffer[index] = 0;
            continue;
        }
        if (splitLabelSet.count(originalLabel) == 0) {
            seedBuffer[index] = originalLabel;
            continue;
        }
        seedBuffer[index] = displayBuffer[index];
    }
    emitWatershedLog("Boundary repair: seed preparation_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)) +
                     ", split_labels=" + std::to_string(splitComponentIds.size()));

    stageStarted = std::chrono::steady_clock::now();
    itk::Image<float, 3>::Pointer distanceMap;
    auto thresholdCopy = thresholdedBoundaries;
    generateDistanceMap(thresholdCopy, distanceMap, 0, distanceMapAlgorithm, threadCount);
    emitWatershedLog("Boundary repair: distance map_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)));

    stageStarted = std::chrono::steady_clock::now();
    itk::Image<float, 3>::Pointer invertedDistanceMap;
    invertDistanceMap(distanceMap, invertedDistanceMap);
    emitWatershedLog("Boundary repair: inversion_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)));

    stageStarted = std::chrono::steady_clock::now();
    WatershedRunOptions options = repairOptions;
    options.showWatershedLines = false;
    options.threadCount = threadCount;
    runWatershed(invertedDistanceMap, seeds, canonicalLabels, options);
    emitWatershedLog("Boundary repair: watershed_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)) +
                     ", total_ms=" + std::to_string(elapsedMilliseconds(totalStarted)));
    return canonicalLabels;
}

} // namespace

void setWatershedLogSink(WatershedLogSink sink) {
    std::lock_guard<std::mutex> guard(watershedLogMutex());
    watershedLogSinkStorage() = std::move(sink);
}

void binaryThresholdImageFilterFloat(itk::Image<unsigned short, 3>::Pointer &inputImage,
                                     itk::Image<unsigned char, 3>::Pointer &outputImage,
                                     float thresholdValueMin) {
    using BinaryThresholdImageFilterType =
        itk::BinaryThresholdImageFilter<itk::Image<unsigned short, 3>, itk::Image<unsigned char, 3>>;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
}

unsigned int getMaximumOfUIntImage(itk::Image<unsigned int, 3>::Pointer &UIntImage) {
    using StatisticsUIntImageFilterType = itk::StatisticsImageFilter<itk::Image<unsigned int, 3>>;
    StatisticsUIntImageFilterType::Pointer statisticsUIntImageFilter = StatisticsUIntImageFilterType::New();
    statisticsUIntImageFilter->SetInput(UIntImage);
    statisticsUIntImageFilter->Update();
    return statisticsUIntImageFilter->GetMaximum();
}

void setBoundariesToValue(itk::Image<unsigned char, 3>::Pointer &CharImage, unsigned char value) {
    using CharIteratorType = itk::ImageRegionIterator<itk::Image<unsigned char, 3>>;

    std::vector<unsigned int> indexX(6, 0), indexY(6, 0), indexZ(6, 0);
    std::vector<unsigned int> sizeX(6, 0), sizeY(6, 0), sizeZ(6, 0);

    itk::Image<unsigned char, 3>::RegionType LargestRegion = CharImage->GetLargestPossibleRegion();
    itk::Image<unsigned char, 3>::SizeType ImageDimensions = LargestRegion.GetSize();
    std::vector<unsigned int> dim(3, 0);
    dim[0] = static_cast<unsigned int>(ImageDimensions[0]);
    dim[1] = static_cast<unsigned int>(ImageDimensions[1]);
    dim[2] = static_cast<unsigned int>(ImageDimensions[2]);

    indexX = {0, 0, 0, dim[0] - 1, 0, 0};
    indexY = {0, 0, 0, 0, 0, dim[1] - 1};
    indexZ = {0, dim[2] - 1, 0, 0, 0, 0};
    sizeX = {dim[0], dim[0], 1, 1, dim[0], dim[0]};
    sizeY = {dim[1], dim[1], dim[1], dim[1], 1, 1};
    sizeZ = {1, 1, dim[2], dim[2], dim[2], dim[2]};

    for (unsigned int i = 0; i < indexX.size(); ++i) {
        itk::Image<unsigned char, 3>::SizeType size_ROI;
        size_ROI[0] = sizeX.at(i);
        size_ROI[1] = sizeY.at(i);
        size_ROI[2] = sizeZ.at(i);

        itk::Image<unsigned char, 3>::IndexType index;
        index[0] = indexX.at(i);
        index[1] = indexY.at(i);
        index[2] = indexZ.at(i);

        itk::Image<unsigned char, 3>::RegionType imageBoundaryROI;
        imageBoundaryROI.SetSize(size_ROI);
        imageBoundaryROI.SetIndex(index);

        CharIteratorType it(CharImage, imageBoundaryROI);
        for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
            it.Set(value);
        }
    }
}

void generateDistanceMap(itk::Image<unsigned char, 3>::Pointer &edgeImage,
                         itk::Image<float, 3>::Pointer &distanceMap,
                         double varianceGaussianFilter,
                         DistanceMapAlgorithm algorithm,
                         int threadCount) {
    emitWatershedLog("Generate Distance Map");

    if (algorithm == DistanceMapAlgorithm::FH) {
        const auto region = edgeImage->GetLargestPossibleRegion();
        const auto size = region.GetSize();
        const std::array<int, 3> dims = {
            static_cast<int>(size[0]),
            static_cast<int>(size[1]),
            static_cast<int>(size[2])
        };

        std::vector<distance_map_benchmark::BinaryVoxelType> mask;
        mask.reserve(region.GetNumberOfPixels());
        itk::ImageRegionConstIterator<itk::Image<unsigned char, 3>> iterator(edgeImage, region);
        for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
            mask.push_back(iterator.Get());
        }

        const auto fhBackground = distance_map_benchmark::runBoundaryAwareSquaredEdt(
            mask, dims, {1.0, 1.0, 1.0}, threadCount);

        std::vector<distance_map_benchmark::BinaryVoxelType> invertedMask(mask.size());
        for (std::size_t i = 0; i < mask.size(); ++i) {
            invertedMask[i] = (mask[i] == 0) ? 1 : 0;
        }
        const auto fhForeground = distance_map_benchmark::runBoundaryAwareSquaredEdt(
            invertedMask, dims, {1.0, 1.0, 1.0}, threadCount);

        distanceMap = itk::Image<float, 3>::New();
        distanceMap->SetRegions(region);
        distanceMap->SetSpacing(edgeImage->GetSpacing());
        distanceMap->SetOrigin(edgeImage->GetOrigin());
        distanceMap->SetDirection(edgeImage->GetDirection());
        distanceMap->Allocate();

        itk::ImageRegionIterator<itk::Image<float, 3>> outIterator(distanceMap, region);
        std::size_t outputIndex = 0;
        for (outIterator.GoToBegin(); !outIterator.IsAtEnd(); ++outIterator, ++outputIndex) {
            if (mask[outputIndex] == 0) {
                const float sq = fhBackground.distances[outputIndex];
                outIterator.Set(sq <= 0.0f ? 0.0f
                    : static_cast<float>(std::sqrt(static_cast<double>(sq))));
            } else {
                const float sq = fhForeground.distances[outputIndex];
                outIterator.Set(sq <= 0.0f ? 0.0f
                    : -static_cast<float>(std::sqrt(static_cast<double>(sq))));
            }
        }
    } else {
        using SignedMaurerDistanceMapImageFilterType =
            itk::SignedMaurerDistanceMapImageFilter<itk::Image<unsigned char, 3>, itk::Image<float, 3>>;
        SignedMaurerDistanceMapImageFilterType::Pointer distanceFilter = SignedMaurerDistanceMapImageFilterType::New();
        distanceFilter->SetInput(edgeImage);
        distanceFilter->SetBackgroundValue(0);
        distanceFilter->SquaredDistanceOff();
        distanceFilter->UseImageSpacingOff();

        distanceMap = distanceFilter->GetOutput();
        distanceFilter->Update();
    }

    if (varianceGaussianFilter > 0) {
        using GaussianFilterType = itk::DiscreteGaussianImageFilter<itk::Image<float, 3>, itk::Image<float, 3>>;
        GaussianFilterType::Pointer gaussianFilter = GaussianFilterType::New();
        gaussianFilter->SetVariance(varianceGaussianFilter);
        gaussianFilter->SetInput(distanceMap);
        gaussianFilter->SetUseImageSpacingOff();
        distanceMap = gaussianFilter->GetOutput();
        gaussianFilter->Update();
    }
}

float getMaximumOfFloatImage(itk::Image<float, 3>::Pointer &floatImage) {
    using StatisticsFloatImageFilterType = itk::StatisticsImageFilter<itk::Image<float, 3>>;
    StatisticsFloatImageFilterType::Pointer statisticsFloatImageFilter = StatisticsFloatImageFilterType::New();
    statisticsFloatImageFilter->SetInput(floatImage);
    statisticsFloatImageFilter->Update();
    return statisticsFloatImageFilter->GetMaximum();
}

void invertDistanceMap(itk::Image<float, 3>::Pointer &distanceMap,
                       itk::Image<float, 3>::Pointer &invertedDistanceMap) {
    using InvertIntensityImageFilterType =
        itk::InvertIntensityImageFilter<itk::Image<float, 3>, itk::Image<float, 3>>;
    InvertIntensityImageFilterType::Pointer invertFilter = InvertIntensityImageFilterType::New();

    float maxDistance = getMaximumOfFloatImage(distanceMap);
    float DistLowestLevel = std::ceil(maxDistance);

    invertFilter->SetInput(distanceMap);
    invertFilter->SetMaximum(DistLowestLevel + 1);
    invertFilter->Update();
    invertedDistanceMap = invertFilter->GetOutput();

    emitWatershedLog("New Maximum should be: " + std::to_string(invertFilter->GetMaximum()));
    emitWatershedLog(
        "Maximum after inverting the distancemap : " + std::to_string(getMaximumOfFloatImage(invertedDistanceMap)));
}

void runWatershed(itk::Image<float, 3>::Pointer &invertedDistanceMap,
                  itk::Image<unsigned int, 3>::Pointer &seeds,
                  itk::Image<unsigned int, 3>::Pointer &watershedOut,
                  const WatershedRunOptions &options,
                  segment_puzzler::FastMarkerWatershedMetrics *fastMetrics) {
    emitWatershedLog("Run watershed ...");
    switch (options.algorithm) {
        case WatershedAlgorithm::MorphologicalWatershedFromMarkers: {
            using WatershedFilterType =
                itk::MorphologicalWatershedFromMarkersImageFilter<itk::Image<float, 3>, itk::Image<unsigned int, 3>>;
            WatershedFilterType::Pointer watershedFilter = WatershedFilterType::New();
            watershedFilter->SetMarkWatershedLine(options.showWatershedLines);
            watershedFilter->SetFullyConnected(options.fullyConnected);
            watershedFilter->SetInput1(invertedDistanceMap);
            watershedFilter->SetInput2(seeds);
            watershedFilter->Update();
            watershedOut = watershedFilter->GetOutput();
            return;
        }
        case WatershedAlgorithm::FastMarkerWatershed: {
            segment_puzzler::FastMarkerWatershedOptions fastOptions;
            fastOptions.fullyConnected = options.fullyConnected;
            fastOptions.markWatershedLine = options.showWatershedLines;
            watershedOut = segment_puzzler::runFastMarkerWatershed3D(
                invertedDistanceMap, seeds, fastOptions, fastMetrics);
            return;
        }
        case WatershedAlgorithm::BlockwiseFastMarkerWatershed: {
            segment_puzzler::BlockwiseFastMarkerWatershedOptions blockwiseOptions;
            blockwiseOptions.threadCount = options.threadCount;
            blockwiseOptions.blockEdge = options.blockEdge;
            blockwiseOptions.halo = options.blockHalo;
            blockwiseOptions.watershed.fullyConnected = options.fullyConnected;
            blockwiseOptions.watershed.markWatershedLine = options.showWatershedLines;
            segment_puzzler::BlockwiseFastMarkerWatershedMetrics blockwiseMetrics;
            watershedOut = segment_puzzler::runBlockwiseFastMarkerWatershed3D(
                invertedDistanceMap, seeds, blockwiseOptions, &blockwiseMetrics);
            emitWatershedLog(
                "Blockwise watershed: edge=" + std::to_string(blockwiseMetrics.blockEdge) +
                ", halo=" + std::to_string(blockwiseOptions.halo) +
                ", threads=" + std::to_string(blockwiseOptions.threadCount) +
                ", red_blocks=" + std::to_string(blockwiseMetrics.redBlockCount) +
                ", black_blocks=" + std::to_string(blockwiseMetrics.blackBlockCount) +
                ", deferred=" + std::to_string(blockwiseMetrics.deferredBlockCount) +
                ", passes=" + std::to_string(blockwiseMetrics.passCount) +
                ", red_ms=" + std::to_string(blockwiseMetrics.redMs) +
                ", black_ms=" + std::to_string(blockwiseMetrics.blackMs) +
                ", elapsed_ms=" + std::to_string(blockwiseMetrics.elapsedMs) +
                ", global_fallback=" + (blockwiseMetrics.usedGlobalFallback ? "yes" : "no"));
            for (std::size_t passIndex = 0; passIndex < blockwiseMetrics.passes.size(); ++passIndex) {
                const auto &pass = blockwiseMetrics.passes[passIndex];
                const double effectiveParallelism = pass.elapsedMs > 0.0
                    ? pass.summedBlockMs / pass.elapsedMs
                    : 0.0;
                emitWatershedLog(
                    "Blockwise pass " + std::to_string(passIndex + 1) +
                    ": color=" + (pass.red ? "red" : "black") +
                    ", scheduled=" + std::to_string(pass.scheduledBlockCount) +
                    ", completed=" + std::to_string(pass.completedBlockCount) +
                    ", deferred=" + std::to_string(pass.deferredBlockCount) +
                    ", threads_used=" + std::to_string(pass.threadsUsed) +
                    ", snapshot_ms=" + std::to_string(pass.snapshotMs) +
                    ", wall_ms=" + std::to_string(pass.elapsedMs) +
                    ", summed_block_ms=" + std::to_string(pass.summedBlockMs) +
                    ", max_block_ms=" + std::to_string(pass.maxBlockMs) +
                    ", effective_parallelism=" + std::to_string(effectiveParallelism));
            }
            return;
        }
    }
}

void insertBoundariesIntoWatershed(itk::Image<unsigned int, 3>::Pointer &watershed,
                                   itk::Image<unsigned char, 3>::Pointer &thresholdedBoundaries) {
    itk::ImageRegionIterator<itk::Image<unsigned int, 3>> wsIterator(
        watershed, watershed->GetLargestPossibleRegion());
    itk::ImageRegionIterator<itk::Image<unsigned char, 3>> thresholdIterator(
        thresholdedBoundaries, thresholdedBoundaries->GetLargestPossibleRegion());
    bool insertWSasHighestValue = false;
    unsigned int valueOfBoundaryInWS;
    if (insertWSasHighestValue) {
        valueOfBoundaryInWS = getMaximumOfUIntImage(watershed) + 1;
    } else {
        valueOfBoundaryInWS = 0;
    }
    emitWatershedLog("Inserting boundaries into watershed with value: " + std::to_string(valueOfBoundaryInWS));
    for (thresholdIterator.GoToBegin(); !thresholdIterator.IsAtEnd(); ++thresholdIterator) {
        if (thresholdIterator.Get() >= 1) {
            wsIterator.Set(valueOfBoundaryInWS);
        }
        ++wsIterator;
    }
}

BoundaryConsistentPartitionResult deriveBoundaryConsistentPartition(
    dataType::SegmentsImageType::Pointer labels,
    itk::Image<unsigned char, 3>::Pointer thresholdedBoundaries,
    const WatershedRunOptions &repairOptions,
    bool repairCanonicalLabels,
    DistanceMapAlgorithm distanceMapAlgorithm,
    int threadCount) {
    const auto totalStarted = std::chrono::steady_clock::now();
    BoundaryConsistentPartitionResult result;
    if (labels.IsNull()) {
        return result;
    }

    auto stageStarted = std::chrono::steady_clock::now();
    relabelInjectedBoundaryComponents(
        labels, thresholdedBoundaries, result.displayLabels, result.splitComponentIds, threadCount);
    emitWatershedLog("Boundary-consistent partition: relabel_ms=" +
                     std::to_string(elapsedMilliseconds(stageStarted)));
    if (repairCanonicalLabels) {
        stageStarted = std::chrono::steady_clock::now();
        result.canonicalLabels = repairSplitLabelsWithWatershed(
            labels,
            thresholdedBoundaries,
            result.displayLabels,
            result.splitComponentIds,
            repairOptions,
            distanceMapAlgorithm,
            threadCount);
        emitWatershedLog("Boundary-consistent partition: repair_ms=" +
                         std::to_string(elapsedMilliseconds(stageStarted)));
    } else {
        result.canonicalLabels = labels;
    }
    emitWatershedLog("Boundary-consistent partition: total_ms=" +
                     std::to_string(elapsedMilliseconds(totalStarted)));
    return result;
}

void binaryThresholdImageFilterFloat(itk::Image<float, 3>::Pointer &inputImage,
                                     itk::Image<float, 3>::Pointer &outputImage,
                                     float thresholdValueMin) {
    using BinaryThresholdImageFilterType =
        itk::BinaryThresholdImageFilter<itk::Image<float, 3>, itk::Image<float, 3>>;
    BinaryThresholdImageFilterType::Pointer thresholdFilter = BinaryThresholdImageFilterType::New();

    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(thresholdValueMin);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    outputImage = thresholdFilter->GetOutput();
}

void castFloatToChar(itk::Image<float, 3>::Pointer &inputImage,
                     itk::Image<unsigned char, 3>::Pointer &outputType) {
    using CastType = itk::CastImageFilter<itk::Image<float, 3>, itk::Image<unsigned char, 3>>;
    CastType::Pointer castFilter = CastType::New();
    castFilter->SetInput(inputImage);
    castFilter->Update();
    outputType = castFilter->GetOutput();
}

void connectedComponentCharToUInt(itk::Image<unsigned char, 3>::Pointer &inputImage,
                                  itk::Image<unsigned int, 3>::Pointer &outputImage) {
    using ConnectedComponentImageFilterType =
        itk::ConnectedComponentImageFilter<itk::Image<unsigned char, 3>, itk::Image<unsigned int, 3>>;
    ConnectedComponentImageFilterType::Pointer componentFilter = ConnectedComponentImageFilterType::New();
    componentFilter->SetInput(inputImage);
    componentFilter->Update();
    outputImage = componentFilter->GetOutput();
}

void extractMinimaFromDistanceMap(itk::Image<float, 3>::Pointer &distanceMap,
                                  itk::Image<unsigned int, 3>::Pointer &seeds,
                                  double minimalHeight,
                                  distance_map_benchmark::SeedExtractorKind seedExtractorKind) {
    emitWatershedLog("Extracting minima ...");
    seeds = distance_map_benchmark::extractSeedsFromDistanceImage(distanceMap, seedExtractorKind, minimalHeight);
    emitWatershedLog("Number of seeds: " + std::to_string(getMaximumOfUIntImage(seeds)));
}

void filterSmallSegmentSeeds(itk::Image<unsigned int, 3>::Pointer &watershedIn,
                             itk::Image<unsigned int, 3>::Pointer &watershedOut,
                             float volumeThreshold) {
    emitWatershedLog("Filtering");
    using SegmentType = itk::Image<unsigned int, 3>;
    using ShapeFilterType = itk::LabelImageToShapeLabelMapFilter<SegmentType>;
    ShapeFilterType::Pointer shapeFilter = ShapeFilterType::New();
    shapeFilter->SetInput(watershedIn);
    shapeFilter->SetBackgroundValue(0);
    shapeFilter->Update();

    using ChangeLabelImageFilterType = itk::ChangeLabelImageFilter<SegmentType, SegmentType>;
    std::map<unsigned int, unsigned int> changeMap;
    const auto *labelMap = shapeFilter->GetOutput();
    const auto labelCount = labelMap->GetNumberOfLabelObjects();
    for (size_t i = 0; i < labelCount; ++i) {
        const auto *labelObject = labelMap->GetNthLabelObject(i);
        const auto labelValue = labelObject->GetLabel();
        const auto labelVolume = labelObject->GetNumberOfPixels();

        if (static_cast<double>(labelVolume) < volumeThreshold) {
            changeMap[labelValue] = 0;
        }
    }
    emitWatershedLog("Filtered " + std::to_string(changeMap.size()) + " seeds!");

    ChangeLabelImageFilterType::Pointer changeLabelImageFilter = ChangeLabelImageFilterType::New();
    changeLabelImageFilter->SetInput(watershedIn);
    changeLabelImageFilter->SetChangeMap(changeMap);
    changeLabelImageFilter->Update();
    watershedOut = changeLabelImageFilter->GetOutput();
}
