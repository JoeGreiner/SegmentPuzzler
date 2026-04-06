#ifndef graphBase_h
#define graphBase_h

//#include <map>
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include "src/file_definitions/dataTypes.h"
#include <unordered_map>


// forward declaration
class Graph;

class GraphBase {
public:
    using SegmentsVoxelType = dataType::SegmentIdType;
    using SegmentsImageType = dataType::SegmentsImageType;
    using EdgeIdType = dataType::EdgePairIdType;
    using MappedEdgeIdType = dataType::MappedEdgeIdType;
    using EdgeImageType = dataType::EdgeImageType;
    using FeatureVoxelType = dataType::FeatureVoxelType;
    using FeatureImageType = dataType::FeatureImageType;

    using BoundaryVoxelType = dataType::BoundaryVoxelType;
    using BoundaryImageType = dataType::BoundaryImageType;


    std::map<std::string, FeatureImageType::Pointer> signalList;
    SegmentsImageType::Pointer pGroundTruth;
    std::vector<GraphBase::SegmentsVoxelType> ignoredSegmentLabels;
    // Non-owning — owned by MainWindow / MainWindowWatershedControl
    Graph *pGraph;

    SegmentsImageType::Pointer pWorkingSegmentsImage;
//    static SegmentsImageType::Pointer pInitialSegments;
    SegmentsImageType::Pointer pSelectedRefinement;
    // Non-owning — points into SignalControl::ownedSignals
    itkSignal<SegmentsVoxelType> *pSelectedRefinementSignal;
    // Non-owning — points into SignalControl::ownedSignals
    itkSignal<SegmentsVoxelType> *pWorkingSegments;


    SegmentsImageType::Pointer pSelectedSegmentation;
// keep track of max id of selected segmentation
    SegmentsVoxelType selectedSegmentationMaxSegmentId;
    // Non-owning — points into SignalControl::ownedSignals
    itkSignal<SegmentsVoxelType> *pSelectedSegmentationSignal;


    // Non-owning — owned by Graph (via Graph::ownedEdgesSignal)
    itkSignal<MappedEdgeIdType> *pEdgesInitialSegmentsITKSignal;

    EdgeImageType::Pointer pEdgesInitialSegmentsImage;
    size_t edgeCounter;
    std::unordered_map<char, std::vector<unsigned char>> colorLookUpEdgesStatus;

//  maps initial edges to their status: 0 = uninformed -2 == do not merge, 2 == do merge
    std::unordered_map<MappedEdgeIdType, char> edgeStatus;

    BoundaryImageType::Pointer pSelectedBoundary;

    bool ROI_set;
    int ROI_fx, ROI_fy, ROI_fz, ROI_tx, ROI_ty, ROI_tz;


    GraphBase() {
        edgeCounter = 0;
        pGraph = nullptr;
        pEdgesInitialSegmentsITKSignal = nullptr;
        pGroundTruth = nullptr;
        pWorkingSegmentsImage = nullptr;
        pSelectedRefinement = nullptr;
        pSelectedRefinementSignal = nullptr;
        pWorkingSegments = nullptr;
        pSelectedSegmentation = nullptr;
        pSelectedSegmentationSignal = nullptr;
        selectedSegmentationMaxSegmentId = 0;
        pSelectedBoundary = nullptr;
        ROI_fx = -1;
        ROI_fy = -1;
        ROI_fz = -1;
        ROI_tx = -1;
        ROI_ty = -1;
        ROI_tz = -1;
        ROI_set = false;
    }
};


#endif /* graphBase_h */
