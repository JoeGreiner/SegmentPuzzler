#include "src/viewers/VolumeContext.h"
#include "src/utils/WorkerThreads.h"
#include "src/image_storage/DenseImageStorage.h"
#include "ConnectedComponentLabelSplitter.h"

#include <algorithm>
#include <exception>
#include <map>
#include <numeric>
#ifdef USE_OMP
#include <omp.h>
#endif

namespace segment_puzzler::connected_components {
namespace {

using SegmentIdType = dataType::SegmentIdType;
using Storage = dataType::SegmentsStorage;

struct Run {
    std::size_t begin, end, parent;
    SegmentIdType label;
    unsigned char rank = 0;
};

struct Component {
    std::size_t root, voxelCount, firstRun;
};

struct RunComponents {
    detail::ImageGeometry geometry;
    std::vector<Run> runs;
    std::vector<std::size_t> rowOffsets;
    std::map<SegmentIdType, std::vector<Component>> byLabel;

    std::size_t root(std::size_t index) {
        while (runs[index].parent != index) {
            runs[index].parent = runs[runs[index].parent].parent;
            index = runs[index].parent;
        }
        return index;
    }

    void connect(std::size_t first, std::size_t second) {
        first = root(first); second = root(second);
        if (first == second) return;
        if (runs[first].rank < runs[second].rank) std::swap(first, second);
        runs[second].parent = first;
        if (runs[first].rank == runs[second].rank) ++runs[first].rank;
    }

