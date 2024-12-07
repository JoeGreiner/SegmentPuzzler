
#include "workingNode.h"


WorkingNode::WorkingNode(InitialNode *pInitialNode,
                         SegmentIdType labelOfNewNode,
                         std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) :
        WorkingNode(std::vector<InitialNode *>{pInitialNode}, labelOfNewNode, allInitialNodes) {
}

WorkingNode::WorkingNode(std::vector<WorkingNode> vecOfWorkingNodesToConstructUpon,
                         SegmentIdType labelOfNewNode,
                         std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) :
        WorkingNode(convertVecOfWorkingNodesToVecOfInitialNodes(vecOfWorkingNodesToConstructUpon), labelOfNewNode,
                    allInitialNodes) {
}

WorkingNode::WorkingNode(std::vector<SegmentIdType> vecOfInitialNodeLabelsToConstructUpon,
                         SegmentIdType labelOfNewNode,
                         std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) :
        WorkingNode(convertVecOfInitialNodeLabelsToVecOfInitialNodes(vecOfInitialNodeLabelsToConstructUpon,
                                                                     allInitialNodes), labelOfNewNode,
                    allInitialNodes) {
}

WorkingNode::WorkingNode(std::set<SegmentIdType> setOfInitialNodeLabelsToConstructUpon,
                         SegmentIdType labelOfNewNode,
                         std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) :
        WorkingNode(convertSetOfInitialNodeLabelsToVecOfInitialNodes(setOfInitialNodeLabelsToConstructUpon,
                                                                     allInitialNodes), labelOfNewNode,
                    allInitialNodes) {
}

WorkingNode::WorkingNode(std::vector<InitialNode *> vecOfInitialNodesToConstructUpon,
                         SegmentIdType labelOfNewNode,
                         std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) {

    assert(!vecOfInitialNodesToConstructUpon.empty());

    label = labelOfNewNode;

    // calculate roi
    roi = Roi();
    for (auto &node : vecOfInitialNodesToConstructUpon) {
        roi.mergeRoiWith(node->roi);
    }

    // calculate initial nodes
    SegmentIdType initialNodeLabel;
    for (auto &initialNode : vecOfInitialNodesToConstructUpon) {
        initialNodeLabel = initialNode->getLabel();
        subInitialNodes[initialNodeLabel] = allInitialNodes[initialNodeLabel]; // get shared ptr, could be owning
    }

    // update working node entry of initial nodes
    for (auto &node : subInitialNodes) {
        node.second->setCurrentWorkingNodeLabel(labelOfNewNode);
    }

    //TODO: calculate: Node/Edge/Union features
    for (auto &feature : FeatureList::nodeFeaturesList) {
        addNodeFeature(feature.get());
    }

}

void WorkingNode::addNodeFeature(Feature *feature) {
//    nodeFeatures.push_back(std::unique_ptr<Feature>(feature->createNew()));
}

void WorkingNode::computeNodeFeatures() {
//    for(auto& nodeFeature : nodeFeatures){
//        nodeFeature->merge(getVoxelPointerArray(), )
//    }
}



std::vector<InitialNode *> WorkingNode::convertVecOfWorkingNodesToVecOfInitialNodes(
        std::vector<WorkingNode> &vecOfWorkingNodesToConstructUpon) {
    assert(!vecOfWorkingNodesToConstructUpon.empty());

    std::vector<InitialNode *> vecOfInitialNodesToConstructUpon;
    // it will be very likely bigger, lower bound for size
    vecOfInitialNodesToConstructUpon.reserve(vecOfWorkingNodesToConstructUpon.size());
    for (auto &workingNode : vecOfWorkingNodesToConstructUpon) {
        for (auto &initialNode : workingNode.subInitialNodes) {
            vecOfInitialNodesToConstructUpon.push_back(initialNode.second.get());
        }
    }

    return vecOfInitialNodesToConstructUpon;
}

std::vector<InitialNode *> WorkingNode::convertVecOfInitialNodeLabelsToVecOfInitialNodes(
        std::vector<SegmentIdType> &labelsOfInitialNodes,
        std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) {
    assert(!labelsOfInitialNodes.empty());

    std::vector<InitialNode *> vecOfInitialNodesToConstructUpon;
    vecOfInitialNodesToConstructUpon.reserve(labelsOfInitialNodes.size());
    for (auto &label : labelsOfInitialNodes) {
        vecOfInitialNodesToConstructUpon.push_back(allInitialNodes[label].get());
    }
    return vecOfInitialNodesToConstructUpon;
}


std::vector<InitialNode *> WorkingNode::convertSetOfInitialNodeLabelsToVecOfInitialNodes(
        std::set<SegmentIdType> &labelsOfInitialNodes,
        std::unordered_map<BaseNode::SegmentIdType, std::shared_ptr<InitialNode>> &allInitialNodes) {
    assert(!labelsOfInitialNodes.empty());

    std::vector<InitialNode *> vecOfInitialNodesToConstructUpon;
    vecOfInitialNodesToConstructUpon.reserve(labelsOfInitialNodes.size());
    for (auto &label : labelsOfInitialNodes) {
        vecOfInitialNodesToConstructUpon.push_back(allInitialNodes[label].get());
    }
    return vecOfInitialNodesToConstructUpon;
}


void WorkingNode::print(int indentationLevel, std::ostream &outStream) {
    std::string indentationString = "";
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outStream << indentationString << "label: " << label << "\n";
    outStream << indentationString << "number of twosided edges: " << twosidedEdges.size() << "\n";
    outStream << indentationString << "fx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << "\n";
    outStream << indentationString << "tx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << "\n";

    outStream << indentationString << "Twosided Edges:\n";
    for (auto &edge : twosidedEdges) {
        outStream << indentationString << "\tedgeKey: " << edge.second->pairId.first << " -> "
                  << edge.second->pairId.second << "\n";
        edge.second->print(indentationLevel + 3, outStream);
    }
    outStream << indentationString << "Sub-InitialNodes:";
    for (auto &node : subInitialNodes) {
        outStream << " " << node.first;
    }
    outStream << "\n";
}

std::vector<BaseNode::SegmentIdType> WorkingNode::getVectorOfConnectedNodeIds() {
    std::vector<BaseNode::SegmentIdType> vectorOfConnectedNodeIds;
    vectorOfConnectedNodeIds.reserve(twosidedEdges.size());
    for (auto &edge : twosidedEdges) {
        vectorOfConnectedNodeIds.push_back(edge.first);
    }
    return vectorOfConnectedNodeIds;
}


std::vector<std::vector<Voxel> *> WorkingNode::getVoxelPointerArray() {
    std::vector<std::vector<Voxel> *> voxelList;
    for (auto &initialNode : subInitialNodes) {
        voxelList.push_back(&initialNode.second->voxels);
    }
    return voxelList;
}

std::vector<Voxel> WorkingNode::getVoxelArray() {
//    this can be faster for certain cases, as everything is local in cache
    size_t totalVoxels = 0;
    for (const auto &initialNode : subInitialNodes) {
        totalVoxels += initialNode.second->voxels.size();
    }

    std::vector<Voxel> voxelArray;
    voxelArray.reserve(totalVoxels);

    for (const auto &initialNode : subInitialNodes) {
        voxelArray.insert(voxelArray.end(),
                          initialNode.second->voxels.begin(),
                          initialNode.second->voxels.end());
    }
    return voxelArray;
}
