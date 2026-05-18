#ifndef SEGMENTPUZZLER_CONNECTEDCOMPONENTLABELSPLITTER_H
#define SEGMENTPUZZLER_CONNECTEDCOMPONENTLABELSPLITTER_H

#include "src/file_definitions/dataTypes.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace segment_puzzler::connected_components {

enum class ConnectivityStencil {
    SixConnected,
    Full
};

struct ConnectedComponentSplitOptions {
    ConnectivityStencil connectivity = ConnectivityStencil::Full;
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

ConnectedComponentSplitStats splitDisconnectedLabelComponentsInPlace(
    const dataType::SegmentsImageType::Pointer &image,
    const ConnectedComponentSplitOptions &options);

} // namespace segment_puzzler::connected_components

#endif // SEGMENTPUZZLER_CONNECTEDCOMPONENTLABELSPLITTER_H
