#ifndef SEGMENTPUZZLER_CONNECTEDCOMPONENTLABELSPLITTER_H
#define SEGMENTPUZZLER_CONNECTEDCOMPONENTLABELSPLITTER_H

#include "src/file_definitions/dataTypes.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace segment_puzzler::connected_components {

enum class ConnectivityStencil {
    SixConnected,
    Full
};

struct ConnectedComponentVisitResult {
    std::size_t voxelCount = 0;
    bool completed = true;
};

namespace detail {

struct ImageGeometry {
    dataType::SegmentsImageType::IndexType start{};
    std::ptrdiff_t dimX = 0;
    std::ptrdiff_t dimY = 0;
    std::ptrdiff_t dimZ = 0;
    std::ptrdiff_t planeXY = 0;
    std::ptrdiff_t total = 0;
};

inline ImageGeometry geometryForRegion(
    const dataType::SegmentsImageType::RegionType &region) {
    const auto size = region.GetSize();
    constexpr auto maximum = std::numeric_limits<std::ptrdiff_t>::max();
    for (unsigned int axis = 0; axis < 3; ++axis) {
        if (static_cast<std::uintmax_t>(size[axis]) >
            static_cast<std::uintmax_t>(maximum)) {
            throw std::overflow_error("Label image dimensions exceed addressable component geometry.");
        }
    }

    ImageGeometry geometry;
    geometry.start = region.GetIndex();
    geometry.dimX = static_cast<std::ptrdiff_t>(size[0]);
    geometry.dimY = static_cast<std::ptrdiff_t>(size[1]);
    geometry.dimZ = static_cast<std::ptrdiff_t>(size[2]);
    if (geometry.dimX != 0 && geometry.dimY > maximum / geometry.dimX) {
        throw std::overflow_error("Label image plane size exceeds addressable component geometry.");
    }
    geometry.planeXY = geometry.dimX * geometry.dimY;
    if (geometry.planeXY != 0 && geometry.dimZ > maximum / geometry.planeXY) {
        throw std::overflow_error("Label image size exceeds addressable component geometry.");
    }
    geometry.total = geometry.planeXY * geometry.dimZ;
    return geometry;
}

inline ImageGeometry geometryForImage(
    const dataType::SegmentsImageType::Pointer &image) {
    if (image.IsNull()) {
        throw std::invalid_argument("Cannot traverse components in a null label image.");
    }
    if (image->GetBufferPointer() == nullptr) {
        throw std::invalid_argument("Cannot traverse components in an unallocated label image.");
    }
    return geometryForRegion(image->GetLargestPossibleRegion());
}

inline std::ptrdiff_t linearIndex(
    const dataType::SegmentsImageType::IndexType &index,
    const ImageGeometry &geometry) {
    const std::ptrdiff_t x = index[0] - geometry.start[0];
    const std::ptrdiff_t y = index[1] - geometry.start[1];
    const std::ptrdiff_t z = index[2] - geometry.start[2];
    return x + y * geometry.dimX + z * geometry.planeXY;
}

inline dataType::SegmentsImageType::IndexType imageIndex(
    std::ptrdiff_t linear,
    const ImageGeometry &geometry) {
    const std::ptrdiff_t z = linear / geometry.planeXY;
    const std::ptrdiff_t withinSlice = linear - z * geometry.planeXY;
    const std::ptrdiff_t y = withinSlice / geometry.dimX;
    const std::ptrdiff_t x = withinSlice - y * geometry.dimX;
    return {{geometry.start[0] + x, geometry.start[1] + y, geometry.start[2] + z}};
}

template<typename Visitor>
void forEachNeighbor(std::ptrdiff_t index,
                     const ImageGeometry &geometry,
                     ConnectivityStencil connectivity,
                     Visitor &&visitor) {
    const std::ptrdiff_t z = index / geometry.planeXY;
    const std::ptrdiff_t withinSlice = index - z * geometry.planeXY;
    const std::ptrdiff_t y = withinSlice / geometry.dimX;
    const std::ptrdiff_t x = withinSlice - y * geometry.dimX;

    const auto visitOffset = [&](int dx, int dy, int dz) {
        const std::ptrdiff_t nx = x + dx;
        const std::ptrdiff_t ny = y + dy;
        const std::ptrdiff_t nz = z + dz;
        if (nx < 0 || ny < 0 || nz < 0 ||
            nx >= geometry.dimX || ny >= geometry.dimY || nz >= geometry.dimZ) {
            return;
        }
        visitor(nx + ny * geometry.dimX + nz * geometry.planeXY);
    };

    if (connectivity == ConnectivityStencil::SixConnected) {
        visitOffset(-1, 0, 0);
        visitOffset(1, 0, 0);
        visitOffset(0, -1, 0);
        visitOffset(0, 1, 0);
        visitOffset(0, 0, -1);
        visitOffset(0, 0, 1);
        return;
    }

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                visitOffset(dx, dy, dz);
            }
        }
    }
}

} // namespace detail

