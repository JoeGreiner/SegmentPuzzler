#ifndef SEGMENTPUZZLER_PROJECTED_3D_CUT_SAMPLE_UTILS_H
#define SEGMENTPUZZLER_PROJECTED_3D_CUT_SAMPLE_UTILS_H

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <itkImageDuplicator.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/segment_handling/Graph.h"
#include "src/segment_handling/graphBase.h"
#include "src/viewers/fileIO.h"

namespace projected3dcutsample {

struct GraphFixture {
    std::shared_ptr<GraphBase> graphBase;
    std::unique_ptr<Graph> graph;
};

struct LargestSegmentInfo {
    dataType::SegmentIdType label = 0;
    Roi roi;
    std::size_t voxelCount = 0;
};

enum class ProjectionPlane {
    XY,
    XZ,
    YZ
};

struct CandidateCut {
    QString name;
    ProjectionPlane plane = ProjectionPlane::XY;
    bool invertedDiagonal = false;
    Projected3DCutRequest request;
};

inline QString defaultSampleSegmentsPath() {
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(tempDir).filePath(QStringLiteral("Watershed_MC.tif"));
}

inline dataType::SegmentsImageType::Pointer duplicateSegmentsImage(
    const dataType::SegmentsImageType::Pointer &sourceImage)
{
    using DuplicatorType = itk::ImageDuplicator<dataType::SegmentsImageType>;
    auto duplicator = DuplicatorType::New();
    duplicator->SetInputImage(sourceImage);
    duplicator->Update();
    return duplicator->GetOutput();
}

inline GraphFixture buildGraphFixture(const dataType::SegmentsImageType::Pointer &sourceImage) {
    GraphFixture fixture;
    fixture.graphBase = std::make_shared<GraphBase>();
    fixture.graphBase->pWorkingSegmentsImage = duplicateSegmentsImage(sourceImage);
    fixture.graph = std::make_unique<Graph>(fixture.graphBase, false);
    fixture.graphBase->pGraph = fixture.graph.get();
    fixture.graph->setPointerToIgnoredSegmentLabels(&fixture.graphBase->ignoredSegmentLabels);
    fixture.graph->constructFromVolume(fixture.graphBase->pWorkingSegmentsImage);
    return fixture;
}

inline std::size_t workingNodeVoxelCount(const WorkingNode &workingNode) {
    std::size_t voxelCount = 0;
    for (const auto &initialNodeEntry : workingNode.subInitialNodes) {
        if (initialNodeEntry.second != nullptr) {
            voxelCount += initialNodeEntry.second->voxels.size();
        }
    }
    return voxelCount;
}

inline LargestSegmentInfo findLargestWorkingSegment(const Graph &graph) {
    LargestSegmentInfo result;
    for (const auto &workingNodeEntry : graph.workingNodes) {
        if (workingNodeEntry.second == nullptr) {
            continue;
        }
        const std::size_t voxelCount = workingNodeVoxelCount(*workingNodeEntry.second);
        if (voxelCount > result.voxelCount) {
            result.label = workingNodeEntry.first;
            result.roi = workingNodeEntry.second->roi;
            result.voxelCount = voxelCount;
        }
    }
    return result;
}

inline QString planeName(ProjectionPlane plane) {
    switch (plane) {
    case ProjectionPlane::XY:
        return QStringLiteral("XY");
    case ProjectionPlane::XZ:
        return QStringLiteral("XZ");
    case ProjectionPlane::YZ:
        return QStringLiteral("YZ");
    }
    return QStringLiteral("Unknown");
}

inline std::pair<int, int> planeAxes(ProjectionPlane plane) {
    switch (plane) {
    case ProjectionPlane::XY:
        return {0, 1};
    case ProjectionPlane::XZ:
        return {0, 2};
    case ProjectionPlane::YZ:
        return {1, 2};
    }
    return {0, 1};
}

inline double roiAxisLength(const Roi &roi, int axis) {
    switch (axis) {
    case 0:
        return static_cast<double>(roi.maxX - roi.minX + 1);
    case 1:
        return static_cast<double>(roi.maxY - roi.minY + 1);
    default:
        return static_cast<double>(roi.maxZ - roi.minZ + 1);
    }
}

inline std::pair<double, double> worldBoundsForAxis(
    const Roi &roi,
    const dataType::SegmentsImageType::SpacingType &spacing,
    const dataType::SegmentsImageType::PointType &origin,
    int axis)
{
    int minIndex = roi.minX;
    int maxIndex = roi.maxX;
    if (axis == 1) {
        minIndex = roi.minY;
        maxIndex = roi.maxY;
    } else if (axis == 2) {
        minIndex = roi.minZ;
        maxIndex = roi.maxZ;
    }

    const double start = origin[axis] + static_cast<double>(minIndex) * spacing[axis];
    const double stop = origin[axis] + static_cast<double>(maxIndex + 1) * spacing[axis];
    return {std::min(start, stop), std::max(start, stop)};
}

inline std::array<double, 16> buildOrthographicWorldToNdcMatrix(
    const Roi &roi,
    const dataType::SegmentsImageType::SpacingType &spacing,
    const dataType::SegmentsImageType::PointType &origin,
    ProjectionPlane plane)
{
    std::array<double, 16> matrix{
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };

    const auto [horizontalAxis, verticalAxis] = planeAxes(plane);
    const auto [horizontalMin, horizontalMax] = worldBoundsForAxis(roi, spacing, origin, horizontalAxis);
    const auto [verticalMin, verticalMax] = worldBoundsForAxis(roi, spacing, origin, verticalAxis);

    const double horizontalSpan = std::max(1e-9, horizontalMax - horizontalMin);
    const double verticalSpan = std::max(1e-9, verticalMax - verticalMin);
    const double horizontalScale = 2.0 / horizontalSpan;
    const double verticalScale = 2.0 / verticalSpan;
    const double horizontalOffset = -(horizontalMax + horizontalMin) / horizontalSpan;
    const double verticalOffset = -(verticalMax + verticalMin) / verticalSpan;

    matrix[horizontalAxis] = horizontalScale;
    matrix[3] = horizontalOffset;
    matrix[4 + verticalAxis] = verticalScale;
    matrix[7] = verticalOffset;
    return matrix;
}

inline std::vector<std::array<double, 2>> buildDiagonalStroke(int viewportSize,
                                                              bool invertedDiagonal,
                                                              int sampleCount = 32)
{
    const double margin = static_cast<double>(viewportSize) * 0.15;
    const double startX = margin;
    const double endX = static_cast<double>(viewportSize) - margin;
    const double startY = invertedDiagonal ? (static_cast<double>(viewportSize) - margin) : margin;
    const double endY = invertedDiagonal ? margin : (static_cast<double>(viewportSize) - margin);

    std::vector<std::array<double, 2>> strokePixels;
    strokePixels.reserve(static_cast<std::size_t>(sampleCount));
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        const double t = sampleCount > 1
                             ? static_cast<double>(sampleIndex) / static_cast<double>(sampleCount - 1)
                             : 0.0;
        strokePixels.push_back({
            startX + t * (endX - startX),
            startY + t * (endY - startY)
        });
    }
    return strokePixels;
}

