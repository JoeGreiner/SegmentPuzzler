#include "src/image_storage/DenseImageStorage.h"
#include "src/image_storage/StreamVByteImageStorage.h"
#include <random>
#include <set>
#include <QCoreApplication>

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

#include "src/segment_handling/Graph.h"
#include "src/segment_handling/feature.h"
#include "src/segment_handling/graphBase.h"

namespace {

using SegmentIdType = dataType::SegmentIdType;
using EdgePairIdType = dataType::EdgePairIdType;
using ImagePointer = dataType::SegmentsImageType::Pointer;

int failTest(const std::string &message) {
    std::cerr << "Assertion failed: " << message << "\n";
    return 1;
}

ImagePointer makeTestImage() {
    auto image = dataType::SegmentsImageType::New();
    dataType::SegmentsImageType::IndexType start{};
    start.Fill(0);
    dataType::SegmentsImageType::SizeType size{{24, 18, 10}};
    dataType::SegmentsImageType::RegionType region(start, size);
    image->SetRegions(region);
    image->Allocate();

    for (int z = 0; z < 10; ++z) {
        for (int y = 0; y < 18; ++y) {
            for (int x = 0; x < 24; ++x) {
                const SegmentIdType tiledLabel =
                    1 + static_cast<SegmentIdType>(x / 4) +
                    6 * static_cast<SegmentIdType>(y / 6) +
                    18 * static_cast<SegmentIdType>(z / 5);
                image->SetPixel({x, y, z}, tiledLabel);
            }
        }
    }

    // One sparse label creates a large ROI and exercises dynamic scheduling.
    for (int z = 0; z < 10; ++z) {
        for (int y = 0; y < 18; ++y) {
            for (int x = 0; x < 24; ++x) {
                if ((x + 3 * y + 5 * z) % 37 == 0) {
                    image->SetPixel({x, y, z}, 40);
                }
            }
        }
    }
    image->SetPixel({0, 0, 0}, 0);
    return image;
}

template<typename Voxels>
void appendVoxels(std::ostringstream &out, const Voxels &input) {
    std::vector<Voxel> voxels(input.begin(), input.end());
    std::sort(voxels.begin(), voxels.end());
    for (const Voxel &voxel : voxels) {
        out << voxel.x << ',' << voxel.y << ',' << voxel.z << ';';
    }
}

void appendEdgeMetadata(std::ostringstream &out, const BaseEdge &edge) {
    const Roi &roi = edge.getRoi();
    out << edge.numId << '|'
        << roi.minX << ',' << roi.minY << ',' << roi.minZ << ','
        << roi.maxX << ',' << roi.maxY << ',' << roi.maxZ << '|';
}

void appendOneSidedInitialEdge(std::ostringstream &out, const OneSidedInitialEdge &edge) {
    appendEdgeMetadata(out, edge);
    appendVoxels(out, edge.voxels);
    out << '|';
}

void appendTwoSidedInitialEdge(std::ostringstream &out, const TwoSidedInitialEdge &edge) {
    appendEdgeMetadata(out, edge);
    std::vector<Voxel> logicalVoxels;
    logicalVoxels.reserve(edge.getVoxelCount());
    for (const VoxelList *voxelList : edge.getVoxelLists()) {
        logicalVoxels.insert(logicalVoxels.end(), voxelList->begin(), voxelList->end());
    }
    appendVoxels(out, logicalVoxels);
    out << '|';
    for (const auto &feature : edge.edgeFeatures) {
        out << feature->filterName << ':';
        for (float value : feature->values) {
            out << value << ',';
        }
        out << ';';
    }
}

std::string graphFingerprint(bool compressed = false) {
    auto graphBase = std::make_shared<GraphBase>();
    auto image = makeTestImage();
    if (compressed) graphBase->pWorkingSegmentsImage = segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image);
    else graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(image);
    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage);

    std::ostringstream out;
    std::vector<SegmentIdType> nodeLabels;
    nodeLabels.reserve(graph->initialNodes.size());
    for (const auto &entry : graph->initialNodes) {
        nodeLabels.push_back(entry.first);
    }
    std::sort(nodeLabels.begin(), nodeLabels.end());
    for (SegmentIdType label : nodeLabels) {
        const auto &node = graph->initialNodes.at(label);
        const Roi &roi = node->roi;
        out << "N" << label << ':'
            << roi.minX << ',' << roi.minY << ',' << roi.minZ << ','
            << roi.maxX << ',' << roi.maxY << ',' << roi.maxZ << '|';
        appendVoxels(out, node->voxels);
        out << '\n';
    }

    std::vector<std::pair<EdgePairIdType, std::shared_ptr<OneSidedInitialEdge>>> oneSidedEdges;
    for (const auto &[sourceLabel, node] : graph->initialNodes) {
        for (const auto &[neighborLabel, edge] : node->neighborLabelToOneSidedInitialEdge) {
            oneSidedEdges.push_back({{sourceLabel, neighborLabel}, edge});
        }
    }
    std::sort(oneSidedEdges.begin(), oneSidedEdges.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    for (const auto &entry : oneSidedEdges) {
        out << "O" << entry.first.first << ',' << entry.first.second << ':';
        appendOneSidedInitialEdge(out, *entry.second);
        out << '\n';
    }
    for (const auto &entry : graph->initialLabelPairToTwoSidedInitialEdge) {
        out << "T" << entry.first.first << ',' << entry.first.second << ':';
        appendTwoSidedInitialEdge(out, *entry.second);
        out << '\n';
    }

    std::vector<std::pair<GraphBase::MappedEdgeIdType, char>> edgeStatuses(
        graphBase->edgeStatus.begin(), graphBase->edgeStatus.end());
    std::sort(edgeStatuses.begin(), edgeStatuses.end());
    for (const auto &[edgeId, status] : edgeStatuses) {
        out << "S" << edgeId << ':' << static_cast<int>(status) << '\n';
    }

    out << "I";
    const auto edgeImageImage = graphBase->pEdgesInitialSegmentsImage->denseRegion(graphBase->pEdgesInitialSegmentsImage->GetLargestPossibleRegion());
    const auto *edgeImageBuffer = edgeImageImage->GetBufferPointer();
    const std::size_t edgeImageVoxelCount =
        graphBase->pEdgesInitialSegmentsImage->GetLargestPossibleRegion().GetNumberOfPixels();
    for (std::size_t voxelIndex = 0; voxelIndex < edgeImageVoxelCount; ++voxelIndex) {
        out << edgeImageBuffer[voxelIndex] << ',';
    }
    out << '\n';
    return out.str();
}

