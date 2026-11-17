#pragma once

#include "StreamVByteImageStorage.h"
#include "DenseImageStorage.h"
#include "src/file_definitions/dataTypes.h"
#include "src/utils/voxel.h"

namespace segment_puzzler {

inline dataType::SegmentsStorage::Pointer storeLabels(const dataType::SegmentsImageType &image) {
    return StreamVByteImageStorage<dataType::SegmentIdType>::fromImage(image);
}

inline dataType::SegmentsImageType::ConstPointer readVoxelRegion(
        const dataType::SegmentsStorage &image, const VoxelLists &lists) {
    auto first = image.GetLargestPossibleRegion().GetUpperIndex();
    auto last = image.GetLargestPossibleRegion().GetIndex();
    bool found = false;
    for (const auto *list : lists) for (const auto &voxel : *list) {
        const dataType::SegmentsStorage::Index index{voxel.x, voxel.y, voxel.z};
        for (unsigned axis = 0; axis < 3; ++axis) {
            first[axis] = std::min(first[axis], index[axis]);
            last[axis] = std::max(last[axis], index[axis]);
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
    std::vector<dataType::SegmentsStorage::Index> indices;
    std::size_t count = 0;
    for (const auto *list : lists) count += list->size();
    indices.reserve(count);
    for (const auto *list : lists)
        for (const auto &voxel : *list) indices.push_back({voxel.x, voxel.y, voxel.z});
    image.setVoxelValues(indices, label);
}

} // namespace segment_puzzler
