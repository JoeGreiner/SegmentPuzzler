#pragma once

#include "StreamVByteImageStorage.h"
#include "DenseImageStorage.h"
#include "src/file_definitions/dataTypes.h"
#include "src/utils/voxel.h"
#include <map>

namespace segment_puzzler {

inline dataType::SegmentsStorage::Pointer storeLabels(const dataType::SegmentsImageType &image) {
    return StreamVByteImageStorage<dataType::SegmentIdType>::fromImage(image);
}

inline dataType::SegmentsImageType::ConstPointer readVoxelRegion(
        const dataType::SegmentsStorage &image, const VoxelLists &lists) {
    auto first = image.GetLargestPossibleRegion().GetUpperIndex();
    auto last = image.GetLargestPossibleRegion().GetIndex();
    bool found = false;
    for (const auto *list : lists) for (const auto &run : list->getRuns()) {
        const dataType::SegmentsStorage::Index index{run.start.x, run.start.y, run.start.z};
        auto end = index;
        end[0] += run.length - 1;
        for (unsigned axis = 0; axis < 3; ++axis) {
            first[axis] = std::min(first[axis], index[axis]);
            last[axis] = std::max(last[axis], end[axis]);
        }
        found = true;
    }
    if (!found) return nullptr;
    dataType::SegmentsStorage::Region region;
    region.SetIndex(first);
    region.SetUpperIndex(last);
    return image.readImageRegion(region);
}

inline void setVoxelLists(dataType::SegmentsStorage &image, const VoxelLists &lists, dataType::SegmentIdType label) {
    std::map<int, std::vector<VoxelList::Run>> runsBySlice;
    for (const auto *list : lists) for (const auto &run : list->getRuns()) {
        const dataType::SegmentsStorage::Region region(
            {run.start.x, run.start.y, run.start.z}, {run.length, 1, 1});
        if (!image.GetLargestPossibleRegion().IsInside(region))
            throw std::out_of_range("Voxel run lies outside image storage");
        runsBySlice[run.start.z].push_back(run);
    }
    std::vector<dataType::SegmentsStorage::VoxelRange> ranges;
    unsigned planes = 0;
    for (auto entry = runsBySlice.begin(); entry != runsBySlice.end(); ++entry) {
        for (const auto &run : entry->second)
            ranges.push_back({{run.start.x, run.start.y, run.start.z}, run.length, label});
        if (++planes == 16 || std::next(entry) == runsBySlice.end()) {
            image.fillVoxelRanges(ranges);
            ranges.clear(); planes = 0;
        }
    }
}

} // namespace segment_puzzler
