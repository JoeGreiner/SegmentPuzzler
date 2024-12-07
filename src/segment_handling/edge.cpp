

#include "edge.h"
#include "graph.h"


//Edge::~Edge(){
//    std::cout << "WARNING: explicit dtor should be only used in debugging!!! Destroying Edge!" << std::endl;
//}

Edge::Edge(Voxel voxel1, Voxel voxel2) : roi(Roi(voxel1)) {
    GraphBase::edgeCounter++;
    edgeId = GraphBase::edgeCounter;
//    std::cout << "Creating Edge, id: " << edgeId << std::endl;
    shouldMerge = 0;
    edgeComputed = false;
    GraphBase::SegmentsVoxelType labelA, labelB;
    GraphBase::SegmentsImageType::IndexType index{0, 0, 0};
    labelA = GraphBase::pSegments->GetPixel(index = {voxel1.x, voxel1.y, voxel1.z});
    labelB = GraphBase::pSegments->GetPixel(index = {voxel2.x, voxel2.y, voxel2.z});
    labelSmaller = (labelA > labelB) ? labelB : labelA;
    labelBigger = (labelA > labelB) ? labelA : labelB;
    pGraph->edgeIdLookup[edgeId] = std::pair<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType>
            (labelSmaller, labelBigger);

    voxels.push_back(voxel1);
    voxels.push_back(voxel2);
    roi.updateBoundingRoi(voxel2);
    for (auto &feature : edgeFeaturesList) {
        addEdgeFeature(feature);
    }
    for (auto &feature : unionFeaturesList) {
        addUnionFeature(feature);
    }
}

Edge::Edge(std::vector<Voxel> voxelList, Roi roiIn, unsigned int labelFrom, unsigned int labelTo) {
    GraphBase::edgeCounter++;
    edgeId = GraphBase::edgeCounter;
//    std::cout << "Creating Edge, id: " << edgeId << std::endl;
    shouldMerge = 0;
    edgeComputed = false;

    labelSmaller = (labelTo > labelFrom) ? labelFrom : labelTo;
    labelBigger = (labelTo > labelFrom) ? labelTo : labelFrom;
    pGraph->edgeIdLookup[edgeId] = std::pair<unsigned int, unsigned int>(labelSmaller, labelBigger);

    roi = roiIn;
    voxels = std::move(voxelList);
    for (auto &feature : edgeFeaturesList) {
        addEdgeFeature(feature);
    }
    for (auto &feature : unionFeaturesList) {
        addUnionFeature(feature);
    }
}


Edge::Edge(Voxel voxel, unsigned int labelA, unsigned int labelB, bool registerEdge) {
    GraphBase::edgeCounter++;
    edgeId = GraphBase::edgeCounter;
//    std::cout << "Creating Edge, id: " << edgeId << std::endl;
    shouldMerge = 0;
    edgeComputed = false;

    labelSmaller = (labelA > labelB) ? labelB : labelA;
    labelBigger = (labelA > labelB) ? labelA : labelB;
    if (registerEdge) {
        pGraph->edgeIdLookup[edgeId] = std::pair<unsigned int, unsigned int>(labelSmaller, labelBigger);
    }

    voxels.push_back(voxel);
    roi = Roi(voxel);
    for (auto &feature : edgeFeaturesList) {
        addEdgeFeature(feature);
    }
    for (auto &feature : unionFeaturesList) {
        addUnionFeature(feature);
    }
}

void Edge::addEdgeFeature(std::unique_ptr<Feature> &feature) {
    edgeFeatures.push_back(feature->createNew());
}

void Edge::addUnionFeature(std::unique_ptr<Feature> &feature) {
    unionFeatures.push_back(feature->createNew());
}

void Edge::mergeVoxelsAndROIwithOtherEdge(std::shared_ptr<Edge> edgeToMerge) {
    roi.mergeRoiWith(edgeToMerge->roi);
    voxels.insert(voxels.end(), edgeToMerge->voxels.begin(), edgeToMerge->voxels.end());
}

std::vector<float> Edge::getEdgeFeatureByName(std::string filterName, std::string signalName) {
    bool featureFound = false;
    std::vector<float> featureValues;
    for (auto &feature : edgeFeatures) {
        if (feature->filterName == filterName) {
            if (feature->signalName == signalName) {
                featureFound = true;
                featureValues = feature->values;
                break;
            }
        }
    }
    if (!featureFound) {
        std::string errorString = "The  requested edgefilter '" + filterName + "' for the signal '" + signalName +
                                  "' was not found. Was it added?";
        throw std::logic_error(errorString);
    }
    return featureValues;
}

std::vector<float> Edge::getUnionFeatureByName(std::string filterName, std::string signalName) {
    bool featureFound = false;
    std::vector<float> featureValues;
    for (auto &feature : unionFeatures) {
        if (feature->filterName == filterName) {
            if (feature->signalName == signalName) {
                featureFound = true;
                featureValues = feature->values;
                break;
            }
        }
    }
    if (!featureFound) {
        std::string errorString = "The requested unionFilter '" + filterName + "' for the signal '" + signalName +
                                  "' was not found. Was it added?";
        throw std::logic_error(errorString);
    }
    return featureValues;
}