inline CandidateCut buildCandidateCut(const LargestSegmentInfo &largestSegment,
                                      const dataType::SegmentsImageType::Pointer &segmentsImage,
                                      ProjectionPlane plane,
                                      bool invertedDiagonal,
                                      int viewportSize,
                                      double strokeWidthPixels)
{
    CandidateCut candidate;
    candidate.plane = plane;
    candidate.invertedDiagonal = invertedDiagonal;
    candidate.name = QStringLiteral("%1 %2")
                         .arg(planeName(plane),
                              invertedDiagonal ? QStringLiteral("anti-diagonal")
                                               : QStringLiteral("diagonal"));

    Projected3DCutRequest request;
    request.targetWorkingLabel = largestSegment.label;
    request.viewportSize = {viewportSize, viewportSize};
    request.strokeWidthPixels = strokeWidthPixels;
    request.strokePixels = buildDiagonalStroke(viewportSize, invertedDiagonal);
    request.worldToNdcMatrix = buildOrthographicWorldToNdcMatrix(
        largestSegment.roi,
        segmentsImage->GetSpacing(),
        segmentsImage->GetOrigin(),
        plane);
    candidate.request = std::move(request);
    return candidate;
}

inline std::vector<CandidateCut> buildCandidateCuts(const LargestSegmentInfo &largestSegment,
                                                    const dataType::SegmentsImageType::Pointer &segmentsImage,
                                                    int viewportSize,
                                                    double strokeWidthPixels)
{
    std::vector<std::pair<double, ProjectionPlane>> rankedPlanes{
        {roiAxisLength(largestSegment.roi, 0) * roiAxisLength(largestSegment.roi, 1), ProjectionPlane::XY},
        {roiAxisLength(largestSegment.roi, 0) * roiAxisLength(largestSegment.roi, 2), ProjectionPlane::XZ},
        {roiAxisLength(largestSegment.roi, 1) * roiAxisLength(largestSegment.roi, 2), ProjectionPlane::YZ}
    };
    std::sort(rankedPlanes.begin(), rankedPlanes.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return static_cast<int>(lhs.second) < static_cast<int>(rhs.second);
    });

    std::vector<CandidateCut> candidates;
    candidates.reserve(rankedPlanes.size() * 2);
    for (const auto &[area, plane] : rankedPlanes) {
        (void)area;
        candidates.push_back(buildCandidateCut(
            largestSegment, segmentsImage, plane, false, viewportSize, strokeWidthPixels));
        candidates.push_back(buildCandidateCut(
            largestSegment, segmentsImage, plane, true, viewportSize, strokeWidthPixels));
    }
    return candidates;
}

} // namespace projected3dcutsample

#endif // SEGMENTPUZZLER_PROJECTED_3D_CUT_SAMPLE_UTILS_H