    void connectRows(std::size_t row, std::size_t neighbor, std::size_t reach) {
        auto firstNeighbor = rowOffsets[neighbor];
        const auto neighborEnd = rowOffsets[neighbor + 1];
        for (auto i = rowOffsets[row]; i < rowOffsets[row + 1]; ++i) {
            const auto &run = runs[i];
            while (firstNeighbor < neighborEnd && runs[firstNeighbor].end + reach <= run.begin)
                ++firstNeighbor;
            for (auto j = firstNeighbor; j < neighborEnd && runs[j].begin < run.end + reach; ++j)
                if (run.label == runs[j].label) connect(i, j);
        }
    }
};

template<typename IncludeLabel>
RunComponents collectRunComponents(const Storage &image, Storage::Region region,
                                   ConnectivityStencil connectivity, IncludeLabel includeLabel) {
    RunComponents result;
    if (!region.Crop(image.GetLargestPossibleRegion())) return result;
    result.geometry = detail::geometryForRegion(region);
    const auto &geometry = result.geometry;
    const auto height = static_cast<std::size_t>(geometry.dimY);
    const auto depth = static_cast<std::size_t>(geometry.dimZ);
    result.rowOffsets.resize(height * depth + 1, 0);
    image.forEachRun(region, [&](const auto &start, std::size_t, SegmentIdType label) {
        if (!includeLabel(label)) return;
        const auto row = (start[2] - geometry.start[2]) * geometry.dimY + start[1] - geometry.start[1];
        ++result.rowOffsets[row + 1];
    });
    std::partial_sum(result.rowOffsets.begin(), result.rowOffsets.end(), result.rowOffsets.begin());
    if (result.rowOffsets.back() == 0) return result;
    // Count first to allocate the run array once.
    result.runs.reserve(result.rowOffsets.back());
    image.forEachRun(region, [&](const auto &start, std::size_t length, SegmentIdType label) {
        if (!includeLabel(label)) return;
        const auto x = static_cast<std::size_t>(start[0] - geometry.start[0]);
        result.runs.push_back({x, x + length, result.runs.size(), label});
    });

    const auto reach = connectivity == ConnectivityStencil::Full ? 1U : 0U;
    for (std::size_t z = 0; z < depth; ++z) for (std::size_t y = 0; y < height; ++y) {
        const auto row = z * height + y;
        for (auto i = result.rowOffsets[row] + 1; i < result.rowOffsets[row + 1]; ++i)
            if (result.runs[i - 1].end == result.runs[i].begin && result.runs[i - 1].label == result.runs[i].label)
                result.connect(i - 1, i);
        if (y > 0) result.connectRows(row, row - 1, reach);
        if (z > 0) {
            result.connectRows(row, row - height, reach);
            if (reach && y > 0) result.connectRows(row, row - height - 1, reach);
            if (reach && y + 1 < height) result.connectRows(row, row - height + 1, reach);
        }
    }

    std::unordered_map<std::size_t, Component> components;
    for (std::size_t i = 0; i < result.runs.size(); ++i) {
        auto &run = result.runs[i];
        run.parent = result.root(i);
        const auto entry = components.try_emplace(run.parent, Component{run.parent, 0, i}).first;
        entry->second.voxelCount += run.end - run.begin;
    }
    for (const auto &[root, component] : components)
        result.byLabel[result.runs[root].label].push_back(component);
    return result;
}

std::size_t countLabelComponentsInRegion(const Storage &image, SegmentIdType label,
                                        Storage::Region region, ConnectivityStencil connectivity) {
    const auto components = collectRunComponents(image, region, connectivity,
        [label](SegmentIdType value) { return value == label; });
    const auto found = components.byLabel.find(label);
    return found == components.byLabel.end() ? 0 : found->second.size();
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
    const std::unordered_set<SegmentIdType> &labels, ConnectivityStencil connectivity) {
    std::unordered_map<SegmentIdType, std::size_t> counts;
    if (labels.empty()) return counts;
    const auto storage = DenseImageStorage<SegmentIdType>::fromImage(image);
    const auto components = collectRunComponents(*storage, storage->GetLargestPossibleRegion(), connectivity,
        [&](SegmentIdType label) { return labels.count(label) > 0; });
    for (const auto label : labels) {
        const auto found = components.byLabel.find(label);
        counts[label] = found == components.byLabel.end() ? 0 : found->second.size();
    }
    return counts;
}

std::unordered_map<SegmentIdType, std::size_t> countConnectedComponentsByLabelInRegions(
    const Storage::Pointer &image,
    const std::unordered_map<SegmentIdType, dataType::SegmentsImageType::RegionType> &regionsByLabel,
    ConnectivityStencil connectivity) {
    if (regionsByLabel.empty()) return {};
    if (!image) throw std::invalid_argument("Cannot traverse components in a null label image.");
    std::vector<std::pair<SegmentIdType, Storage::Region>> regions(regionsByLabel.begin(), regionsByLabel.end());
    std::vector<std::size_t> counts(regions.size());
    std::vector<std::exception_ptr> failures(regions.size());
#ifdef USE_OMP
    const int threadCount = workerThreadCount();
#pragma omp parallel for schedule(dynamic, 1) num_threads(threadCount)
#endif
    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(regions.size()); ++i) {
        try {
            counts[i] = countLabelComponentsInRegion(*image, regions[i].first, regions[i].second, connectivity);
        } catch (...) { failures[i] = std::current_exception(); }
    }
    std::unordered_map<SegmentIdType, std::size_t> result;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (failures[i]) std::rethrow_exception(failures[i]);
        result.emplace(regions[i].first, counts[i]);
    }
    return result;
}

std::unordered_map<SegmentIdType, std::size_t> countConnectedComponentsByLabelInRegions(
    const dataType::SegmentsImageType::Pointer &image,
    const std::unordered_map<SegmentIdType, dataType::SegmentsImageType::RegionType> &regionsByLabel,
    ConnectivityStencil connectivity) {
    if (regionsByLabel.empty()) return {};
    Storage::Pointer storage = DenseImageStorage<SegmentIdType>::fromImage(image);
    return countConnectedComponentsByLabelInRegions(storage, regionsByLabel, connectivity);
}

