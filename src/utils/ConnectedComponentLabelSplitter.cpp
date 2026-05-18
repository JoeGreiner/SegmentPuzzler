#include "ConnectedComponentLabelSplitter.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace segment_puzzler::connected_components {
namespace {

using SegmentIdType = dataType::SegmentIdType;

struct Component {
    SegmentIdType originalLabel = 0;
    std::ptrdiff_t seedIndex = 0;
    std::vector<std::ptrdiff_t> voxelIndices;
};

struct ImageGeometry {
    std::ptrdiff_t dimX = 0;
    std::ptrdiff_t dimY = 0;
    std::ptrdiff_t dimZ = 0;
    std::ptrdiff_t planeXY = 0;
    std::ptrdiff_t total = 0;
};

ImageGeometry geometryForImage(const dataType::SegmentsImageType::Pointer &image) {
    if (image.IsNull()) {
        throw std::invalid_argument("Cannot split components in a null label image.");
    }

    const auto size = image->GetLargestPossibleRegion().GetSize();
    ImageGeometry geometry;
    geometry.dimX = static_cast<std::ptrdiff_t>(size[0]);
    geometry.dimY = static_cast<std::ptrdiff_t>(size[1]);
    geometry.dimZ = static_cast<std::ptrdiff_t>(size[2]);
    geometry.planeXY = geometry.dimX * geometry.dimY;
    geometry.total = geometry.planeXY * geometry.dimZ;
    return geometry;
}

bool usesNeighborOffset(ConnectivityStencil connectivity, int dx, int dy, int dz) {
    if (dx == 0 && dy == 0 && dz == 0) {
        return false;
    }
    if (connectivity == ConnectivityStencil::Full) {
        return true;
    }
    return std::abs(dx) + std::abs(dy) + std::abs(dz) == 1;
}

template<typename Fn>
void forEachNeighbor(std::ptrdiff_t index,
                     const ImageGeometry &geometry,
                     ConnectivityStencil connectivity,
                     Fn &&fn) {
    const std::ptrdiff_t z = index / geometry.planeXY;
    const std::ptrdiff_t remainder = index % geometry.planeXY;
    const std::ptrdiff_t y = remainder / geometry.dimX;
    const std::ptrdiff_t x = remainder % geometry.dimX;

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!usesNeighborOffset(connectivity, dx, dy, dz)) {
                    continue;
                }

                const std::ptrdiff_t nx = x + dx;
                const std::ptrdiff_t ny = y + dy;
                const std::ptrdiff_t nz = z + dz;
                if (nx < 0 || ny < 0 || nz < 0 ||
                    nx >= geometry.dimX || ny >= geometry.dimY || nz >= geometry.dimZ) {
                    continue;
                }

                fn(nx + ny * geometry.dimX + nz * geometry.planeXY);
            }
        }
    }
}

SegmentIdType firstAvailableLabel(SegmentIdType label,
                                  const std::unordered_set<SegmentIdType> &ignoredLabels) {
    while (ignoredLabels.count(label) > 0) {
        if (label == std::numeric_limits<SegmentIdType>::max()) {
            throw std::overflow_error("No free segment label is available.");
        }
        ++label;
    }
    return label;
}

SegmentIdType nextAvailableLabel(SegmentIdType &nextFreeLabel,
                                 const std::unordered_set<SegmentIdType> &ignoredLabels) {
    nextFreeLabel = firstAvailableLabel(nextFreeLabel, ignoredLabels);
    if (nextFreeLabel == std::numeric_limits<SegmentIdType>::max()) {
        throw std::overflow_error("No free segment label is available.");
    }
    const SegmentIdType label = nextFreeLabel;
    ++nextFreeLabel;
    return label;
}

} // namespace

const char *connectivityStencilName(ConnectivityStencil connectivity) {
    switch (connectivity) {
        case ConnectivityStencil::SixConnected:
            return "6-connected";
        case ConnectivityStencil::Full:
            return "full";
    }
    return "unknown";
}

