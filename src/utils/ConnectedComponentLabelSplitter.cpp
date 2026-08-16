#include "ConnectedComponentLabelSplitter.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#ifdef USE_OMP
#include <omp.h>
#endif

namespace segment_puzzler::connected_components {
namespace {

using SegmentIdType = dataType::SegmentIdType;

#ifdef USE_OMP
constexpr std::size_t kMaxRegionComponentThreads = 8;
#endif

struct Component {
    SegmentIdType originalLabel = 0;
    std::ptrdiff_t seedIndex = 0;
    std::vector<std::ptrdiff_t> voxelIndices;
};

using ComponentsByLabel = std::unordered_map<SegmentIdType, std::vector<Component>>;

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

template<typename IncludeLabel>
ComponentsByLabel collectComponentsByLabel(
    const dataType::SegmentsImageType::Pointer &image,
    ConnectivityStencil connectivity,
    IncludeLabel &&includeLabel) {
    const detail::ImageGeometry geometry = detail::geometryForImage(image);
    const SegmentIdType *buffer = image->GetBufferPointer();

    std::vector<unsigned char> visited(static_cast<std::size_t>(geometry.total), 0);
    std::vector<std::ptrdiff_t> open;
    open.reserve(1024);
    ComponentsByLabel componentsByLabel;
    componentsByLabel.reserve(1024);

    for (std::ptrdiff_t seed = 0; seed < geometry.total; ++seed) {
        if (visited[static_cast<std::size_t>(seed)] != 0) {
            continue;
        }

        const SegmentIdType label = buffer[seed];
        visited[static_cast<std::size_t>(seed)] = 1;
        if (!includeLabel(label)) {
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

            detail::forEachNeighbor(current, geometry, connectivity, [&](std::ptrdiff_t neighbor) {
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
    return componentsByLabel;
}

std::size_t countLabelComponentsInRegion(
    const dataType::SegmentsImageType::Pointer &image,
    SegmentIdType label,
    dataType::SegmentsImageType::RegionType region,
    ConnectivityStencil connectivity) {
    const auto imageGeometry = detail::geometryForImage(image);
    const auto imageRegion = image->GetLargestPossibleRegion();
    if (!region.Crop(imageRegion)) {
        return 0;
    }

    const auto imageStart = imageRegion.GetIndex();
    const auto regionStart = region.GetIndex();
    const auto regionGeometry = detail::geometryForRegion(region);
    if (regionGeometry.total <= 0) {
        return 0;
    }

    const std::ptrdiff_t regionOffsetX = regionStart[0] - imageStart[0];
    const std::ptrdiff_t regionOffsetY = regionStart[1] - imageStart[1];
    const std::ptrdiff_t regionOffsetZ = regionStart[2] - imageStart[2];
    const std::ptrdiff_t regionBase =
        regionOffsetX + regionOffsetY * imageGeometry.dimX + regionOffsetZ * imageGeometry.planeXY;
    const auto fullImageIndex = [&](std::ptrdiff_t regionIndex) {
        const std::ptrdiff_t z = regionIndex / regionGeometry.planeXY;
        const std::ptrdiff_t withinSlice = regionIndex - z * regionGeometry.planeXY;
        const std::ptrdiff_t y = withinSlice / regionGeometry.dimX;
        const std::ptrdiff_t x = withinSlice - y * regionGeometry.dimX;
        return regionBase + x + y * imageGeometry.dimX + z * imageGeometry.planeXY;
    };

    const SegmentIdType *buffer = image->GetBufferPointer();
    std::vector<unsigned char> visited(static_cast<std::size_t>(regionGeometry.total), 0);
    std::vector<std::ptrdiff_t> open;
    open.reserve(1024);
    std::size_t componentCount = 0;

    for (std::ptrdiff_t seed = 0; seed < regionGeometry.total; ++seed) {
        const auto seedIndex = static_cast<std::size_t>(seed);
        if (visited[seedIndex] != 0) {
            continue;
        }
        visited[seedIndex] = 1;
        if (buffer[fullImageIndex(seed)] != label) {
            continue;
        }

        ++componentCount;
        open.clear();
        open.push_back(seed);
        for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
            detail::forEachNeighbor(open[queueIndex], regionGeometry, connectivity, [&](std::ptrdiff_t neighbor) {
                const auto neighborIndex = static_cast<std::size_t>(neighbor);
                if (visited[neighborIndex] != 0) {
                    return;
                }
                visited[neighborIndex] = 1;
                if (buffer[fullImageIndex(neighbor)] == label) {
                    open.push_back(neighbor);
                }
            });
        }
    }
    return componentCount;
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
    const detail::ImageGeometry geometry = detail::geometryForImage(image);
    const SegmentIdType *buffer = image->GetBufferPointer();
    SegmentIdType maxLabel = 0;
    for (std::ptrdiff_t index = 0; index < geometry.total; ++index) {
        maxLabel = std::max(maxLabel, buffer[index]);
    }
    return maxLabel;
}

std::unordered_map<SegmentIdType, std::size_t> countConnectedComponentsByLabel(
    const dataType::SegmentsImageType::Pointer &image,
    const std::unordered_set<SegmentIdType> &labels,
    ConnectivityStencil connectivity) {
    std::unordered_map<SegmentIdType, std::size_t> componentCounts;
    componentCounts.reserve(labels.size());
    for (const SegmentIdType label : labels) {
        componentCounts[label] = 0;
    }
    if (labels.empty()) {
        return componentCounts;
    }

    const auto componentsByLabel = collectComponentsByLabel(
        image,
        connectivity,
        [&labels](SegmentIdType label) { return labels.count(label) > 0; });
    for (const auto &[label, components] : componentsByLabel) {
        componentCounts[label] = components.size();
    }
    return componentCounts;
}

std::unordered_map<SegmentIdType, std::size_t> countConnectedComponentsByLabelInRegions(
    const dataType::SegmentsImageType::Pointer &image,
    const std::unordered_map<SegmentIdType, dataType::SegmentsImageType::RegionType> &regionsByLabel,
    ConnectivityStencil connectivity) {
    using LabelRegion =
        std::pair<SegmentIdType, dataType::SegmentsImageType::RegionType>;

    std::unordered_map<SegmentIdType, std::size_t> componentCounts;
    componentCounts.reserve(regionsByLabel.size());
    if (regionsByLabel.empty()) {
        return componentCounts;
    }

    std::vector<LabelRegion> labelRegions;
    labelRegions.reserve(regionsByLabel.size());
    for (const auto &entry : regionsByLabel) {
        labelRegions.push_back(entry);
    }

    std::vector<std::size_t> counts(labelRegions.size(), 0);
    std::vector<std::exception_ptr> failures(labelRegions.size());
#ifdef USE_OMP
    const std::size_t availableThreads =
        static_cast<std::size_t>(std::max(1, omp_get_max_threads()));
    const int threadCount = static_cast<int>(std::min(
        {labelRegions.size(), availableThreads, kMaxRegionComponentThreads}));
#pragma omp parallel for schedule(dynamic, 1) num_threads(threadCount) if(threadCount > 1)
#endif
    for (std::ptrdiff_t index = 0;
         index < static_cast<std::ptrdiff_t>(labelRegions.size());
         ++index) {
        try {
            const auto &[label, region] = labelRegions[static_cast<std::size_t>(index)];
            counts[static_cast<std::size_t>(index)] =
                countLabelComponentsInRegion(image, label, region, connectivity);
        } catch (...) {
            failures[static_cast<std::size_t>(index)] = std::current_exception();
        }
    }

    for (std::size_t index = 0; index < labelRegions.size(); ++index) {
        if (failures[index] != nullptr) {
            std::rethrow_exception(failures[index]);
        }
        componentCounts.emplace(labelRegions[index].first, counts[index]);
    }
    return componentCounts;
}

ConnectedComponentSplitStats splitDisconnectedLabelComponentsInPlace(
    const dataType::SegmentsImageType::Pointer &image,
    const ConnectedComponentSplitOptions &options) {
    SegmentIdType *buffer = image->GetBufferPointer();

    ConnectedComponentSplitStats stats;
    stats.maxLabel = maxLabelInImage(image);
    const SegmentIdType minFreeLabel =
        stats.maxLabel == std::numeric_limits<SegmentIdType>::max()
            ? stats.maxLabel
            : static_cast<SegmentIdType>(stats.maxLabel + 1);
    stats.nextFreeLabel = std::max(options.nextFreeLabel, minFreeLabel);

    auto componentsByLabel = collectComponentsByLabel(
        image,
        options.connectivity,
        [&options](SegmentIdType label) {
            return options.ignoredLabels.count(label) == 0
                   && (options.includedLabels.empty()
                       || options.includedLabels.count(label) > 0);
        });

    stats.labelsVisited = componentsByLabel.size();
    std::vector<SegmentIdType> originalLabels;
    originalLabels.reserve(componentsByLabel.size());
    for (const auto &entry : componentsByLabel) {
        originalLabels.push_back(entry.first);
    }
    std::sort(originalLabels.begin(), originalLabels.end());

    // Allocate every required label before changing the image. This keeps a
    // label-space failure from leaving a partially relabelled segmentation.
    for (const SegmentIdType originalLabel : originalLabels) {
        auto &components = componentsByLabel.at(originalLabel);
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
        }
    }

    for (const SegmentIdType originalLabel : originalLabels) {
        const auto &components = componentsByLabel.at(originalLabel);
        const auto &finalLabels = stats.finalLabelsByOriginalLabel.at(originalLabel);
        for (std::size_t componentIndex = 1; componentIndex < components.size(); ++componentIndex) {
            const SegmentIdType newLabel = finalLabels[componentIndex];
            for (const std::ptrdiff_t voxelIndex : components[componentIndex].voxelIndices) {
                buffer[voxelIndex] = newLabel;
            }
            stats.voxelsRelabeled += components[componentIndex].voxelIndices.size();
        }
    }

    return stats;
}

} // namespace segment_puzzler::connected_components
