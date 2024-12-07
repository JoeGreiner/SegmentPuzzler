#ifndef graphBase_h
#define graphBase_h

//#include <map>
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include "src/file_definitions/dataTypes.h"
#include <unordered_map>


// forward declaration
class Graph;
//template <typename dType> class itkSignal;
class SliceViewer;

class OrthoViewer;

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

    bool currentlyCalculating;


    std::map<std::string, FeatureImageType::Pointer> signalList;
    SegmentsImageType::Pointer pGroundTruth;
    std::vector<GraphBase::SegmentsVoxelType> ignoredSegmentLabels;
    Graph *pGraph;

    SegmentsImageType::Pointer pWorkingSegmentsImage;
//    static SegmentsImageType::Pointer pInitialSegments;
    SegmentsImageType::Pointer pRefinementWatershed;
    itkSignal<SegmentsVoxelType> *pRefinementWatershedSignal;
    itkSignal<SegmentsVoxelType> *pWorkingSegments;


    SegmentsImageType::Pointer pSelectedSegmentation;
// keep track of max id of selected segmentation
    SegmentsVoxelType selectedSegmentationMaxSegmentId;
    itkSignal<SegmentsVoxelType> *pSelectedSegmentationSignal;


    itkSignal<MappedEdgeIdType> *pEdgesInitialSegmentsITKSignal;
    std::vector<SliceViewer *> viewerList;
    OrthoViewer *pOrthoViewer;

    EdgeImageType::Pointer pEdgesInitialSegmentsImage;
    size_t edgeCounter;
    std::unordered_map<char, std::vector<unsigned char>> colorLookUpEdgesStatus;

//  maps initial edges to their status: 0 = uninformed -2 == do not merge, 2 == do merge
    std::unordered_map<MappedEdgeIdType, char> edgeStatus;

    // TODO: Handle that bullshit with different datatypes robustly
    BoundaryImageType::Pointer pSelectedBoundary;

    bool ROI_set;
    int ROI_fx, ROI_fy, ROI_fz, ROI_tx, ROI_ty, ROI_tz;


    GraphBase() {
        edgeCounter = 0;
        pGraph = nullptr;
        pOrthoViewer = nullptr;
        pGroundTruth = nullptr;
        pWorkingSegmentsImage = nullptr;
        pRefinementWatershed = nullptr;
        pRefinementWatershedSignal = nullptr;
        pWorkingSegments = nullptr;
        pSelectedSegmentation = nullptr;
        pSelectedSegmentationSignal = nullptr;
        selectedSegmentationMaxSegmentId = 254;
        pEdgesInitialSegmentsITKSignal = nullptr;
        pSelectedBoundary = nullptr;
        ROI_fx = -1;
        ROI_fy = -1;
        ROI_fz = -1;
        ROI_tx = -1;
        ROI_ty = -1;
        ROI_tz = -1;
        ROI_set = false;
        currentlyCalculating = false;
    }
};


#endif /* graphBase_h */