int testRunContacts() {
    std::mt19937 random(19);
    for (const auto size : {dataType::SegmentsImageType::SizeType{{1, 5, 3}}, {{6, 1, 3}}, {{4, 3, 1}}, {{11, 7, 19}}}) {
        auto image = dataType::SegmentsImageType::New();
        image->SetRegions(size); image->Allocate();
        for (std::size_t i = 0; i < image->GetBufferedRegion().GetNumberOfPixels(); ++i)
            image->GetBufferPointer()[i] = random() % 7;
        std::map<EdgePairIdType, std::vector<Voxel>> expected;
        for (int z = 0; z < int(size[2]); ++z) for (int y = 0; y < int(size[1]); ++y) for (int x = 0; x < int(size[0]); ++x) {
            const dataType::SegmentsImageType::IndexType index{x, y, z};
            const auto label = image->GetPixel(index);
            if (label == 0 || label == 2) continue;
            std::set<SegmentIdType> neighbors;
            for (unsigned axis = 0; axis < 3; ++axis) for (int offset : {-1, 1}) {
                auto other = index; other[axis] += offset;
                if (!image->GetBufferedRegion().IsInside(other)) continue;
                const auto neighbor = image->GetPixel(other);
                if (neighbor != label && neighbor != 0 && neighbor != 2) neighbors.insert(neighbor);
            }
            for (auto neighbor : neighbors) expected[{label, neighbor}].emplace_back(x, y, z);
        }
        for (bool compressed : {false, true}) {
            auto base = std::make_shared<GraphBase>();
            if (compressed) base->pWorkingSegmentsImage = segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image);
            else base->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(image);
            Graph graph(base, false); base->pGraph = &graph;
            base->ignoredSegmentLabels = {0, 2};
            graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
            graph.constructFromVolume(base->pWorkingSegmentsImage, 0);
            std::map<EdgePairIdType, std::vector<Voxel>> actual;
            for (const auto &[label, node] : graph.initialNodes)
                for (const auto &[neighbor, edge] : node->neighborLabelToOneSidedInitialEdge)
                    actual[{label, neighbor}] = std::vector<Voxel>(edge->voxels.begin(), edge->voxels.end());
            if (actual != expected) return failTest("Run contacts differ from six-neighbor voxel reference");
        }
    }
    return 0;
}

