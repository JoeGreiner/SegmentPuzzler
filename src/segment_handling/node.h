#ifndef node_h
#define node_h

#include "edge.h"
#include "src/utils/voxel.h"
#include "feature.h"
#include <map>


#include <algorithm>
#include "graphBase.h"

class Graph;

struct Node : FeatureList, GraphBase {
    // Region Of Interest of the node
    Roi roi;

    // Label of the node
    GraphBase::SegmentsVoxelType label;

    // was this node merged into a larger node?
    // note: this is a reference to the state of the node in the current segmentation!
    bool merged;

    // id that the merged node has
    GraphBase::SegmentsVoxelType mergedIntoId;

    // all the voxels in the node
    std::vector<Voxel> voxels;

    // all the voxels of the node that make up the surface
    std::vector<Voxel> surfaceVoxels;

    // pointer to the feature objects that are associated with that node
    std::vector<std::unique_ptr<Feature>> nodeFeatures;

    // map that assoicate the connections from this node to a given node id
    std::map<GraphBase::SegmentsVoxelType, std::shared_ptr<Edge>> edges;

    // set of ids, that describe this segment as union of __initial__ segments
    std::set<GraphBase::SegmentsVoxelType> initialSegments;

    // if the segment was created by merging two other segments, this tuple holds them
    std::tuple<GraphBase::SegmentsVoxelType, GraphBase::SegmentsVoxelType> originSegments;

    // check if a given label is considered ignored
    bool isAIgnoredSegmentLabel(GraphBase::SegmentsVoxelType label);

    //empty constructor
    Node();

    // constructor from voxel
    explicit Node(Voxel voxel);

    // add a feature to the node Features vector
    void addFeature(std::unique_ptr<Feature> &feature);

    // recalculate all nodefeatures stored for this node
    void calculateNodeFeatures();

    // recalculate all nodefeatures stored for this node
    void mergeNodeFeatures(std::vector<Voxel> &allVoxel, Node &A, Node &B);

    // recalculate all edge features stored for this node
    void calculateEdgeFeatures();

    // recalculate all union features stored for this node
    void calculateUnionFeatures();

    // remove connections to given node id
    void removeConnectionToId(GraphBase::SegmentsVoxelType idToErase, bool verbose = false);

    // add a voxel to the node
    void addVoxel(Voxel &voxel);

    // add a Edge to the node
    void addEdge(std::shared_ptr<Edge> edge);

    // define connectivity
    // this also adds edges
    void computeSurfaceVoxels();

    void computeSurfaceVoxelsParallel(std::map<GraphBase::SegmentsVoxelType, std::shared_ptr<Edge>> &tmpEdges,
                                      std::vector<Voxel> &tmpSurfaceVoxels);

    void subCalculateConnectivity(bool recalculateSurfaceVoxels,
                                  std::set<GraphBase::SegmentsVoxelType> &nodeIdsOfEdgesToCompute,
                                  const Voxel &voxel);

    std::vector<std::vector<Voxel> *> getVoxelList();


    // get a vector of the ids of the connected nodes. useful to iterate over elements if normal iterators get invalided
    std::vector<GraphBase::SegmentsVoxelType> getVectorOfConnectedNodeIds();

    // recompute a the edges on a given voxel list
    // attention: this does NOT clear previous edge entries on those voxels
    void recomputEdge(std::vector<Voxel> &voxelList);

    // recompute edge to one specific id
    // this just inserts further voxels, make sure it is not already processed
    void recomputEdge(GraphBase::SegmentsVoxelType connectedNodeId);

    // clears the edges container and recomputes all edges
    void recomputAllEdges();

    // returns a bool that indicates if this nodes is the result of a merge with node ids
    bool isMergedFrom(GraphBase::SegmentsVoxelType nodeIdA);

    bool isMergedFrom(GraphBase::SegmentsVoxelType nodeIdA, GraphBase::SegmentsVoxelType nodeIdB);

    // print node properties
    void print(int indentationLevel);

    // return the previously calculated ground truth label
    GraphBase::SegmentsVoxelType getGroundTruthLabel();

    // compute and set the shouldMerge flag on all edges of this node
    void setShouldMergeAttributeOnAllEdges();

    // get a node feature by its filter name
    std::vector<float> getNodeFeatureByName(std::string filterName, std::string signalName);

    // TODO also give a string vector with a feature description with the values
    // get all the NodeFeatures of the nodes concatenated into a single vector
    std::vector<float> getAllNodeFeaturesAsVector();

    void printToFile(int indentationLevel, std::ostream &outFile);

};


#endif /* node_h */