void Edge::calculateEdgeFeatures() {
    for (auto &feature : edgeFeatures) {
        feature->compute(voxels, labelSmaller, labelBigger);
    }
}

void Edge::calculateUnionFeatures() {
    for (auto &feature : unionFeatures) {
        feature->compute(voxels, labelSmaller, labelBigger);
    }
}


void Edge::addVoxel(Voxel voxel1, Voxel voxel2) {
    voxels.push_back(voxel1);
    voxels.push_back(voxel2);
    roi.updateBoundingRoi(voxel1);
    roi.updateBoundingRoi(voxel2);
}

void Edge::addVoxel(Voxel voxel) {
    voxels.push_back(voxel);
    roi.updateBoundingRoi(voxel);
}

void Edge::printToFile(int indentationLevel, std::ostream &outFile) {
    std::string indentationString;
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    outFile << indentationString << "connection: " << labelSmaller << " -> " << labelBigger << std::endl;
    outFile << indentationString << "\tid: " << edgeId << std::endl;
    outFile << indentationString << "\tnumber of voxels: " << voxels.size() << std::endl;
    outFile << indentationString << "\tShouldMerge: " << shouldMerge << std::endl;
    outFile << indentationString << "\tfx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << std::endl;
    outFile << indentationString << "\ttx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << std::endl;
    for (auto &feature : edgeFeatures) {
        outFile << "\t" << indentationString << feature->filterName << " " << feature->signalName << " ";
        for (auto &value : feature->values) {
            outFile << value << " ";
        }
        outFile << std::endl;
    }
    for (auto &feature : unionFeatures) {
        outFile << "\t" << indentationString << feature->filterName << " " << feature->signalName << " ";
        for (auto &value : feature->values) {
            outFile << value << " ";
        }
        outFile << std::endl;
    }
}

void Edge::print(int indentationLevel = 0) {
    std::string indentationString;
    for (int i = 0; i < indentationLevel; ++i) {
        indentationString += "\t";
    }
    std::cout << indentationString << "connection: " << labelSmaller << " -> " << labelBigger << std::endl;
    std::cout << indentationString << "\tid: " << edgeId << std::endl;
    std::cout << indentationString << "\tnumber of voxels: " << voxels.size() << std::endl;
    std::cout << indentationString << "\tShouldMerge: " << shouldMerge << std::endl;
    std::cout << indentationString << "\tfx: " << roi.minX << " fy: " << roi.minY << " fz: " << roi.minZ << std::endl;
    std::cout << indentationString << "\ttx: " << roi.maxX << " ty: " << roi.maxY << " tz: " << roi.maxZ << std::endl;
    for (auto &feature : edgeFeatures) {
        std::cout << "\t" << indentationString << feature->filterName << " " << feature->signalName << " ";
        for (auto &value : feature->values) {
            std::cout << value << " ";
        }
        std::cout << std::endl;
    }
    for (auto &feature : unionFeatures) {
        std::cout << "\t" << indentationString << feature->filterName << " " << feature->signalName << " ";
        for (auto &value : feature->values) {
            std::cout << value << " ";
        }
        std::cout << std::endl;
    }
}

std::vector<float> Edge::getAllEdgeFeaturesAsVector() {
    std::vector<float> allFeatures;

    for (auto &edgeFeature : edgeFeatures) {
        allFeatures.insert(allFeatures.end(), edgeFeature->values.begin(), edgeFeature->values.end());
    }

    return allFeatures;
}

std::vector<float> Edge::getAllUnionFeaturesAsVector() {
    std::vector<float> allFeatures;

    for (auto &unionFeature : unionFeatures) {
        allFeatures.insert(allFeatures.end(), unionFeature->values.begin(), unionFeature->values.end());
    }

    return allFeatures;
}

std::vector<float> Edge::getAllFeaturesAsVector() {
    std::vector<float> featureTmp;
    std::vector<float> edgeFeatureTmp;
    std::vector<float> unionFeatureTmp;
    std::vector<float> nodeAFeatureTmp;
    std::vector<float> nodeBFeatureTmp;

    // add EdgeFeatures
    edgeFeatureTmp = getAllEdgeFeaturesAsVector();
    featureTmp.insert(featureTmp.end(), edgeFeatureTmp.begin(), edgeFeatureTmp.end());

    // add union features
    unionFeatureTmp = getAllUnionFeaturesAsVector();
    featureTmp.insert(featureTmp.end(), unionFeatureTmp.begin(), unionFeatureTmp.end());

    // add NodeFeatures for NodeA
    nodeAFeatureTmp = pGraph->mergeTree[labelSmaller]->getAllNodeFeaturesAsVector();
    featureTmp.insert(featureTmp.end(), nodeAFeatureTmp.begin(), nodeAFeatureTmp.end());

    // add NodeFeatures for NodeB
    nodeBFeatureTmp = pGraph->mergeTree[labelBigger]->getAllNodeFeaturesAsVector();
    featureTmp.insert(featureTmp.end(), nodeBFeatureTmp.begin(), nodeBFeatureTmp.end());
    return featureTmp;
}