int testBatchedEdgeImage() {
    auto image = dataType::SegmentsImageType::New();
    image->SetRegions(dataType::SegmentsImageType::SizeType{{24, 18, 35}});
    image->Allocate(true);
    for (int z = 16; z < 35; ++z) for (int y = 3; y < 15; ++y) for (int x = 4; x < 20; ++x)
        if (z != 20) image->SetPixel({x, y, z}, x < 12 ? 1 : 2);
    auto base = std::make_shared<GraphBase>();
    base->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(image);
    Graph graph(base, false);
    base->pGraph = &graph;
    graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
    graph.constructFromVolume(image);
    if (graph.initialLabelPairToTwoSidedInitialEdge.size() != 1)
        return failTest("Batched edge fixture must contain one edge");
    const auto edge = graph.initialLabelPairToTwoSidedInitialEdge.begin()->second;
    std::vector<dataType::MappedEdgeIdType> expected(image->GetBufferedRegion().GetNumberOfPixels(), 0);
    for (const auto *list : edge->getVoxelLists()) for (const auto voxel : *list)
        expected[image->ComputeOffset({voxel.x, voxel.y, voxel.z})] = edge->numId;
    const auto actual = base->pEdgesInitialSegmentsImage->denseRegion(image->GetBufferedRegion());
    return std::equal(expected.begin(), expected.end(), actual->GetBufferPointer())
        ? 0 : failTest("Batched edge image differs across empty planes or the final partial batch");
}

int testRepeatableInitialEdgeBuild() {
    const std::string reference = graphFingerprint();
    if (graphFingerprint(true) != reference) return failTest("Compressed graph differs from dense graph");
    for (int repetition = 0; repetition < 5; ++repetition) {
        if (graphFingerprint() != reference) {
            return failTest("Repeated graph construction differs from the first result.");
        }
    }
    return 0;
}

int testTwoSidedInitialEdgeReferencesBothOneSidedInitialEdges() {
    auto graphBase = std::make_shared<GraphBase>();
    graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(makeTestImage());
    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));

    std::size_t oneSidedEdgeReferenceCount = 0;
    for (const auto &[labelPair, edge] : graph->initialLabelPairToTwoSidedInitialEdge) {
        const auto &smallerLabelEdge =
            graph->initialNodes.at(labelPair.first)->neighborLabelToOneSidedInitialEdge.at(labelPair.second);
        const auto &largerLabelEdge =
            graph->initialNodes.at(labelPair.second)->neighborLabelToOneSidedInitialEdge.at(labelPair.first);
        if (edge->getSmallerLabelOneSidedInitialEdge() != smallerLabelEdge ||
            edge->getLargerLabelOneSidedInitialEdge() != largerLabelEdge) {
            return failTest("A two-sided initial edge does not reference its registered one-sided edges.");
        }
        oneSidedEdgeReferenceCount += 2;
    }
    if (oneSidedEdgeReferenceCount != 2 * graph->initialLabelPairToTwoSidedInitialEdge.size()) {
        return failTest("Two-sided initial-edge reference count is not exactly two per edge.");
    }
    return 0;
}

int testTwoSidedInitialEdgePreservesVoxelOrderAndCount() {
    auto graphBase = std::make_shared<GraphBase>();
    graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(makeTestImage());
    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));

    for (const auto &[labelPair, edge] : graph->initialLabelPairToTwoSidedInitialEdge) {
        const VoxelLists voxelLists = edge->getVoxelLists();
        const auto &smallerLabelVoxels = edge->getSmallerLabelOneSidedInitialEdge()->voxels;
        const auto &largerLabelVoxels = edge->getLargerLabelOneSidedInitialEdge()->voxels;
        if (voxelLists.size() != 2 || voxelLists[0] != &smallerLabelVoxels ||
            voxelLists[1] != &largerLabelVoxels) {
            return failTest("Two-sided initial-edge voxel views are not in label order.");
        }
        if (edge->getVoxelCount() != smallerLabelVoxels.size() + largerLabelVoxels.size()) {
            return failTest("Two-sided initial-edge voxel count differs from both one-sided lists.");
        }
    }
    return 0;
}