ConnectedComponentSplitStats splitDisconnectedLabelComponentsInPlace(
    const Storage::Pointer &image, const ConnectedComponentSplitOptions &options) {
    (void)detail::geometryForImage(image);
    auto region = image->GetLargestPossibleRegion();
    ConnectedComponentSplitStats stats;
    if (!options.includedLabels.empty()) {
        Storage::LabelStatistics selected;
        for (const auto &[label, statistics] : image->labelStatistics()) {
            if (options.ignoredLabels.count(label)) continue;
            stats.maxLabel = std::max(stats.maxLabel, label);
            if (options.includedLabels.count(label)) selected.include(statistics.bounds, statistics.voxelCount);
        }
        if (!selected.voxelCount) {
            stats.nextFreeLabel = std::max(options.nextFreeLabel, std::uint64_t(stats.maxLabel) + 1);
            return stats;
        }
        region = selected.bounds;
    }
    auto components = collectRunComponents(*image, region, options.connectivity, [&](SegmentIdType label) {
        if (options.ignoredLabels.count(label)) return false;
        stats.maxLabel = std::max(stats.maxLabel, label);
        return options.includedLabels.empty() || options.includedLabels.count(label) > 0;
    });
    stats.nextFreeLabel = std::max(options.nextFreeLabel, std::uint64_t(stats.maxLabel) + 1);
    stats.labelsVisited = components.byLabel.size();
    std::unordered_map<std::size_t, SegmentIdType> newLabels;
    // Reserve all IDs before writing any changed runs.
    for (auto &[label, parts] : components.byLabel) {
        std::sort(parts.begin(), parts.end(), [](const Component &a, const Component &b) {
            if (a.voxelCount != b.voxelCount) return a.voxelCount > b.voxelCount;
            return a.firstRun < b.firstRun;
        });
        auto &finalLabels = stats.finalLabelsByOriginalLabel[label];
        finalLabels.push_back(label);
        if (parts.size() > 1) ++stats.labelsSplit;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const auto next = volume_io::nextAvailableLabel(stats.nextFreeLabel, options.ignoredLabels);
            const auto newLabel = static_cast<SegmentIdType>(next);
            stats.nextFreeLabel = next + 1;
            stats.maxLabel = std::max(stats.maxLabel, newLabel);
            finalLabels.push_back(newLabel);
            newLabels.emplace(parts[i].root, newLabel);
            ++stats.componentsCreated;
            stats.voxelsRelabeled += parts[i].voxelCount;
        }
    }
    if (!stats.changed()) return stats;

    const auto &start = components.geometry.start;
    const auto height = static_cast<std::size_t>(components.geometry.dimY);
    const auto depth = static_cast<std::size_t>(components.geometry.dimZ);
    std::vector<Storage::VoxelRange> edits;
    for (std::size_t z = 0; z < depth; ++z) {
        for (std::size_t y = 0; y < height; ++y) {
            const auto row = z * height + y;
            for (auto i = components.rowOffsets[row]; i < components.rowOffsets[row + 1]; ++i) {
                const auto &run = components.runs[i];
                const auto found = newLabels.find(run.parent);
                if (found != newLabels.end())
                    edits.push_back({{start[0] + itk::IndexValueType(run.begin), start[1] + itk::IndexValueType(y),
                                      start[2] + itk::IndexValueType(z)}, run.end - run.begin, found->second});
            }
        }
        if ((z + 1) % 16 == 0 || z + 1 == depth) {
            image->fillVoxelRanges(edits);
            edits.clear();
        }
    }
    return stats;
}

ConnectedComponentSplitStats splitDisconnectedLabelComponentsInPlace(
    const dataType::SegmentsImageType::Pointer &image, const ConnectedComponentSplitOptions &options) {
    Storage::Pointer storage = DenseImageStorage<SegmentIdType>::fromImage(image);
    return splitDisconnectedLabelComponentsInPlace(storage, options);
}

} // namespace segment_puzzler::connected_components
