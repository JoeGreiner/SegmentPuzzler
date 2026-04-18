#ifndef SEGMENTPUZZLER_PROJECTED3DCUT_H
#define SEGMENTPUZZLER_PROJECTED3DCUT_H

#include <array>
#include <cstddef>
#include <vector>

#include "src/file_definitions/dataTypes.h"

struct Projected3DCutRequest {
    dataType::SegmentIdType targetWorkingLabel = 0;
    std::array<int, 2> viewportSize{0, 0};
    std::array<double, 16> worldToNdcMatrix{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    std::vector<std::array<double, 2>> strokePixels;
    double strokeWidthPixels = 6.0;
};

struct Projected3DCutProfile {
    std::size_t targetVoxelCount = 0;
    std::size_t provisionalCutVoxelCount = 0;
    int finalComponentCount = 0;
    std::size_t replacementInitialCount = 0;

    double collectTargetVoxelsMs = 0.0;
    double rasterizeStrokeMaskMs = 0.0;
    double projectAndClassifyTargetVoxelsMs = 0.0;
    double connectedComponentsMs = 0.0;
    double reassignCutVoxelsMs = 0.0;
    double splitReplacementInitialsMs = 0.0;
    double collectNeighborGroupsMs = 0.0;
    double splitWorkingNodesMs = 0.0;
    double removeOriginalNodesMs = 0.0;
    double clearTargetRegionMs = 0.0;
    double createReplacementInitialsMs = 0.0;
    double materializeReplacementVoxelListsMs = 0.0;
    double recomputeInitialEdgesMs = 0.0;
    double rebuildWorkingNodesMs = 0.0;
    double recalculateWorkingEdgesMs = 0.0;
    double rewriteWorkingImageMs = 0.0;
    double totalMs = 0.0;
};

#endif // SEGMENTPUZZLER_PROJECTED3DCUT_H