int testTwoSidedInitialEdgeFeaturesUseBothOneSidedInitialEdges() {
    FeatureList::edgeFeaturesList.emplace_back(std::make_unique<NumberOfVoxels>());
    auto graphBase = std::make_shared<GraphBase>();
    graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(makeTestImage());
    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));

    bool featuresAreValid = true;
    for (const auto &[labelPair, edge] : graph->initialLabelPairToTwoSidedInitialEdge) {
        if (edge->edgeFeatures.size() != 1 || edge->edgeFeatures.front()->values.size() != 1 ||
            edge->edgeFeatures.front()->values.front() != static_cast<float>(edge->getVoxelCount())) {
            featuresAreValid = false;
            break;
        }
    }
    FeatureList::edgeFeaturesList.clear();
    if (!featuresAreValid) {
        return failTest("Two-sided initial-edge features did not use both one-sided voxel lists.");
    }
    return 0;
}

int testRebuiltTwoSidedInitialEdgeReferencesCurrentOneSidedInitialEdges() {
    auto graphBase = std::make_shared<GraphBase>();
    graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(makeTestImage());
    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));

    if (graph->initialLabelPairToTwoSidedInitialEdge.empty()) {
        return failTest("The rebuild fixture contains no two-sided initial edge.");
    }
    const EdgePairIdType labelPair = graph->initialLabelPairToTwoSidedInitialEdge.begin()->first;
    const auto previousEdge = graph->initialLabelPairToTwoSidedInitialEdge.begin()->second;
    const auto previousSmallerLabelEdge = previousEdge->getSmallerLabelOneSidedInitialEdge();
    const auto previousLargerLabelEdge = previousEdge->getLargerLabelOneSidedInitialEdge();

    graph->constructFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));
    const auto &rebuiltEdge = graph->initialLabelPairToTwoSidedInitialEdge.at(labelPair);
    const auto &currentSmallerLabelEdge =
        graph->initialNodes.at(labelPair.first)->neighborLabelToOneSidedInitialEdge.at(labelPair.second);
    const auto &currentLargerLabelEdge =
        graph->initialNodes.at(labelPair.second)->neighborLabelToOneSidedInitialEdge.at(labelPair.first);
    if (rebuiltEdge->getSmallerLabelOneSidedInitialEdge() != currentSmallerLabelEdge ||
        rebuiltEdge->getLargerLabelOneSidedInitialEdge() != currentLargerLabelEdge ||
        currentSmallerLabelEdge == previousSmallerLabelEdge ||
        currentLargerLabelEdge == previousLargerLabelEdge) {
        return failTest("A rebuilt two-sided initial edge does not reference the current one-sided edges.");
    }
    return 0;
}

int testRepeatableFeatureEnabledBuild() {
    FeatureList::edgeFeaturesList.emplace_back(std::make_unique<NumberOfVoxels>());
    const std::string reference = graphFingerprint();
    const std::string repeated = graphFingerprint();
    FeatureList::edgeFeaturesList.clear();
    if (repeated != reference) {
        return failTest("Repeated edge-feature fallback differs from the first result.");
    }
    return 0;
}

int testEmptyNodeProducesNoEdges() {
    auto graphBase = std::make_shared<GraphBase>();
    InitialNode node(graphBase, segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(makeTestImage()), 41);
    node.computeOneSidedEdges({0});
    if (!node.neighborLabelToOneSidedInitialEdge.empty()) {
        return failTest("An empty initial node should not produce one-sided edges.");
    }
    return 0;
}

