#ifndef edge_h
#define edge_h

#include "src/utils/voxel.h"
#include "feature.h"

#include <set>
#include "graphBase.h"
#include "src/utils/roi.h"
//#include "graph.h"


// the edge is defined as a one voxel thick interface
struct Edge : public FeatureList, GraphBase {
    Roi roi;

    unsigned int edgeId;

    unsigned int labelSmaller, labelBigger;

    // a edge has only to be computed once, as neither its voxel or its feature should change
    // this bool is a indicator if that computation is finished
    bool edgeComputed;

    int shouldMerge; // 0 = dont care (ecm, unset, ...) -1 == do not merge, 1 == do merge
    std::vector<std::unique_ptr<Feature>> edgeFeatures;
    std::vector<std::unique_ptr<Feature>> unionFeatures;

    // vector of all voxels in the edge
    std::vector<Voxel> voxels;

    // for debugging reasons, do not use it, will leak!
//    ~Edge();

    // constructor from two voxels
    Edge(Voxel voxel1, Voxel voxel2);

    // constructors from one voxel and two labels
    Edge(Voxel voxel, unsigned int labelA, unsigned int labelB, bool registerEdge = true);

    // constructor from voxel list and roi
    Edge(std::vector<Voxel> voxelList, Roi roiIn, unsigned int labelFrom, unsigned int labelTo);

    // add a feature to the edge features vector
    void addEdgeFeature(std::unique_ptr<Feature> &feature);

    // add a feature to the union features vector
    void addUnionFeature(std::unique_ptr<Feature> &feature);

    // recalculate all features stored for this node
    void calculateEdgeFeatures();

    // calculate all union features related to the edge
    void calculateUnionFeatures();

    // add two voxels to edge
    void addVoxel(Voxel voxel1, Voxel voxel2);

    // add one voxel to edge
    void addVoxel(Voxel voxel);

    // merge Edge with other edge
    void mergeVoxelsAndROIwithOtherEdge(std::shared_ptr<Edge> edgeToMerge);

    // print edge propoerties
    void print(int indentationLevel);

    void printToFile(int indentationLevel, std::ostream &outFile);

    // get edge feature by name
    std::vector<float> getEdgeFeatureByName(std::string filterName, std::string signalName);

    // get union feature by name
    std::vector<float> getUnionFeatureByName(std::string filterName, std::string signalName);


    // get all the edge features accociated with that edge
    std::vector<float> getAllEdgeFeaturesAsVector();

    // get all the union features assosciated with that edge
    std::vector<float> getAllUnionFeaturesAsVector();

    std::vector<float> getAllFeaturesAsVector();


};


#endif /* edge_h */
