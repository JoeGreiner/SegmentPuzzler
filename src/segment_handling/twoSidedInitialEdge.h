#ifndef SEGMENTCOUPLER_TWOSIDEDINITIALEDGE_H
#define SEGMENTCOUPLER_TWOSIDEDINITIALEDGE_H

#include "src/segment_handling/baseEdge.h"
#include "src/segment_handling/feature.h"
#include "src/segment_handling/oneSidedInitialEdge.h"

#include <memory>

class TwoSidedInitialEdge final : public BaseEdge {
public:
    TwoSidedInitialEdge(
        std::shared_ptr<OneSidedInitialEdge> smallerLabelOneSidedInitialEdge,
        std::shared_ptr<OneSidedInitialEdge> largerLabelOneSidedInitialEdge);

    const std::shared_ptr<const OneSidedInitialEdge> &getSmallerLabelOneSidedInitialEdge() const;
    const std::shared_ptr<const OneSidedInitialEdge> &getLargerLabelOneSidedInitialEdge() const;

    VoxelLists getVoxelLists() const override;
    std::size_t getVoxelCount() const;
    void calculateEdgeFeatures();
    void print(int indentationLevel, std::ostream &outStream) override;

    std::vector<std::unique_ptr<Feature>> edgeFeatures;

private:
    std::shared_ptr<const OneSidedInitialEdge> smallerLabelOneSidedInitialEdge;
    std::shared_ptr<const OneSidedInitialEdge> largerLabelOneSidedInitialEdge;
};

#endif // SEGMENTCOUPLER_TWOSIDEDINITIALEDGE_H