int testIgnoredLabelsAreNotMaterialized() {
    constexpr SegmentIdType additionallyIgnoredLabel = 40;
    constexpr auto highestLabel = std::numeric_limits<SegmentIdType>::max();
    auto graphBase = std::make_shared<GraphBase>();
    auto image = makeTestImage();
    image->SetPixel({1, 0, 0}, highestLabel);
    graphBase->pWorkingSegmentsImage = segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(image);
    graphBase->ignoredSegmentLabels = {additionallyIgnoredLabel, highestLabel};

    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
#ifdef USE_OMP
    const int defaultThreadCount = omp_get_max_threads();
#endif
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));
#ifdef USE_OMP
    if (omp_get_max_threads() != defaultThreadCount) {
        return failTest("Graph construction changed the caller's OpenMP default.");
    }
#endif
    if (graph->backgroundId != 0 || graph->nextFreeId != 37 ||
        graph->getLargestIdInSegmentVolume(graphBase->pWorkingSegmentsImage) != highestLabel) {
        return failTest("Graph label extrema or next free id are incorrect.");
    }

    for (auto ignored : graphBase->ignoredSegmentLabels) {
        if (graph->initialNodes.count(ignored) || graph->workingNodes.count(ignored)) {
            return failTest("Ignored labels were materialized as graph nodes.");
        }
    }

    std::size_t expectedVoxelCount = 0;
    std::vector<std::size_t> expectedCountByLabel(41, 0);
    const auto segmentLabelImage = graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion());
    const auto *segmentLabelBuffer = segmentLabelImage->GetBufferPointer();
    const std::size_t imageVoxelCount =
        graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion().GetNumberOfPixels();
    for (std::size_t voxelIndex = 0; voxelIndex < imageVoxelCount; ++voxelIndex) {
        const SegmentIdType label = segmentLabelBuffer[voxelIndex];
        if (label != 0 && label != additionallyIgnoredLabel && label != highestLabel) {
            ++expectedCountByLabel.at(label);
            ++expectedVoxelCount;
        }
    }

    std::size_t materializedVoxelCount = 0;
    for (const auto &[label, initialNode] : graph->initialNodes) {
        if (initialNode->voxels.size() != expectedCountByLabel.at(label)) {
            return failTest("Initial-node voxel count differs from the reference histogram.");
        }
        materializedVoxelCount += initialNode->voxels.size();
    }
    for (SegmentIdType label = 37; label < 40; ++label) {
        if (graph->initialNodes.count(label) != 0) {
            return failTest("A missing label was materialized as an initial node.");
        }
    }
    if (materializedVoxelCount != expectedVoxelCount) {
        return failTest("Initial nodes do not contain exactly the non-ignored voxels.");
    }
    graph->setBackgroundIdStrategy("backgroundIsHighestId");
    graph->updateBackgroundIdFromVolume(graphBase->pWorkingSegmentsImage->denseRegion(graphBase->pWorkingSegmentsImage->GetLargestPossibleRegion()));
    if (graph->backgroundId != highestLabel) {
        return failTest("Highest-label background selection is incorrect.");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    if (!FeatureList::edgeFeaturesList.empty()) {
        return failTest("Edge feature registry must be empty when the test starts.");
    }
    if (int result = testTwoSidedInitialEdgeReferencesBothOneSidedInitialEdges()) {
        return result;
    }
    if (int result = testTwoSidedInitialEdgePreservesVoxelOrderAndCount()) {
        return result;
    }
    if (int result = testTwoSidedInitialEdgeFeaturesUseBothOneSidedInitialEdges()) {
        return result;
    }
    if (int result = testRunContacts()) return result;
    if (int result = testBatchedEdgeImage()) return result;
    if (int result = testRepeatableInitialEdgeBuild()) {
        return result;
    }
    if (int result = testRebuiltTwoSidedInitialEdgeReferencesCurrentOneSidedInitialEdges()) {
        return result;
    }
    if (int result = testRepeatableFeatureEnabledBuild()) {
        return result;
    }
    if (int result = testEmptyNodeProducesNoEdges()) {
        return result;
    }
    if (int result = testIgnoredLabelsAreNotMaterialized()) {
        return result;
    }

    if (argc == 2 && std::string(argv[1]) == "--fingerprint") {
        std::cout << graphFingerprint();
        FeatureList::edgeFeaturesList.emplace_back(std::make_unique<NumberOfVoxels>());
        std::cout << graphFingerprint();
        FeatureList::edgeFeaturesList.clear();
    } else {
        std::cout << "Initial-edge result-equivalence tests passed.\n";
    }
    return 0;
}
