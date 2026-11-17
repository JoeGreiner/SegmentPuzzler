#pragma once

#include "oneSidedInitialEdge.h"
#include <algorithm>
#include <tuple>

namespace segment_puzzler {

template<class Visitor>
void visitLabelContacts(const dataType::SegmentsStorage &image, dataType::SegmentsStorage::Region readRegion,
                        itk::IndexValueType firstOwnedZ, const Visitor &addContact) {
    using SegmentIdType = dataType::SegmentIdType;
    using SegmentIdImageType = dataType::SegmentsImageType;
    struct RowRun { itk::IndexValueType begin, end; SegmentIdType label; };
    using Row = std::vector<RowRun>;
    const auto origin = readRegion.GetIndex();
    const auto height = readRegion.GetSize()[1];
    const auto endZ = origin[2] + static_cast<itk::IndexValueType>(readRegion.GetSize()[2]);
    std::vector<Row> previous(height), current(height);
    const auto compareRows = [&](const Row &a, const Row &b, SegmentIdImageType::IndexType first,
                                 SegmentIdImageType::IndexType second) {
        std::size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            const auto begin = std::max(a[i].begin, b[j].begin);
            const auto end = std::min(a[i].end, b[j].end);
            first[0] = second[0] = begin;
            if (begin < end) addContact(a[i].label, b[j].label, first, second, end - begin);
            const auto endA = a[i].end, endB = b[j].end;
            if (endA <= endB) ++i;
            if (endB <= endA) ++j;
        }
    };
    readRegion.SetSize(2, 1);
    for (auto z = origin[2]; z < endZ; ++z) {
        previous.swap(current);
        for (auto &row : current) row.clear();
        readRegion.SetIndex(2, z);
        image.forEachRun(readRegion, [&](const auto &index, std::size_t length, SegmentIdType value) {
            current[index[1] - origin[1]].push_back({index[0], index[0] + static_cast<itk::IndexValueType>(length), value});
        });
        if (z < firstOwnedZ) continue;
        for (std::size_t row = 0; row < height; ++row) {
            const auto y = origin[1] + static_cast<itk::IndexValueType>(row);
            const auto &runs = current[row];
            for (std::size_t i = 1; i < runs.size(); ++i)
                addContact(runs[i - 1].label, runs[i].label, {runs[i].begin - 1, y, z}, {runs[i].begin, y, z}, 1);
            if (row) compareRows(current[row - 1], runs, {0, y - 1, z}, {0, y, z});
            if (z > origin[2]) compareRows(previous[row], runs, {0, y, z - 1}, {0, y, z});
        }
    }
}

inline std::shared_ptr<OneSidedInitialEdge> makeContactEdge(dataType::SegmentIdType label,
        dataType::SegmentIdType neighbor, std::vector<VoxelList::Run> &runs) {
    std::sort(runs.begin(), runs.end(), [](const auto &a, const auto &b) {
        return std::tie(a.start.z, a.start.y, a.start.x) < std::tie(b.start.z, b.start.y, b.start.x);
    });
    auto end = runs.begin();
    for (const auto &run : runs) {
        if (end != runs.begin() && (end - 1)->start.z == run.start.z && (end - 1)->start.y == run.start.y &&
            std::int64_t((end - 1)->start.x) + (end - 1)->length >= run.start.x) {
            auto &last = *(end - 1);
            last.length = std::max<std::int64_t>(last.length, std::int64_t(run.start.x) + run.length - last.start.x);
        } else *end++ = run;
    }
    auto edge = std::make_shared<OneSidedInitialEdge>(neighbor, label);
    edge->voxels.reserveRuns(end - runs.begin());
    for (auto it = runs.begin(); it != end; ++it) edge->addRun(it->start, it->length);
    return edge;
}

} // namespace segment_puzzler
