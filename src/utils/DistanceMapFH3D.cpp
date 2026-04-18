#include "DistanceMapFH3D.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace distance_map_benchmark {
namespace {

constexpr DistanceVoxelType kInfinityDistance = 1.0e20f;

double wallTimeSeconds() {
#ifdef USE_OMP
    return omp_get_wtime();
#else
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
#endif
}

std::size_t flatIndex(int x, int y, int z, const std::array<int, 3> &dims) {
    return static_cast<std::size_t>((z * dims[1] + y) * dims[0] + x);
}

bool isBoundaryBackgroundVoxel(const std::vector<BinaryVoxelType> &mask,
                               const std::array<int, 3> &dims,
                               int x,
                               int y,
                               int z) {
    if (mask[flatIndex(x, y, z, dims)] != 0) {
        return false;
    }

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                const int nx = x + dx;
                const int ny = y + dy;
                const int nz = z + dz;
                if (nx < 0 || ny < 0 || nz < 0 || nx >= dims[0] || ny >= dims[1] || nz >= dims[2]) {
                    continue;
                }
                if (mask[flatIndex(nx, ny, nz, dims)] != 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

void edt1d(const DistanceVoxelType *input,
           int length,
           double spacingSquared,
           DistanceVoxelType *output,
           std::vector<int> &v,
           std::vector<double> &z) {
    if (static_cast<int>(v.size()) < length) {
        v.resize(length);
    }
    if (static_cast<int>(z.size()) < length + 1) {
        z.resize(length + 1);
    }

    auto intersection = [&](int q, int r) {
        const double fq = static_cast<double>(input[q]);
        const double fr = static_cast<double>(input[r]);
        return ((fq + spacingSquared * q * q) - (fr + spacingSquared * r * r)) /
               (2.0 * spacingSquared * (q - r));
    };

    int k = -1;
    for (int q = 0; q < length; ++q) {
        if (!std::isfinite(input[q]) || input[q] >= kInfinityDistance) {
            continue;
        }

        if (k < 0) {
            k = 0;
            v[0] = q;
            z[0] = -std::numeric_limits<double>::infinity();
            z[1] = std::numeric_limits<double>::infinity();
            continue;
        }

        double s = intersection(q, v[k]);
        while (k > 0 && s <= z[k]) {
            --k;
            s = intersection(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = std::numeric_limits<double>::infinity();
    }

    if (k < 0) {
        std::fill(output, output + length, kInfinityDistance);
        return;
    }

    int envelopeIndex = 0;
    for (int q = 0; q < length; ++q) {
        while (envelopeIndex < k && z[envelopeIndex + 1] < static_cast<double>(q)) {
            ++envelopeIndex;
        }
        const double distance = spacingSquared * (q - v[envelopeIndex]) * (q - v[envelopeIndex]) +
                                static_cast<double>(input[v[envelopeIndex]]);
        output[q] = static_cast<DistanceVoxelType>(distance);
    }
}

}

FhRunResult runBoundaryAwareSquaredEdt(const std::vector<BinaryVoxelType> &mask,
                                       const std::array<int, 3> &dims,
                                       const std::array<double, 3> &spacing,
                                       int threadCount) {
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        throw std::runtime_error("FH EDT requires positive dimensions.");
    }

#ifdef USE_OMP
    omp_set_num_threads(threadCount);
#else
    (void)threadCount;
#endif

    const std::size_t voxelCount = mask.size();
    std::vector<DistanceVoxelType> current(voxelCount, kInfinityDistance);
    std::vector<DistanceVoxelType> next(voxelCount, kInfinityDistance);
    for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
        for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
            for (int xIndex = 0; xIndex < dims[0]; ++xIndex) {
                const std::size_t index = flatIndex(xIndex, yIndex, zIndex, dims);
                if (isBoundaryBackgroundVoxel(mask, dims, xIndex, yIndex, zIndex)) {
                    current[index] = 0.0f;
                }
            }
        }
    }

    const double startTime = wallTimeSeconds();

    const double xStart = wallTimeSeconds();
#ifdef USE_OMP
#pragma omp parallel
    {
        std::vector<int> v(static_cast<std::size_t>(dims[0]));
        std::vector<double> z(static_cast<std::size_t>(dims[0]) + 1);
#pragma omp for collapse(2) schedule(static)
        for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
            for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
                const std::size_t offset = flatIndex(0, yIndex, zIndex, dims);
                edt1d(current.data() + offset, dims[0], spacing[0] * spacing[0], next.data() + offset, v, z);
            }
        }
    }
#else
    {
        std::vector<int> v(static_cast<std::size_t>(dims[0]));
        std::vector<double> z(static_cast<std::size_t>(dims[0]) + 1);
        for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
            for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
                const std::size_t offset = flatIndex(0, yIndex, zIndex, dims);
                edt1d(current.data() + offset, dims[0], spacing[0] * spacing[0], next.data() + offset, v, z);
            }
        }
    }
#endif
    const double xEnd = wallTimeSeconds();
    current.swap(next);

    const double yStart = wallTimeSeconds();
