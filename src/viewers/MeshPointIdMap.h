#ifndef MESH_POINT_ID_MAP_H
#define MESH_POINT_ID_MAP_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include <vtkType.h>

// Reuse across segments; grow with the number of unique points.
struct PointIdMap {
    static constexpr auto empty = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> entries = std::vector<std::uint32_t>(64, empty);
    std::vector<vtkIdType> sourceIds;
    int shift = 58;

    void reset() {
        std::fill(entries.begin(), entries.end(), empty);
        sourceIds.clear();
    }

    std::size_t slot(vtkIdType id) const {
        auto index = static_cast<std::size_t>((static_cast<std::uint64_t>(id)
            * 11400714819323198485ULL) >> shift);
        while (entries[index] != empty && sourceIds[entries[index]] != id) {
            index = (index + 1) & (entries.size() - 1);
        }
        return index;
    }

    std::pair<std::uint32_t, bool> insert(vtkIdType id) {
        auto index = slot(id);
        if (entries[index] != empty) {
            return {entries[index], false};
        }
        if (sourceIds.size() >= entries.size() / 2) {
            entries.assign(entries.size() * 2, empty);
            --shift;
            for (std::size_t i = 0; i < sourceIds.size(); ++i) {
                entries[slot(sourceIds[i])] = static_cast<std::uint32_t>(i);
            }
            index = slot(id);
        }
        const auto localId = static_cast<std::uint32_t>(sourceIds.size());
        sourceIds.push_back(id);
        entries[index] = localId;
        return {localId, true};
    }
};

#endif
