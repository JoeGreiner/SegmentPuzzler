#ifndef SEGMENTPUZZLER_DISTANCEMAPFH3D_H
#define SEGMENTPUZZLER_DISTANCEMAPFH3D_H

#include <array>
#include <cstddef>
#include <vector>

namespace distance_map_benchmark {

using BinaryVoxelType = unsigned char;
using DistanceVoxelType = float;

struct FhRunMetrics {
    double elapsedMs = 0.0;
    double xPassMs = 0.0;
    double yPassMs = 0.0;
    double zPassMs = 0.0;
    std::size_t scratchBytes = 0;
};

struct FhRunResult {
    std::vector<DistanceVoxelType> distances;
    FhRunMetrics metrics;
};

FhRunResult runBoundaryAwareSquaredEdt(const std::vector<BinaryVoxelType> &mask,
                                       const std::array<int, 3> &dims,
                                       const std::array<double, 3> &spacing,
                                       int threadCount);

}

#endif