#ifdef USE_OMP
#pragma omp parallel
    {
        std::vector<DistanceVoxelType> lineIn(static_cast<std::size_t>(dims[1]));
        std::vector<DistanceVoxelType> lineOut(static_cast<std::size_t>(dims[1]));
        std::vector<int> v(static_cast<std::size_t>(dims[1]));
        std::vector<double> z(static_cast<std::size_t>(dims[1]) + 1);
#pragma omp for collapse(2) schedule(static)
        for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
            for (int xIndex = 0; xIndex < dims[0]; ++xIndex) {
                for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
                    lineIn[static_cast<std::size_t>(yIndex)] = current[flatIndex(xIndex, yIndex, zIndex, dims)];
                }
                edt1d(lineIn.data(), dims[1], spacing[1] * spacing[1], lineOut.data(), v, z);
                for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
                    next[flatIndex(xIndex, yIndex, zIndex, dims)] = lineOut[static_cast<std::size_t>(yIndex)];
                }
            }
        }
    }
#else
    {
        std::vector<DistanceVoxelType> lineIn(static_cast<std::size_t>(dims[1]));
        std::vector<DistanceVoxelType> lineOut(static_cast<std::size_t>(dims[1]));
        std::vector<int> v(static_cast<std::size_t>(dims[1]));
        std::vector<double> z(static_cast<std::size_t>(dims[1]) + 1);
        for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
            for (int xIndex = 0; xIndex < dims[0]; ++xIndex) {
                for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
                    lineIn[static_cast<std::size_t>(yIndex)] = current[flatIndex(xIndex, yIndex, zIndex, dims)];
                }
                edt1d(lineIn.data(), dims[1], spacing[1] * spacing[1], lineOut.data(), v, z);
                for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
                    next[flatIndex(xIndex, yIndex, zIndex, dims)] = lineOut[static_cast<std::size_t>(yIndex)];
                }
            }
        }
    }
#endif
    const double yEnd = wallTimeSeconds();
    current.swap(next);

    const double zStart = wallTimeSeconds();
#ifdef USE_OMP
#pragma omp parallel
    {
        std::vector<DistanceVoxelType> lineIn(static_cast<std::size_t>(dims[2]));
        std::vector<DistanceVoxelType> lineOut(static_cast<std::size_t>(dims[2]));
        std::vector<int> v(static_cast<std::size_t>(dims[2]));
        std::vector<double> z(static_cast<std::size_t>(dims[2]) + 1);
#pragma omp for collapse(2) schedule(static)
        for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
            for (int xIndex = 0; xIndex < dims[0]; ++xIndex) {
                for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
                    lineIn[static_cast<std::size_t>(zIndex)] = current[flatIndex(xIndex, yIndex, zIndex, dims)];
                }
                edt1d(lineIn.data(), dims[2], spacing[2] * spacing[2], lineOut.data(), v, z);
                for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
                    next[flatIndex(xIndex, yIndex, zIndex, dims)] = lineOut[static_cast<std::size_t>(zIndex)];
                }
            }
        }
    }
#else
    {
        std::vector<DistanceVoxelType> lineIn(static_cast<std::size_t>(dims[2]));
        std::vector<DistanceVoxelType> lineOut(static_cast<std::size_t>(dims[2]));
        std::vector<int> v(static_cast<std::size_t>(dims[2]));
        std::vector<double> z(static_cast<std::size_t>(dims[2]) + 1);
        for (int yIndex = 0; yIndex < dims[1]; ++yIndex) {
            for (int xIndex = 0; xIndex < dims[0]; ++xIndex) {
                for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
                    lineIn[static_cast<std::size_t>(zIndex)] = current[flatIndex(xIndex, yIndex, zIndex, dims)];
                }
                edt1d(lineIn.data(), dims[2], spacing[2] * spacing[2], lineOut.data(), v, z);
                for (int zIndex = 0; zIndex < dims[2]; ++zIndex) {
                    next[flatIndex(xIndex, yIndex, zIndex, dims)] = lineOut[static_cast<std::size_t>(zIndex)];
                }
            }
        }
    }
#endif
    const double zEnd = wallTimeSeconds();

    FhRunResult result;
    result.distances = std::move(next);
    result.metrics.elapsedMs = (zEnd - startTime) * 1000.0;
    result.metrics.xPassMs = (xEnd - xStart) * 1000.0;
    result.metrics.yPassMs = (yEnd - yStart) * 1000.0;
    result.metrics.zPassMs = (zEnd - zStart) * 1000.0;
    result.metrics.scratchBytes =
        2 * voxelCount * sizeof(DistanceVoxelType) +
        static_cast<std::size_t>(threadCount) *
            (2 * static_cast<std::size_t>(std::max({dims[0], dims[1], dims[2]})) * sizeof(DistanceVoxelType) +
             static_cast<std::size_t>(std::max({dims[0], dims[1], dims[2]})) * sizeof(int) +
             (static_cast<std::size_t>(std::max({dims[0], dims[1], dims[2]})) + 1) * sizeof(double));
    return result;
}

}
