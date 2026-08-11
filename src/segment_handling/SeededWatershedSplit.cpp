#include "SeededWatershedSplit.h"

#include "src/utils/ConnectedComponentLabelSplitter.h"

#include <itkDiscreteGaussianImageFilter.h>
#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>
#include <itkSignedMaurerDistanceMapImageFilter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace segment_puzzler {
namespace {

using Clock = std::chrono::steady_clock;
using Index = SeededWatershedSplitSession::IndexType;

double elapsedMilliseconds(const Clock::time_point &startedAt) {
    return std::chrono::duration<double, std::milli>(Clock::now() - startedAt).count();
}

template<typename TImage>
typename TImage::Pointer allocateLike(const itk::ImageBase<3> *reference) {
    auto image = TImage::New();
    image->SetRegions(reference->GetLargestPossibleRegion());
    image->SetSpacing(reference->GetSpacing());
    image->SetOrigin(reference->GetOrigin());
    image->SetDirection(reference->GetDirection());
    image->Allocate();
    return image;
}

template<typename TImage>
std::uint64_t imageValueHash(const TImage *image) {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offsetBasis;
    const auto *bytes = reinterpret_cast<const unsigned char *>(image->GetBufferPointer());
    const std::size_t byteCount =
        image->GetLargestPossibleRegion().GetNumberOfPixels()
        * sizeof(typename TImage::PixelType);
    for (std::size_t index = 0; index < byteCount; ++index) {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

SeededSplitDistanceImage::Pointer smoothMaskedDistance(
    SeededSplitDistanceImage::Pointer distance,
    SeededSplitMaskImage::Pointer mask,
    double smoothingSigmaPixels)
{
    if (smoothingSigmaPixels <= 0.0) {
        return distance;
    }
    auto weightedDistance = allocateLike<SeededSplitDistanceImage>(mask);
    auto maskWeights = allocateLike<SeededSplitDistanceImage>(mask);
    const auto region = mask->GetLargestPossibleRegion();
    itk::ImageRegionConstIterator<SeededSplitDistanceImage> distanceIt(distance, region);
    itk::ImageRegionConstIterator<SeededSplitMaskImage> maskIt(mask, region);
    itk::ImageRegionIterator<SeededSplitDistanceImage> weightedIt(weightedDistance, region);
    itk::ImageRegionIterator<SeededSplitDistanceImage> weightIt(maskWeights, region);
    for (distanceIt.GoToBegin(), maskIt.GoToBegin(), weightedIt.GoToBegin(), weightIt.GoToBegin();
         !distanceIt.IsAtEnd(); ++distanceIt, ++maskIt, ++weightedIt, ++weightIt) {
        const float weight = maskIt.Get() == 0 ? 0.0f : 1.0f;
        weightedIt.Set(weight * distanceIt.Get());
        weightIt.Set(weight);
    }

    using GaussianFilter =
        itk::DiscreteGaussianImageFilter<SeededSplitDistanceImage, SeededSplitDistanceImage>;
    const auto configureGaussian = [smoothingSigmaPixels](GaussianFilter *filter) {
        filter->SetVariance(smoothingSigmaPixels * smoothingSigmaPixels);
        filter->SetUseImageSpacing(false);
    };
    auto distanceFilter = GaussianFilter::New();
    distanceFilter->SetInput(weightedDistance);
    configureGaussian(distanceFilter);
    distanceFilter->Update();
    auto weightFilter = GaussianFilter::New();
    weightFilter->SetInput(maskWeights);
    configureGaussian(weightFilter);
    weightFilter->Update();
    auto normalizedDistance = allocateLike<SeededSplitDistanceImage>(mask);
    itk::ImageRegionConstIterator<SeededSplitDistanceImage> smoothedDistanceIt(
        distanceFilter->GetOutput(), region);
    itk::ImageRegionConstIterator<SeededSplitDistanceImage> smoothedWeightIt(
        weightFilter->GetOutput(), region);
    itk::ImageRegionIterator<SeededSplitDistanceImage> normalizedIt(
        normalizedDistance, region);
    for (smoothedDistanceIt.GoToBegin(), smoothedWeightIt.GoToBegin(), normalizedIt.GoToBegin();
         !smoothedDistanceIt.IsAtEnd();
         ++smoothedDistanceIt, ++smoothedWeightIt, ++normalizedIt) {
        const float weight = smoothedWeightIt.Get();
        normalizedIt.Set(weight > 0.0f ? smoothedDistanceIt.Get() / weight : 0.0f);
    }
    return normalizedDistance;
}

struct MarkerBuildResult {
    SeededSplitMarkerImage::Pointer markers;
    std::array<std::size_t, 2> connectionVoxelCounts{0, 0};
    double connectionMs = 0.0;
    std::string error;
};

struct PathQueueEntry {
    float barrier = 0.0f;
    double distance = 0.0;
    std::size_t offset = 0;
};

struct WorsePathPriority {
    bool operator()(const PathQueueEntry &left, const PathQueueEntry &right) const {
        if (left.barrier != right.barrier) {
            return left.barrier > right.barrier;
        }
        if (left.distance != right.distance) {
            return left.distance > right.distance;
        }
        return left.offset > right.offset;
    }
};

std::size_t flatOffset(
    const Index &index,
    const SeededSplitMaskImage::IndexType &start,
    const SeededSplitMaskImage::SizeType &size)
{
    const auto x = static_cast<std::size_t>(index[0] - start[0]);
    const auto y = static_cast<std::size_t>(index[1] - start[1]);
    const auto z = static_cast<std::size_t>(index[2] - start[2]);
    return x + static_cast<std::size_t>(size[0])
                   * (y + static_cast<std::size_t>(size[1]) * z);
}

Index indexFromFlatOffset(
    std::size_t offset,
    const SeededSplitMaskImage::IndexType &start,
    const SeededSplitMaskImage::SizeType &size)
{
    const std::size_t sizeX = static_cast<std::size_t>(size[0]);
    const std::size_t sizeY = static_cast<std::size_t>(size[1]);
    Index index;
    index[0] = start[0] + static_cast<Index::IndexValueType>(offset % sizeX);
    offset /= sizeX;
    index[1] = start[1] + static_cast<Index::IndexValueType>(offset % sizeY);
    index[2] = start[2] + static_cast<Index::IndexValueType>(offset / sizeY);
    return index;
}

std::optional<std::unordered_set<std::size_t>> connectedSeedOffsets(
    const SeededWatershedSplitSession &session,
    const std::vector<Index> &seeds)
{
    const auto region = session.mask->GetLargestPossibleRegion();
    const auto start = region.GetIndex();
    const auto size = region.GetSize();
    const std::size_t voxelCount = region.GetNumberOfPixels();
    std::unordered_set<std::size_t> seedOffsets;
    for (const Index &seed : seeds) {
        seedOffsets.insert(flatOffset(seed, start, size));
    }
    if (seedOffsets.size() <= 1) {
        return seedOffsets;
    }

    const std::size_t source = flatOffset(seeds.front(), start, size);
    std::unordered_set<std::size_t> remainingTargets = seedOffsets;
    remainingTargets.erase(source);
    std::vector<float> bestBarrier(voxelCount, std::numeric_limits<float>::infinity());
    std::vector<double> bestDistance(voxelCount, std::numeric_limits<double>::infinity());
    std::vector<std::size_t> predecessor(voxelCount, std::numeric_limits<std::size_t>::max());
    std::priority_queue<PathQueueEntry,
                        std::vector<PathQueueEntry>,
                        WorsePathPriority> queue;
    const auto *mask = session.mask->GetBufferPointer();
    const auto *landscape = session.landscape->GetBufferPointer();
    const auto spacing = session.mask->GetSpacing();
    bestBarrier[source] = landscape[source];
    bestDistance[source] = 0.0;
    predecessor[source] = source;
    queue.push({bestBarrier[source], 0.0, source});

    while (!queue.empty() && !remainingTargets.empty()) {
        const PathQueueEntry current = queue.top();
        queue.pop();
        if (current.barrier != bestBarrier[current.offset]
            || current.distance != bestDistance[current.offset]) {
            continue;
        }
        remainingTargets.erase(current.offset);
        const Index currentIndex = indexFromFlatOffset(current.offset, start, size);
        for (unsigned int axis = 0; axis < 3; ++axis) {
            for (int direction : {-1, 1}) {
                Index neighbor = currentIndex;
                neighbor[axis] += direction;
                if (!region.IsInside(neighbor)) {
                    continue;
                }
                const std::size_t neighborOffset = flatOffset(neighbor, start, size);
                if (mask[neighborOffset] == 0) {
                    continue;
                }
                const float candidateBarrier =
                    std::max(current.barrier, landscape[neighborOffset]);
                const double candidateDistance = current.distance + spacing[axis];
                const bool betterBarrier = candidateBarrier < bestBarrier[neighborOffset];
                const bool equalBarrier = candidateBarrier == bestBarrier[neighborOffset];
                if (!betterBarrier
                    && (!equalBarrier || candidateDistance >= bestDistance[neighborOffset])) {
                    continue;
                }
                bestBarrier[neighborOffset] = candidateBarrier;
                bestDistance[neighborOffset] = candidateDistance;
                predecessor[neighborOffset] = current.offset;
                queue.push({candidateBarrier, candidateDistance, neighborOffset});
            }
        }
    }
    if (!remainingTargets.empty()) {
        return std::nullopt;
    }

    std::unordered_set<std::size_t> connected = seedOffsets;
    for (std::size_t target : seedOffsets) {
        std::size_t current = target;
        while (current != source) {
            connected.insert(current);
            current = predecessor[current];
            if (current == std::numeric_limits<std::size_t>::max()) {
                return std::nullopt;
            }
        }
    }
    connected.insert(source);
    return connected;
}

MarkerBuildResult buildSeededSplitMarkers(
    const SeededWatershedSplitSession &session,
    const SeededSplitSeedGroups &seedGroups,
    bool connectSeeds)
{
    MarkerBuildResult result;
    const auto region = session.mask->GetLargestPossibleRegion();
    const auto start = region.GetIndex();
    const auto size = region.GetSize();
    std::array<std::unordered_set<std::size_t>, 2> markerOffsets;
    for (std::size_t seedClass = 0; seedClass < seedGroups.size(); ++seedClass) {
        for (const Index &seed : seedGroups[seedClass]) {
            if (!region.IsInside(seed) || session.mask->GetPixel(seed) == 0) {
                result.error = "A seed lies outside the selected segment.";
                return result;
            }
            markerOffsets[seedClass].insert(flatOffset(seed, start, size));
        }
    }
    if (markerOffsets[0].empty() || markerOffsets[1].empty()) {
        result.error = "Both watershed marker classes need at least one seed.";
        return result;
    }
    for (std::size_t offset : markerOffsets[0]) {
        if (markerOffsets[1].count(offset) != 0) {
            result.error = "The two marker classes contain the same seed voxel.";
            return result;
        }
    }

    if (connectSeeds) {
        const auto startedAt = Clock::now();
        for (std::size_t seedClass = 0; seedClass < seedGroups.size(); ++seedClass) {
            auto connected = connectedSeedOffsets(session, seedGroups[seedClass]);
            if (!connected.has_value()) {
                result.error = "Could not connect all seeds inside the selected segment.";
                return result;
            }
            result.connectionVoxelCounts[seedClass] =
                connected->size() - markerOffsets[seedClass].size();
            markerOffsets[seedClass] = std::move(connected.value());
        }
        result.connectionMs = elapsedMilliseconds(startedAt);
        for (std::size_t offset : markerOffsets[0]) {
            if (markerOffsets[1].count(offset) != 0) {
                result.error =
                    "The red and blue seed connections overlap. Move the seeds or disable Connect Seeds.";
                return result;
            }
        }
    }

    result.markers = allocateLike<SeededSplitMarkerImage>(session.mask);
    result.markers->FillBuffer(0);
    for (std::size_t seedClass = 0; seedClass < markerOffsets.size(); ++seedClass) {
        const auto markerLabel = static_cast<dataType::SegmentIdType>(seedClass + 1);
        for (std::size_t offset : markerOffsets[seedClass]) {
            result.markers->SetPixel(indexFromFlatOffset(offset, start, size), markerLabel);
        }
    }
    return result;
}

} // namespace

SeededWatershedSplitSession prepareSeededWatershedSplit(
    dataType::SegmentsImageType::Pointer segments,
    dataType::SegmentIdType label)
{
    if (segments.IsNull() || label == 0) {
        throw std::invalid_argument("A nonzero label image is required for a seeded split.");
    }

    SeededWatershedSplitSession session;
    session.sourceLabel = label;
    session.sourceModifiedTime = static_cast<std::uint64_t>(segments->GetMTime());

    const auto maskStartedAt = Clock::now();
    std::vector<Index> targetVoxels;
    bool found = false;
    Index globalMin{};
    Index globalMax{};
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> sourceIt(
        segments, segments->GetLargestPossibleRegion());
    for (sourceIt.GoToBegin(); !sourceIt.IsAtEnd(); ++sourceIt) {
        if (sourceIt.Get() != label) {
            continue;
        }
        const Index index = sourceIt.GetIndex();
        targetVoxels.push_back(index);
        if (!found) {
            globalMin = index;
            globalMax = index;
            found = true;
        } else {
            for (unsigned int axis = 0; axis < 3; ++axis) {
                globalMin[axis] = std::min(globalMin[axis], index[axis]);
                globalMax[axis] = std::max(globalMax[axis], index[axis]);
            }
        }
    }
    if (!found) {
        throw std::runtime_error("The selected label is no longer present.");
    }

    session.voxelCount = targetVoxels.size();
    session.sourceRoi.minX = static_cast<int>(globalMin[0]);
    session.sourceRoi.minY = static_cast<int>(globalMin[1]);
    session.sourceRoi.minZ = static_cast<int>(globalMin[2]);
    session.sourceRoi.maxX = static_cast<int>(globalMax[0]);
    session.sourceRoi.maxY = static_cast<int>(globalMax[1]);
    session.sourceRoi.maxZ = static_cast<int>(globalMax[2]);

    SeededSplitMaskImage::SizeType localSize;
    for (unsigned int axis = 0; axis < 3; ++axis) {
        session.globalOffset[axis] = globalMin[axis] - 1;
        localSize[axis] = static_cast<SeededSplitMaskImage::SizeType::SizeValueType>(
            globalMax[axis] - globalMin[axis] + 3);
    }
    SeededSplitMaskImage::IndexType localStart;
    localStart.Fill(0);
    SeededSplitMaskImage::RegionType localRegion;
    localRegion.SetIndex(localStart);
    localRegion.SetSize(localSize);

    SeededSplitMaskImage::PointType localOrigin;
    segments->TransformIndexToPhysicalPoint(session.globalOffset, localOrigin);
    session.mask = SeededSplitMaskImage::New();
    session.mask->SetRegions(localRegion);
    session.mask->SetSpacing(segments->GetSpacing());
    session.mask->SetOrigin(localOrigin);
    session.mask->SetDirection(segments->GetDirection());
    session.mask->Allocate();
    session.mask->FillBuffer(0);

    for (const Index &globalIndex : targetVoxels) {
        Index localIndex;
        for (unsigned int axis = 0; axis < 3; ++axis) {
            localIndex[axis] = globalIndex[axis] - session.globalOffset[axis];
        }
        session.mask->SetPixel(localIndex, 1);
    }
    session.maskHash = imageValueHash(session.mask.GetPointer());
    dataType::SegmentsImageType::SizeType sourceSize;
    for (unsigned int axis = 0; axis < 3; ++axis) {
        sourceSize[axis] = static_cast<dataType::SegmentsImageType::SizeType::SizeValueType>(
            globalMax[axis] - globalMin[axis] + 1);
    }
    const dataType::SegmentsImageType::RegionType sourceRegion(globalMin, sourceSize);
    const auto sourceComponentCounts =
        connected_components::countConnectedComponentsByLabelInRegions(
            segments,
            {{label, sourceRegion}},
            connected_components::ConnectivityStencil::SixConnected);
    session.connectedComponentCount = sourceComponentCounts.at(label);
    session.maskAndConnectivityMs = elapsedMilliseconds(maskStartedAt);

    const auto distanceStartedAt = Clock::now();
    using DistanceFilter =
        itk::SignedMaurerDistanceMapImageFilter<SeededSplitMaskImage, SeededSplitDistanceImage>;
    auto distanceFilter = DistanceFilter::New();
    distanceFilter->SetInput(session.mask);
    // Treat segment voxels as the Maurer background so their distance is measured
    // to the nearest non-segment voxel centre (the conventional binary EDT).
    distanceFilter->SetBackgroundValue(1);
    distanceFilter->SetInsideIsPositive(false);
    distanceFilter->SetUseImageSpacing(true);
    distanceFilter->SquaredDistanceOff();
    distanceFilter->Update();
    session.distance = distanceFilter->GetOutput();
    session.distance->DisconnectPipeline();
    session.distanceTransformMs = elapsedMilliseconds(distanceStartedAt);

    updateSeededWatershedSplitLandscape(
        session, kDefaultSeededSplitSmoothingSigmaPixels);
    return session;
}

void updateSeededWatershedSplitLandscape(
    SeededWatershedSplitSession &session,
    double smoothingSigmaPixels)
{
    if (session.mask.IsNull() || session.distance.IsNull()
        || !std::isfinite(smoothingSigmaPixels) || smoothingSigmaPixels < 0.0) {
        throw std::invalid_argument("A valid seeded split session and smoothing sigma are required.");
    }

    const auto smoothingStartedAt = Clock::now();
    session.landscapeSmoothingSigmaPixels = smoothingSigmaPixels;
    const auto smoothedDistance = smoothMaskedDistance(
        session.distance, session.mask, smoothingSigmaPixels);
    session.landscape = allocateLike<SeededSplitDistanceImage>(session.mask);
    itk::ImageRegionConstIterator<SeededSplitDistanceImage> distanceIt(
        session.distance, session.distance->GetLargestPossibleRegion());
    itk::ImageRegionConstIterator<SeededSplitDistanceImage> smoothedDistanceIt(
        smoothedDistance, smoothedDistance->GetLargestPossibleRegion());
    itk::ImageRegionIterator<SeededSplitDistanceImage> landscapeIt(
        session.landscape, session.landscape->GetLargestPossibleRegion());
    session.maximumDistance = 0.0f;
    for (distanceIt.GoToBegin(), smoothedDistanceIt.GoToBegin(), landscapeIt.GoToBegin();
         !distanceIt.IsAtEnd(); ++distanceIt, ++smoothedDistanceIt, ++landscapeIt) {
        session.maximumDistance = std::max(session.maximumDistance, distanceIt.Get());
        landscapeIt.Set(-smoothedDistanceIt.Get());
    }
    session.landscapeHash = imageValueHash(session.landscape.GetPointer());
    session.landscapeSmoothingMs = elapsedMilliseconds(smoothingStartedAt);
}

std::optional<Index> seededSplitMaximumAlongWorldRay(
    const SeededWatershedSplitSession &session,
    const std::array<double, 3> &rayStartWorld,
    const std::array<double, 3> &rayEndWorld)
{
    if (session.mask.IsNull() || session.distance.IsNull()) {
        return std::nullopt;
    }

    SeededSplitMaskImage::PointType startPoint;
    SeededSplitMaskImage::PointType endPoint;
    for (unsigned int axis = 0; axis < 3; ++axis) {
        startPoint[axis] = rayStartWorld[axis];
        endPoint[axis] = rayEndWorld[axis];
    }
    itk::ContinuousIndex<double, 3> rayStart;
    itk::ContinuousIndex<double, 3> rayEnd;
    static_cast<void>(
        session.mask->TransformPhysicalPointToContinuousIndex(startPoint, rayStart));
    static_cast<void>(
        session.mask->TransformPhysicalPointToContinuousIndex(endPoint, rayEnd));

    const auto region = session.mask->GetLargestPossibleRegion();
    const auto regionStart = region.GetIndex();
    const auto regionSize = region.GetSize();
    double firstT = 0.0;
    double lastT = 1.0;
    for (unsigned int axis = 0; axis < 3; ++axis) {
        const double direction = rayEnd[axis] - rayStart[axis];
        const double minimum = static_cast<double>(regionStart[axis]) - 0.5;
        const double maximum = minimum + static_cast<double>(regionSize[axis]);
        if (std::abs(direction) <= 1e-12) {
            if (rayStart[axis] < minimum || rayStart[axis] > maximum) {
                return std::nullopt;
            }
            continue;
        }
        double entryT = (minimum - rayStart[axis]) / direction;
        double exitT = (maximum - rayStart[axis]) / direction;
        if (entryT > exitT) {
            std::swap(entryT, exitT);
        }
        firstT = std::max(firstT, entryT);
        lastT = std::min(lastT, exitT);
        if (firstT > lastT) {
            return std::nullopt;
        }
    }

    std::array<double, 3> clippedStart{};
    std::array<double, 3> clippedDirection{};
    double lengthInVoxelsSquared = 0.0;
    for (unsigned int axis = 0; axis < 3; ++axis) {
        const double direction = rayEnd[axis] - rayStart[axis];
        clippedStart[axis] = rayStart[axis] + firstT * direction;
        clippedDirection[axis] = (lastT - firstT) * direction;
        lengthInVoxelsSquared += clippedDirection[axis] * clippedDirection[axis];
    }

    const int sampleCount = std::max(
        1, static_cast<int>(std::ceil(4.0 * std::sqrt(lengthInVoxelsSquared))));
    float maximumDistance = -std::numeric_limits<float>::infinity();
    std::optional<Index> maximumIndex;
    bool enteredSegment = false;
    for (int sample = 0; sample <= sampleCount; ++sample) {
        const double t = static_cast<double>(sample) / sampleCount;
        Index index;
        for (unsigned int axis = 0; axis < 3; ++axis) {
            index[axis] = static_cast<Index::IndexValueType>(
                std::lround(clippedStart[axis] + t * clippedDirection[axis]));
        }
        const bool isSegmentVoxel =
            region.IsInside(index) && session.mask->GetPixel(index) != 0;
        if (!isSegmentVoxel) {
            if (enteredSegment) {
                break;
            }
            continue;
        }
        enteredSegment = true;
        const float distance = session.distance->GetPixel(index);
        if (distance > maximumDistance) {
            maximumDistance = distance;
            maximumIndex = index;
        }
    }
    return maximumIndex;
}

std::array<double, 3> seededSplitSeedWorldPoint(
    const SeededWatershedSplitSession &session,
    const Index &localIndex)
{
    SeededSplitMaskImage::PointType point;
    session.mask->TransformIndexToPhysicalPoint(localIndex, point);
    return {point[0], point[1], point[2]};
}

namespace {

SeededWatershedSplitResult computeFromMarkers(
    const SeededWatershedSplitSession &session,
    SeededSplitMarkerImage::Pointer markers,
    const SeededWatershedSplitOptions &splitOptions,
    const MarkerBuildResult &markerBuild)
{
    SeededWatershedSplitResult result;
    result.compactness = splitOptions.compactness;
    result.connectSeeds = splitOptions.connectSeeds;
    result.disconnectedPartsAllowed = splitOptions.allowDisconnectedParts;
    result.connectionVoxelCounts = markerBuild.connectionVoxelCounts;
    result.markerConnectionMs = markerBuild.connectionMs;
    const auto startedAt = Clock::now();
    if (session.mask.IsNull() || session.landscape.IsNull() || markers.IsNull()) {
        result.error = "The seeded split session is incomplete.";
        return result;
    }
    result.markers = markers;
    if (session.connectedComponentCount != 1) {
        result.error = "The selected segment is disconnected.";
        return result;
    }
    if (!std::isfinite(splitOptions.compactness) || splitOptions.compactness < 0.0) {
        result.error = "Watershed compactness must be finite and non-negative.";
        return result;
    }
    const auto region = session.mask->GetLargestPossibleRegion();
    if (markers->GetLargestPossibleRegion() != region) {
        result.error = "The marker image does not match the selected segment.";
        return result;
    }
    itk::ImageRegionConstIterator<SeededSplitMaskImage> markerMaskIt(session.mask, region);
    itk::ImageRegionConstIterator<SeededSplitMarkerImage> markerIt(markers, region);
    for (markerMaskIt.GoToBegin(), markerIt.GoToBegin();
         !markerMaskIt.IsAtEnd(); ++markerMaskIt, ++markerIt) {
        const auto markerLabel = markerIt.Get();
        if (markerLabel > 2 || (markerLabel != 0 && markerMaskIt.Get() == 0)) {
            result.error = "The marker image contains an invalid seed voxel.";
            return result;
        }
        if (markerLabel != 0) {
            ++result.markerVoxelCounts[markerLabel - 1];
        }
    }
    if (result.markerVoxelCounts[0] == 0 || result.markerVoxelCounts[1] == 0) {
        result.error = "Both watershed markers must contain segment voxels.";
        return result;
    }
    result.markerHash = imageValueHash(markers.GetPointer());

    FastMarkerWatershedOptions options;
    options.fullyConnected = false;
    options.compactness = splitOptions.compactness;
    if (splitOptions.compactness == 0.0) {
        options.tieBreak = FastMarkerWatershedTieBreak::GeodesicDistance;
    }
    options.domainMask = session.mask;
    result.partition = runFastMarkerWatershed3D(
        session.landscape, markers, options, &result.floodMetrics);
    itk::ImageRegionConstIterator<SeededSplitMaskImage> maskIt(
        session.mask, session.mask->GetLargestPossibleRegion());
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> partitionIt(
        result.partition, result.partition->GetLargestPossibleRegion());
    for (maskIt.GoToBegin(), partitionIt.GoToBegin();
         !maskIt.IsAtEnd(); ++maskIt, ++partitionIt) {
        if (maskIt.Get() == 0) {
            continue;
        }
        const auto splitLabel = partitionIt.Get();
        if (splitLabel < 1 || splitLabel > 2) {
            result.error = "Watershed left selected voxels unassigned.";
            result.watershedMs = elapsedMilliseconds(startedAt);
            return result;
        }
        ++result.voxelCounts[splitLabel - 1];
    }

    if (result.voxelCounts[0] == 0 || result.voxelCounts[1] == 0 ||
        result.voxelCounts[0] + result.voxelCounts[1] != session.voxelCount) {
        result.error = "Watershed did not produce two non-empty, exhaustive parts.";
        result.watershedMs = elapsedMilliseconds(startedAt);
        return result;
    }
    result.partitionHash = imageValueHash(result.partition.GetPointer());
    const auto componentCounts = connected_components::countConnectedComponentsByLabel(
        result.partition,
        {1, 2},
        connected_components::ConnectivityStencil::SixConnected);
    for (dataType::SegmentIdType label = 1; label <= 2; ++label) {
        result.connectedComponentCounts[label - 1] = componentCounts.at(label);
    }
    if (result.hasDisconnectedParts() && !splitOptions.allowDisconnectedParts) {
        result.error = "Watershed produced disconnected split parts.";
        result.watershedMs = elapsedMilliseconds(startedAt);
        return result;
    }
    result.watershedMs = elapsedMilliseconds(startedAt);
    return result;
}

} // namespace

SeededWatershedSplitResult computeSeededWatershedSplit(
    const SeededWatershedSplitSession &session,
    const SeededSplitSeedGroups &seedGroups,
    const SeededWatershedSplitOptions &options)
{
    SeededWatershedSplitResult result;
    result.compactness = options.compactness;
    result.connectSeeds = options.connectSeeds;
    result.disconnectedPartsAllowed = options.allowDisconnectedParts;
    if (session.mask.IsNull() || session.landscape.IsNull()) {
        result.error = "The seeded split session is incomplete.";
        return result;
    }
    if (session.connectedComponentCount != 1) {
        result.error = "The selected segment is disconnected.";
        return result;
    }
    if (!std::isfinite(options.compactness) || options.compactness < 0.0) {
        result.error = "Watershed compactness must be finite and non-negative.";
        return result;
    }
    MarkerBuildResult markerBuild = buildSeededSplitMarkers(
        session, seedGroups, options.connectSeeds);
    if (markerBuild.markers.IsNull()) {
        result.connectionVoxelCounts = markerBuild.connectionVoxelCounts;
        result.markerConnectionMs = markerBuild.connectionMs;
        result.error = markerBuild.error.empty()
                           ? "Could not create watershed markers."
                           : markerBuild.error;
        return result;
    }
    return computeFromMarkers(session, markerBuild.markers, options, markerBuild);
}

SeededWatershedSplitResult computeSeededWatershedSplit(
    const SeededWatershedSplitSession &session,
    const std::array<Index, 2> &seedIndices,
    const SeededWatershedSplitOptions &options)
{
    SeededSplitSeedGroups seedGroups;
    seedGroups[0].push_back(seedIndices[0]);
    seedGroups[1].push_back(seedIndices[1]);
    return computeSeededWatershedSplit(session, seedGroups, options);
}

} // namespace segment_puzzler