SegmentIdType maxLabelInImage(const dataType::SegmentsImageType::Pointer &image) {
    const ImageGeometry geometry = geometryForImage(image);
    const SegmentIdType *buffer = image->GetBufferPointer();
    SegmentIdType maxLabel = 0;
    for (std::ptrdiff_t index = 0; index < geometry.total; ++index) {
        maxLabel = std::max(maxLabel, buffer[index]);
    }
    return maxLabel;
}

ConnectedComponentSplitStats splitDisconnectedLabelComponentsInPlace(
    const dataType::SegmentsImageType::Pointer &image,
    const ConnectedComponentSplitOptions &options) {
    const ImageGeometry geometry = geometryForImage(image);
    SegmentIdType *buffer = image->GetBufferPointer();

    ConnectedComponentSplitStats stats;
    stats.maxLabel = maxLabelInImage(image);
    const SegmentIdType minFreeLabel =
        stats.maxLabel == std::numeric_limits<SegmentIdType>::max()
            ? stats.maxLabel
            : static_cast<SegmentIdType>(stats.maxLabel + 1);
    stats.nextFreeLabel = std::max(options.nextFreeLabel, minFreeLabel);
    stats.nextFreeLabel = firstAvailableLabel(stats.nextFreeLabel, options.ignoredLabels);

    std::vector<unsigned char> visited(static_cast<std::size_t>(geometry.total), 0);
    std::vector<std::ptrdiff_t> open;
    open.reserve(1024);
    std::unordered_map<SegmentIdType, std::vector<Component>> componentsByLabel;
    componentsByLabel.reserve(1024);

    for (std::ptrdiff_t seed = 0; seed < geometry.total; ++seed) {
        if (visited[static_cast<std::size_t>(seed)] != 0) {
            continue;
        }

        const SegmentIdType label = buffer[seed];
        visited[static_cast<std::size_t>(seed)] = 1;
        if (options.ignoredLabels.count(label) > 0) {
            continue;
        }

        Component component;
        component.originalLabel = label;
        component.seedIndex = seed;
        open.clear();
        open.push_back(seed);

        for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
            const std::ptrdiff_t current = open[queueIndex];
            component.voxelIndices.push_back(current);

            forEachNeighbor(current, geometry, options.connectivity, [&](std::ptrdiff_t neighbor) {
                const auto neighborIndex = static_cast<std::size_t>(neighbor);
                if (visited[neighborIndex] != 0 || buffer[neighbor] != label) {
                    return;
                }

                visited[neighborIndex] = 1;
                open.push_back(neighbor);
            });
        }

        componentsByLabel[label].push_back(std::move(component));
    }

    stats.labelsVisited = componentsByLabel.size();
    for (auto &entry : componentsByLabel) {
        const SegmentIdType originalLabel = entry.first;
        auto &components = entry.second;
        std::sort(components.begin(), components.end(), [](const Component &lhs, const Component &rhs) {
            if (lhs.voxelIndices.size() != rhs.voxelIndices.size()) {
                return lhs.voxelIndices.size() > rhs.voxelIndices.size();
            }
            return lhs.seedIndex < rhs.seedIndex;
        });

        auto &finalLabels = stats.finalLabelsByOriginalLabel[originalLabel];
        finalLabels.reserve(components.size());
        if (components.empty()) {
            continue;
        }

        finalLabels.push_back(originalLabel);
        if (components.size() == 1) {
            continue;
        }

        ++stats.labelsSplit;
        for (std::size_t componentIndex = 1; componentIndex < components.size(); ++componentIndex) {
            const SegmentIdType newLabel = nextAvailableLabel(stats.nextFreeLabel, options.ignoredLabels);
            finalLabels.push_back(newLabel);
            stats.maxLabel = std::max(stats.maxLabel, newLabel);
            ++stats.componentsCreated;

            for (const std::ptrdiff_t voxelIndex : components[componentIndex].voxelIndices) {
                buffer[voxelIndex] = newLabel;
            }
            stats.voxelsRelabeled += components[componentIndex].voxelIndices.size();
        }
    }

    return stats;
}

} // namespace segment_puzzler::connected_components
