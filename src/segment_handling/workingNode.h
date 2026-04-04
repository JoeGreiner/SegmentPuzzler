
#ifndef SEGMENTCOUPLER_WORKINGNODE_H
#define SEGMENTCOUPLER_WORKINGNODE_H


#include "src/segment_handling/baseNode.h"
#include "workingEdge.h"
#include "initialNode.h"
#include <unordered_map>

class WorkingNode : public BaseNode {

public:
    WorkingNode(InitialNode *pInitialNode,
                SegmentIdType labelOfNewNode,
                std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes);

    WorkingNode(std::vector<WorkingNode> vecOfWorkingNodesToConstructUpon,
                SegmentIdType labelOfNewNode,
                std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes);

    WorkingNode(std::vector<InitialNode *> vecOfInitialNodesToConstructUpon,
                SegmentIdType labelOfNewNode,
                std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes);

    WorkingNode(std::set<SegmentIdType> setOfInitialNodeLabelsToConstructUpon, SegmentIdType labelOfNewNode,
                std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes);

    WorkingNode(std::vector<SegmentIdType> vecOfInitialNodeLabelsToConstructUpon, SegmentIdType labelOfNewNode,
                std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes);

    ~WorkingNode() = default;

    std::unordered_map<SegmentIdType, std::shared_ptr<WorkingEdge>> twosidedEdges;
    std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> subInitialNodes;

    //FIXME: Implement me
    void print(int indentationLevel, std::ostream &outStream) override;

    std::vector<std::shared_ptr<Feature>> nodeFeatures;

    std::vector<std::vector<Voxel> *> getVoxelPointerArray() override;

    std::vector<Voxel> getVoxelArray() override;

    std::vector<SegmentIdType> getVectorOfConnectedNodeIds() override;

    void addNodeFeature(Feature *feature);

    void computeNodeFeatures();

private:
    static std::vector<InitialNode *>
    convertVecOfWorkingNodesToVecOfInitialNodes(std::vector<WorkingNode> &vecOfWorkingNodes);

    template<typename Container>
    static std::vector<InitialNode *>
    convertInitialNodeLabelsToVecOfInitialNodes(
            const Container &labelsOfInitialNodes,
            std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes);
};

template<typename Container>
std::vector<InitialNode *> WorkingNode::convertInitialNodeLabelsToVecOfInitialNodes(
        const Container &labelsOfInitialNodes,
        std::unordered_map<SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) {
    assert(!labelsOfInitialNodes.empty());
    std::vector<InitialNode *> result;
    result.reserve(labelsOfInitialNodes.size());
    for (const auto &lbl : labelsOfInitialNodes) {
        result.push_back(allInitialNodes[lbl].get());
    }
    return result;
}


#endif //SEGMENTCOUPLER_WORKINGNODE_H
