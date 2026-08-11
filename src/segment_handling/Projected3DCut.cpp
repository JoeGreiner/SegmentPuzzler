#include "Projected3DCut.h"

#include <itkImageRegionConstIteratorWithIndex.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {

using Clock = std::chrono::steady_clock;
using MaskIndex = Projected3DCutMaskImage::IndexType;
using MaskRegion = Projected3DCutMaskImage::RegionType;

double elapsedMilliseconds(const Clock::time_point &start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct StrokeSegment {
    std::array<double, 2> start;
    std::array<double, 2> end;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
};

struct StrokeMask {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;

    bool contains(double displayX, double displayY) const {
        if (pixels.empty() || width <= 0 || height <= 0) {
            return false;
        }
        const int pixelX = std::clamp(static_cast<int>(displayX), 0, width - 1);
        const int pixelY = std::clamp(static_cast<int>(displayY), 0, height - 1);
        return pixels[static_cast<std::size_t>(pixelY) * static_cast<std::size_t>(width)
                      + static_cast<std::size_t>(pixelX)] != 0;
    }
};

struct DisplayTransform {
    std::array<double, 4> clipX{0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> clipY{0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> clipW{0.0, 0.0, 0.0, 1.0};
    int viewportWidth = 0;
    int viewportHeight = 0;
};

std::array<double, 4> projectionRow(
    const std::array<double, 16> &matrix,
    int row,
    const Projected3DCutMaskImage::SpacingType &spacing,
    const Projected3DCutMaskImage::PointType &origin)
{
    const std::size_t offset = static_cast<std::size_t>(row) * 4;
    return {
        matrix[offset] * spacing[0],
        matrix[offset + 1] * spacing[1],
        matrix[offset + 2] * spacing[2],
        matrix[offset] * (origin[0] + 0.5 * spacing[0])
            + matrix[offset + 1] * (origin[1] + 0.5 * spacing[1])
            + matrix[offset + 2] * (origin[2] + 0.5 * spacing[2])
            + matrix[offset + 3]};
}

DisplayTransform makeDisplayTransform(
    const Projected3DCutMaskImage *mask,
    const Projected3DCutRequest &request)
{
    DisplayTransform transform;
    transform.clipX = projectionRow(
        request.worldToNdcMatrix, 0, mask->GetSpacing(), mask->GetOrigin());
    transform.clipY = projectionRow(
        request.worldToNdcMatrix, 1, mask->GetSpacing(), mask->GetOrigin());
    transform.clipW = projectionRow(
        request.worldToNdcMatrix, 3, mask->GetSpacing(), mask->GetOrigin());
    transform.viewportWidth = request.viewportSize[0];
    transform.viewportHeight = request.viewportSize[1];
    return transform;
}

std::array<double, 2> projectToDisplay(
    const MaskIndex &index,
    const DisplayTransform &transform)
{
    const double x = static_cast<double>(index[0]);
    const double y = static_cast<double>(index[1]);
    const double z = static_cast<double>(index[2]);
    const auto evaluate = [x, y, z](const std::array<double, 4> &row) {
        return row[0] * x + row[1] * y + row[2] * z + row[3];
    };
    const double clipW = evaluate(transform.clipW);
    const double ndcX = std::abs(clipW) > 1e-9
                            ? evaluate(transform.clipX) / clipW
                            : 0.0;
    const double ndcY = std::abs(clipW) > 1e-9
                            ? evaluate(transform.clipY) / clipW
                            : 0.0;
    return {
        (0.5 * ndcX + 0.5) * static_cast<double>(transform.viewportWidth),
        (1.0 - (0.5 * ndcY + 0.5)) * static_cast<double>(transform.viewportHeight)};
}

double distanceSquaredToSegment(
    const std::array<double, 2> &point,
    const std::array<double, 2> &start,
    const std::array<double, 2> &end)
{
    const double segmentX = end[0] - start[0];
    const double segmentY = end[1] - start[1];
    const double pointX = point[0] - start[0];
    const double pointY = point[1] - start[1];
    const double lengthSquared = segmentX * segmentX + segmentY * segmentY;
    if (lengthSquared <= 1e-9) {
        return pointX * pointX + pointY * pointY;
    }
    const double fraction = std::clamp(
        (pointX * segmentX + pointY * segmentY) / lengthSquared, 0.0, 1.0);
    const double dx = point[0] - (start[0] + fraction * segmentX);
    const double dy = point[1] - (start[1] + fraction * segmentY);
    return dx * dx + dy * dy;
}

StrokeMask rasterizeStroke(const Projected3DCutRequest &request) {
    StrokeMask mask;
    mask.width = request.viewportSize[0];
    mask.height = request.viewportSize[1];
    if (mask.width <= 0 || mask.height <= 0 || request.strokePixels.size() < 2) {
        return mask;
    }
    mask.pixels.assign(
        static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height), 0);
    const double maximumDistanceSquared =
        request.strokeWidthPixels * request.strokeWidthPixels;
    for (std::size_t point = 1; point < request.strokePixels.size(); ++point) {
        const StrokeSegment segment{
            request.strokePixels[point - 1],
            request.strokePixels[point],
            std::min(request.strokePixels[point - 1][0], request.strokePixels[point][0])
                - request.strokeWidthPixels,
            std::max(request.strokePixels[point - 1][0], request.strokePixels[point][0])
                + request.strokeWidthPixels,
            std::min(request.strokePixels[point - 1][1], request.strokePixels[point][1])
                - request.strokeWidthPixels,
            std::max(request.strokePixels[point - 1][1], request.strokePixels[point][1])
                + request.strokeWidthPixels};
        const int minX = std::clamp(
            static_cast<int>(std::floor(segment.minX)), 0, mask.width - 1);
        const int maxX = std::clamp(
            static_cast<int>(std::ceil(segment.maxX)), 0, mask.width - 1);
        const int minY = std::clamp(
            static_cast<int>(std::floor(segment.minY)), 0, mask.height - 1);
        const int maxY = std::clamp(
            static_cast<int>(std::ceil(segment.maxY)), 0, mask.height - 1);
        for (int pixelY = minY; pixelY <= maxY; ++pixelY) {
            for (int pixelX = minX; pixelX <= maxX; ++pixelX) {
                const std::array<double, 2> pixelCenter{
                    static_cast<double>(pixelX) + 0.5,
                    static_cast<double>(pixelY) + 0.5};
                if (distanceSquaredToSegment(
                        pixelCenter, segment.start, segment.end)
                    <= maximumDistanceSquared) {
                    mask.pixels[static_cast<std::size_t>(pixelY)
                                    * static_cast<std::size_t>(mask.width)
                                + static_cast<std::size_t>(pixelX)] = 1;
                }
            }
        }
    }
    return mask;
}

struct RegionGrid {
    MaskIndex start;
    Projected3DCutMaskImage::SizeType size;
    std::vector<int> voxelIndices;

    std::size_t linearIndex(const MaskIndex &index) const {
        const std::size_t x = static_cast<std::size_t>(index[0] - start[0]);
        const std::size_t y = static_cast<std::size_t>(index[1] - start[1]);
        const std::size_t z = static_cast<std::size_t>(index[2] - start[2]);
        return z * static_cast<std::size_t>(size[1]) * static_cast<std::size_t>(size[0])
               + y * static_cast<std::size_t>(size[0]) + x;
    }

    template<typename Function>
    void forEachNeighbor(const MaskIndex &index, Function &&function) const {
        static constexpr int offsets[6][3] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
            {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto &offset : offsets) {
            MaskIndex neighbor = index;
            bool inside = true;
            for (unsigned int axis = 0; axis < 3; ++axis) {
                neighbor[axis] += offset[axis];
                inside = inside
                         && neighbor[axis] >= start[axis]
                         && neighbor[axis]
                                < start[axis]
                                      + static_cast<MaskIndex::IndexValueType>(size[axis]);
            }
            if (!inside) {
                continue;
            }
            const int voxelIndex = voxelIndices[linearIndex(neighbor)];
            if (voxelIndex >= 0) {
                function(voxelIndex);
            }
        }
    }
};

} // namespace

Projected3DCutResult computeProjected3DCut(
    const Projected3DCutMaskImage *mask,
    const Projected3DCutRequest &request)
{
    Projected3DCutResult result;
    const auto totalStartedAt = Clock::now();
    const auto finish = [&result, totalStartedAt]() {
        result.profile.totalMs = elapsedMilliseconds(totalStartedAt);
        return result;
    };
    if (mask == nullptr || request.strokePixels.size() < 2
        || request.viewportSize[0] <= 0 || request.viewportSize[1] <= 0
        || !std::isfinite(request.strokeWidthPixels)
        || request.strokeWidthPixels <= 0.0) {
        result.error = "The projected cut request is incomplete.";
        return finish();
    }

    const auto rasterStartedAt = Clock::now();
    const StrokeMask strokeMask = rasterizeStroke(request);
    result.profile.rasterizeStrokeMaskMs = elapsedMilliseconds(rasterStartedAt);

    const MaskRegion region = mask->GetLargestPossibleRegion();
    RegionGrid grid{region.GetIndex(), region.GetSize(), {}};
    grid.voxelIndices.assign(region.GetNumberOfPixels(), -1);
    std::vector<MaskIndex> voxelIndices;
    std::vector<int> componentIds;
    std::vector<unsigned char> cutVoxels;
    const DisplayTransform transform = makeDisplayTransform(mask, request);
    const auto classifyStartedAt = Clock::now();
    itk::ImageRegionConstIteratorWithIndex<Projected3DCutMaskImage> maskIt(mask, region);
    for (maskIt.GoToBegin(); !maskIt.IsAtEnd(); ++maskIt) {
        if (maskIt.Get() == 0) {
            continue;
        }
        const MaskIndex index = maskIt.GetIndex();
        const auto displayPoint = projectToDisplay(index, transform);
        const bool isCut = strokeMask.contains(displayPoint[0], displayPoint[1]);
        const int voxelIndex = static_cast<int>(voxelIndices.size());
        voxelIndices.push_back(index);
        componentIds.push_back(-1);
        cutVoxels.push_back(isCut ? 1U : 0U);
        grid.voxelIndices[grid.linearIndex(index)] = voxelIndex;
        result.profile.provisionalCutVoxelCount += isCut ? 1 : 0;
    }
    result.profile.targetVoxelCount = voxelIndices.size();
    result.profile.projectAndClassifyTargetVoxelsMs =
        elapsedMilliseconds(classifyStartedAt);
    result.profile.collectTargetVoxelsMs =
        result.profile.rasterizeStrokeMaskMs
        + result.profile.projectAndClassifyTargetVoxelsMs;
    if (voxelIndices.empty() || result.profile.provisionalCutVoxelCount == 0) {
        result.error = "The projected line does not intersect the segment.";
        return finish();
    }

    const auto componentsStartedAt = Clock::now();
    std::vector<int> queue;
    queue.reserve(voxelIndices.size());
    int componentCount = 0;
    for (int voxelIndex = 0;
         voxelIndex < static_cast<int>(voxelIndices.size()); ++voxelIndex) {
        if (cutVoxels[static_cast<std::size_t>(voxelIndex)] != 0
            || componentIds[static_cast<std::size_t>(voxelIndex)] >= 0) {
            continue;
        }
        componentIds[static_cast<std::size_t>(voxelIndex)] = componentCount;
        queue.clear();
        queue.push_back(voxelIndex);
        for (std::size_t current = 0; current < queue.size(); ++current) {
            const int active = queue[current];
            grid.forEachNeighbor(voxelIndices[static_cast<std::size_t>(active)],
                                 [&](int neighbor) {
                if (cutVoxels[static_cast<std::size_t>(neighbor)] != 0
                    || componentIds[static_cast<std::size_t>(neighbor)] >= 0) {
                    return;
                }
                componentIds[static_cast<std::size_t>(neighbor)] = componentCount;
                queue.push_back(neighbor);
            });
        }
        ++componentCount;
    }
    result.profile.finalComponentCount = componentCount;
    result.profile.connectedComponentsMs = elapsedMilliseconds(componentsStartedAt);
    if (componentCount < 2) {
        result.error = "The projected line does not separate the segment.";
        return finish();
    }

    const auto reassignStartedAt = Clock::now();
    queue.clear();
    for (int voxelIndex = 0;
         voxelIndex < static_cast<int>(voxelIndices.size()); ++voxelIndex) {
        if (componentIds[static_cast<std::size_t>(voxelIndex)] >= 0) {
            queue.push_back(voxelIndex);
        }
    }
    for (std::size_t current = 0; current < queue.size(); ++current) {
        const int active = queue[current];
        const int component = componentIds[static_cast<std::size_t>(active)];
        grid.forEachNeighbor(voxelIndices[static_cast<std::size_t>(active)],
                             [&](int neighbor) {
            if (componentIds[static_cast<std::size_t>(neighbor)] >= 0) {
                return;
            }
            componentIds[static_cast<std::size_t>(neighbor)] = component;
            queue.push_back(neighbor);
        });
    }
    result.profile.reassignCutVoxelsMs = elapsedMilliseconds(reassignStartedAt);
    if (std::any_of(componentIds.begin(), componentIds.end(),
                    [](int component) { return component < 0; })) {
        result.error = "The projected cut could not assign every segment voxel.";
        return finish();
    }

    result.partition = dataType::SegmentsImageType::New();
    result.partition->CopyInformation(mask);
    result.partition->SetRegions(region);
    result.partition->Allocate();
    result.partition->FillBuffer(0);
    result.componentVoxelCounts.assign(static_cast<std::size_t>(componentCount), 0);
    for (std::size_t voxel = 0; voxel < voxelIndices.size(); ++voxel) {
        const auto label = static_cast<dataType::SegmentIdType>(componentIds[voxel] + 1);
        result.partition->SetPixel(voxelIndices[voxel], label);
        ++result.componentVoxelCounts[static_cast<std::size_t>(componentIds[voxel])];
    }
    return finish();
}