// Visits the connected component carrying the seed's label. The visitor
// returns false to stop early. Storage grows with the visited component rather
// than with the full image volume.
template<typename Visitor>
ConnectedComponentVisitResult visitLabelComponent(
    const dataType::SegmentsImageType::Pointer &image,
    const dataType::SegmentsImageType::IndexType &seed,
    ConnectivityStencil connectivity,
    Visitor &&visitor) {
    const auto geometry = detail::geometryForImage(image);
    const auto region = image->GetLargestPossibleRegion();
    if (!region.IsInside(seed)) {
        throw std::invalid_argument("Connected-component seed is outside the label image.");
    }

    const auto *buffer = image->GetBufferPointer();
    const std::ptrdiff_t seedLinear = detail::linearIndex(seed, geometry);
    const dataType::SegmentIdType componentLabel = buffer[seedLinear];

    std::unordered_set<std::ptrdiff_t> visited;
    std::vector<std::ptrdiff_t> open;
    visited.reserve(1024);
    open.reserve(1024);
    visited.insert(seedLinear);
    open.push_back(seedLinear);

    ConnectedComponentVisitResult result;
    for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
        const std::ptrdiff_t current = open[queueIndex];
        ++result.voxelCount;
        if (!visitor(detail::imageIndex(current, geometry))) {
            result.completed = false;
            return result;
        }

        detail::forEachNeighbor(current, geometry, connectivity, [&](std::ptrdiff_t neighbor) {
            if (buffer[neighbor] == componentLabel && visited.insert(neighbor).second) {
                open.push_back(neighbor);
            }
        });
    }
    return result;
}

struct ConnectedComponentSplitOptions {
    ConnectivityStencil connectivity = ConnectivityStencil::Full;
    // Empty includes every label that is not ignored. A non-empty set limits
    // splitting to the listed labels; ignoredLabels still takes precedence.
    std::unordered_set<dataType::SegmentIdType> includedLabels;
    std::unordered_set<dataType::SegmentIdType> ignoredLabels;
    dataType::SegmentIdType nextFreeLabel = 1;
};

struct ConnectedComponentSplitStats {
    std::size_t labelsVisited = 0;
    std::size_t labelsSplit = 0;
    std::size_t componentsCreated = 0;
    std::size_t voxelsRelabeled = 0;
    dataType::SegmentIdType maxLabel = 0;
    dataType::SegmentIdType nextFreeLabel = 1;
    std::unordered_map<dataType::SegmentIdType, std::vector<dataType::SegmentIdType>> finalLabelsByOriginalLabel;

    bool changed() const {
        return componentsCreated > 0;
    }
};

const char *connectivityStencilName(ConnectivityStencil connectivity);

dataType::SegmentIdType maxLabelInImage(const dataType::SegmentsImageType::Pointer &image);

std::unordered_map<dataType::SegmentIdType, std::size_t> countConnectedComponentsByLabel(
    const dataType::SegmentsImageType::Pointer &image,
    const std::unordered_set<dataType::SegmentIdType> &labels,
    ConnectivityStencil connectivity);

std::unordered_map<dataType::SegmentIdType, std::size_t> countConnectedComponentsByLabelInRegions(
    const dataType::SegmentsImageType::Pointer &image,
    const std::unordered_map<dataType::SegmentIdType, dataType::SegmentsImageType::RegionType> &regionsByLabel,
    ConnectivityStencil connectivity);

ConnectedComponentSplitStats splitDisconnectedLabelComponentsInPlace(
    const dataType::SegmentsImageType::Pointer &image,
    const ConnectedComponentSplitOptions &options);

} // namespace segment_puzzler::connected_components

#endif // SEGMENTPUZZLER_CONNECTEDCOMPONENTLABELSPLITTER_H
