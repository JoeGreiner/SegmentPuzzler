#ifndef SEGMENTPUZZLER_WATERSHEDRAGAGGLOMERATION_H
#define SEGMENTPUZZLER_WATERSHEDRAGAGGLOMERATION_H

#include "src/file_definitions/dataTypes.h"

#include <itkImage.h>

#include <array>
#include <cstddef>

namespace segment_puzzler {

enum class RagLinkage {
    Average,
    Sum
};

enum class BoundaryNormalizationMode {
    AutoDetect,
    ProbabilityZeroToOne,
    ProbabilityZeroToTwo,
    UInt8FullRange,
    UInt16FullRange
};

enum class BoundaryEvidenceStrategy {
    RawInterfaceMean,
    OpenInterfaceMean,
    OpenFractionWeighted
};

enum class AgglomerationExecutionPolicy {
    Auto,
    Serial,
    OmpBatched
};

struct WatershedRagAgglomerationOptions {
    RagLinkage linkage = RagLinkage::Average;
    BoundaryNormalizationMode boundaryNormalization = BoundaryNormalizationMode::UInt16FullRange;
    BoundaryEvidenceStrategy boundaryEvidenceStrategy = BoundaryEvidenceStrategy::OpenFractionWeighted;
    AgglomerationExecutionPolicy executionPolicy = AgglomerationExecutionPolicy::Auto;
    double tau = 0.5;
    bool usePhysicalFaceArea = true;
    int threadCount = 0;
    std::size_t parallelMergeEdgeThreshold = 200000;
};

struct WatershedRagAgglomerationStats {
    std::size_t inputFragmentCount = 0;
    std::size_t ragEdgeCount = 0;
    std::size_t mergeCount = 0;
    std::size_t outputClusterCount = 0;
    std::size_t batchCount = 0;
    std::size_t maxBatchPairs = 0;
    double compactLabelsMs = 0.0;
    double ragBuildMs = 0.0;
    double heapInitMs = 0.0;
    double agglomerationMs = 0.0;
    double projectionMs = 0.0;
    double batchSelectionMs = 0.0;
    double batchReduceMs = 0.0;
    double batchApplyMs = 0.0;
    double boundaryMin = 0.0;
    double boundaryMax = 0.0;
    BoundaryNormalizationMode resolvedBoundaryNormalization = BoundaryNormalizationMode::AutoDetect;
    AgglomerationExecutionPolicy executionPolicyUsed = AgglomerationExecutionPolicy::Serial;
};

struct WatershedRagAgglomerationResult {
    dataType::SegmentsImageType::Pointer agglomeratedLabels;
    WatershedRagAgglomerationStats stats;
};

struct OrthoPlanePreviewSelection {
    std::array<int, 3> sliceIndices = {{-1, -1, -1}};
};

using BoundaryFloatImageType = itk::Image<float, 3>;
using BoundaryMaskImageType = itk::Image<unsigned char, 3>;

WatershedRagAgglomerationResult runWatershedRagAgglomeration(
    dataType::SegmentsImageType::Pointer labels,
    BoundaryFloatImageType::Pointer boundary,
    BoundaryMaskImageType::Pointer thresholdMask = nullptr,
    const WatershedRagAgglomerationOptions &options = {});

WatershedRagAgglomerationResult runWatershedRagAgglomerationPreview(
    dataType::SegmentsImageType::Pointer labels,
    BoundaryFloatImageType::Pointer boundary,
    const OrthoPlanePreviewSelection &previewSelection,
    BoundaryMaskImageType::Pointer thresholdMask = nullptr,
    const WatershedRagAgglomerationOptions &options = {});

const char *ragLinkageName(RagLinkage linkage);
const char *boundaryNormalizationModeName(BoundaryNormalizationMode mode);
const char *boundaryEvidenceStrategyName(BoundaryEvidenceStrategy strategy);
const char *agglomerationExecutionPolicyName(AgglomerationExecutionPolicy policy);

} // namespace segment_puzzler

#endif
