#include <QCoreApplication>

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "src/segment_handling/Graph.h"
#include "src/segment_handling/graphBase.h"
#include "src/utils/ConnectedComponentLabelSplitter.h"

namespace {

using SegmentIdType = dataType::SegmentIdType;
using ImagePointer = dataType::SegmentsImageType::Pointer;
using segment_puzzler::connected_components::ConnectedComponentSplitOptions;
using segment_puzzler::connected_components::ConnectivityStencil;
using segment_puzzler::connected_components::splitDisconnectedLabelComponentsInPlace;

int failTest(const std::string &message) {
    std::cerr << "Assertion failed: " << message << "\n";
    return 1;
}

ImagePointer makeImage(unsigned int dimX, unsigned int dimY, unsigned int dimZ) {
    auto image = dataType::SegmentsImageType::New();
    dataType::SegmentsImageType::IndexType start{};
    start.Fill(0);
    dataType::SegmentsImageType::SizeType size{{dimX, dimY, dimZ}};
    dataType::SegmentsImageType::RegionType region(start, size);
    image->SetRegions(region);
    image->Allocate();
    image->FillBuffer(0);
    return image;
}

std::size_t countLabel(const ImagePointer &image, SegmentIdType label) {
    const auto total = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const SegmentIdType *buffer = image->GetBufferPointer();
    std::size_t count = 0;
    for (std::size_t index = 0; index < total; ++index) {
        count += buffer[index] == label ? 1U : 0U;
    }
    return count;
}

std::vector<SegmentIdType> copyImageBuffer(const ImagePointer &image) {
    const auto total = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const SegmentIdType *buffer = image->GetBufferPointer();
    return {buffer, buffer + total};
}

struct GraphFixture {
    std::shared_ptr<GraphBase> graphBase;
    std::unique_ptr<Graph> graph;
};

GraphFixture buildGraphFixture(const ImagePointer &image) {
    GraphFixture fixture;
    fixture.graphBase = std::make_shared<GraphBase>();
    fixture.graphBase->pWorkingSegmentsImage = image;
    fixture.graph = std::make_unique<Graph>(fixture.graphBase, false);
    fixture.graphBase->pGraph = fixture.graph.get();
    fixture.graph->setPointerToIgnoredSegmentLabels(&fixture.graphBase->ignoredSegmentLabels);
    fixture.graph->constructFromVolume(fixture.graphBase->pWorkingSegmentsImage);
    return fixture;
}

int testUtilityConnectivityModes() {
    auto sixImage = makeImage(2, 2, 1);
    sixImage->SetPixel({0, 0, 0}, 1);
    sixImage->SetPixel({1, 1, 0}, 1);

    ConnectedComponentSplitOptions sixOptions;
    sixOptions.connectivity = ConnectivityStencil::SixConnected;
    sixOptions.ignoredLabels.insert(0);
    sixOptions.nextFreeLabel = 2;
    const auto sixStats = splitDisconnectedLabelComponentsInPlace(sixImage, sixOptions);
    if (sixStats.labelsSplit != 1 || sixStats.componentsCreated != 1) {
        return failTest("Diagonal voxels should split with 6-connected connectivity.");
    }
    if (countLabel(sixImage, 1) != 1 || countLabel(sixImage, 2) != 1) {
        return failTest("6-connected split should keep one original label and add one fresh label.");
    }

    auto fullImage = makeImage(2, 2, 1);
    fullImage->SetPixel({0, 0, 0}, 1);
    fullImage->SetPixel({1, 1, 0}, 1);

    ConnectedComponentSplitOptions fullOptions;
    fullOptions.connectivity = ConnectivityStencil::Full;
    fullOptions.ignoredLabels.insert(0);
    fullOptions.nextFreeLabel = 2;
    const auto fullStats = splitDisconnectedLabelComponentsInPlace(fullImage, fullOptions);
    if (fullStats.changed()) {
        return failTest("Diagonal voxels should stay connected with full connectivity.");
    }
    if (countLabel(fullImage, 1) != 2) {
        return failTest("Full connectivity should leave the original label unchanged.");
    }

    return 0;
}

int testUtilityLargestComponentKeepsOriginalLabel() {
    auto image = makeImage(4, 1, 1);
    image->SetPixel({0, 0, 0}, 1);
    image->SetPixel({1, 0, 0}, 1);
    image->SetPixel({3, 0, 0}, 1);

    ConnectedComponentSplitOptions options;
    options.connectivity = ConnectivityStencil::SixConnected;
    options.ignoredLabels.insert(0);
    options.nextFreeLabel = 5;
    const auto stats = splitDisconnectedLabelComponentsInPlace(image, options);
    if (stats.labelsSplit != 1 || stats.componentsCreated != 1) {
        return failTest("One disconnected label should create one fresh component.");
    }
    if (countLabel(image, 1) != 2 || countLabel(image, 5) != 1) {
        return failTest("Largest component should keep the original label.");
    }
    if (image->GetPixel({0, 0, 0}) != 1 || image->GetPixel({1, 0, 0}) != 1 ||
        image->GetPixel({3, 0, 0}) != 5) {
        return failTest("Unexpected relabeling for largest-component preservation.");
    }
    return 0;
}

int testUtilityIgnoresBackground() {
    auto image = makeImage(3, 1, 1);
    image->SetPixel({0, 0, 0}, 0);
    image->SetPixel({2, 0, 0}, 0);

    ConnectedComponentSplitOptions options;
    options.connectivity = ConnectivityStencil::SixConnected;
    options.ignoredLabels.insert(0);
    options.nextFreeLabel = 1;
    const auto stats = splitDisconnectedLabelComponentsInPlace(image, options);
    if (stats.changed() || countLabel(image, 0) != 3) {
        return failTest("Ignored background labels should not be split.");
    }
    return 0;
}

int testGraphPreservesMergesAndSplitsWorkingOutput() {
    auto image = makeImage(5, 3, 1);
    image->SetPixel({1, 1, 0}, 1);
    image->SetPixel({4, 1, 0}, 1);
    image->SetPixel({2, 1, 0}, 2);

    auto fixture = buildGraphFixture(image);
    auto edgeIt = fixture.graph->initialTwoSidedEdges.find({1, 2});
    if (edgeIt == fixture.graph->initialTwoSidedEdges.end()) {
        return failTest("Expected an initial edge between labels 1 and 2.");
    }

    fixture.graph->mergeEdge(edgeIt->second.get());
    if (fixture.graph->workingNodes.size() != 1) {
        return failTest("Fixture merge should create one working node.");
    }

    const auto stats = fixture.graph->splitDisconnectedInitialSegments(ConnectivityStencil::SixConnected);
    if (stats.labelsSplit != 1 || stats.componentsCreated != 1) {
        return failTest("Disconnected initial label should split into one fresh initial node.");
    }
    if (fixture.graph->initialNodes.size() != 3) {
        return failTest("Graph should contain labels 1, 2, and the fresh split label.");
    }

    SegmentIdType freshInitialLabel = 0;
    for (const auto &initialEntry : fixture.graph->initialNodes) {
        if (initialEntry.first != 1 && initialEntry.first != 2) {
            freshInitialLabel = initialEntry.first;
        }
    }
    if (freshInitialLabel == 0) {
        return failTest("Fresh initial label was not found.");
    }

    if (fixture.graph->workingNodes.size() != 2) {
        return failTest("Disconnected preserved merge group should split into two working nodes.");
    }

    SegmentIdType mergedWorkingLabel = 0;
    SegmentIdType isolatedWorkingLabel = 0;
    for (const auto &workingEntry : fixture.graph->workingNodes) {
        const auto &subInitialNodes = workingEntry.second->subInitialNodes;
        if (subInitialNodes.count(1) > 0 && subInitialNodes.count(2) > 0) {
            mergedWorkingLabel = workingEntry.first;
        }
        if (subInitialNodes.size() == 1 && subInitialNodes.count(freshInitialLabel) > 0) {
            isolatedWorkingLabel = workingEntry.first;
        }
    }

    if (mergedWorkingLabel == 0 || isolatedWorkingLabel == 0 || mergedWorkingLabel == isolatedWorkingLabel) {
        return failTest("Working nodes should preserve the connected merge and isolate the split component.");
    }

    if (image->GetPixel({1, 1, 0}) != mergedWorkingLabel ||
        image->GetPixel({2, 1, 0}) != mergedWorkingLabel ||
        image->GetPixel({4, 1, 0}) != isolatedWorkingLabel) {
        return failTest("Working image labels do not match rebuilt working nodes.");
    }

    return 0;
}

int testEnsureReusesExactSelectedComponent() {
    auto workingImage = makeImage(4, 3, 1);
    workingImage->SetPixel({1, 1, 0}, 1);
    workingImage->SetPixel({2, 1, 0}, 1);

    auto selectedImage = makeImage(4, 3, 1);
    selectedImage->SetPixel({1, 1, 0}, 9);
    selectedImage->SetPixel({2, 1, 0}, 9);
    selectedImage->SetPixel({3, 2, 0}, 9); // Diagonal only: not part of the clicked 6-component.

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    const auto imageBefore = copyImageBuffer(workingImage);
    const auto nextFreeIdBefore = fixture.graph->nextFreeId;
    const auto initialNodeCountBefore = fixture.graph->initialNodes.size();
    const auto workingNodeCountBefore = fixture.graph->workingNodes.size();

    const auto result = fixture.graph->ensureSelectedSegmentationComponentInWorkingGraph(1, 1, 0);
    if (result.status != Graph::WorkingSegmentResolution::Status::ReusedExisting || result.workingLabel != 1) {
        return failTest("An exact selected component should reuse the existing working node despite different labels.");
    }
    if (fixture.graph->nextFreeId != nextFreeIdBefore ||
        fixture.graph->initialNodes.size() != initialNodeCountBefore ||
        fixture.graph->workingNodes.size() != workingNodeCountBefore ||
        copyImageBuffer(workingImage) != imageBefore) {
        return failTest("The exact-component fast path must not mutate the graph or working image.");
    }
    return 0;
}

int expectSelectedGeometryInsertion(const std::vector<std::array<int, 3>> &workingVoxels,
                                    const std::vector<std::array<int, 3>> &selectedVoxels,
                                    const std::string &scenario,
                                    bool verifyRefinementRestore = false,
                                    const std::vector<std::array<int, 3>> &disconnectedSelectedVoxels = {}) {
    auto workingImage = makeImage(6, 4, 1);
    for (const auto &voxel : workingVoxels) {
        workingImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 1);
    }

    auto selectedImage = makeImage(6, 4, 1);
    for (const auto &voxel : selectedVoxels) {
        selectedImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 9);
    }
    for (const auto &voxel : disconnectedSelectedVoxels) {
        selectedImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 9);
    }

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    const SegmentIdType expectedWorkingLabel = fixture.graph->nextFreeId;

    auto previousRefinement = makeImage(6, 4, 1);
    itkSignal<SegmentIdType> previousRefinementSignal(previousRefinement, false);
    if (verifyRefinementRestore) {
        fixture.graphBase->pSelectedRefinement = previousRefinement;
        fixture.graphBase->pSelectedRefinementSignal = &previousRefinementSignal;
    }

    const auto &seed = selectedVoxels.front();
    const auto result = fixture.graph->ensureSelectedSegmentationComponentInWorkingGraph(
        seed[0], seed[1], seed[2]);
    if (result.status != Graph::WorkingSegmentResolution::Status::Inserted ||
        result.workingLabel != expectedWorkingLabel) {
        return failTest(scenario + ": differing geometry should insert one working segment.");
    }
    if (fixture.graph->nextFreeId != expectedWorkingLabel + 1 ||
        fixture.graph->workingNodes.count(expectedWorkingLabel) != 1) {
        return failTest(scenario + ": insertion should create exactly the expected fresh working label.");
    }
    for (const auto &voxel : selectedVoxels) {
        if (workingImage->GetPixel({voxel[0], voxel[1], voxel[2]}) != expectedWorkingLabel) {
            return failTest(scenario + ": inserted component is missing from the working image.");
        }
    }
    for (const auto &voxel : disconnectedSelectedVoxels) {
        if (workingImage->GetPixel({voxel[0], voxel[1], voxel[2]}) == expectedWorkingLabel) {
            return failTest(scenario + ": a disconnected occurrence of the selected label was inserted.");
        }
    }
    if (verifyRefinementRestore &&
        (fixture.graphBase->pSelectedRefinement != previousRefinement ||
         fixture.graphBase->pSelectedRefinementSignal != &previousRefinementSignal)) {
        return failTest(scenario + ": temporary refinement pointers were not restored.");
    }
    return 0;
}

int testEnsureInsertsDifferentSelectedGeometry() {
    if (int result = expectSelectedGeometryInsertion(
            {{1, 1, 0}, {2, 1, 0}},
            {{1, 1, 0}, {1, 2, 0}},
            "equal-size shifted component",
            true,
            {{5, 3, 0}})) {
        return result;
    }
    if (int result = expectSelectedGeometryInsertion(
            {{1, 1, 0}, {2, 1, 0}, {3, 1, 0}},
            {{1, 1, 0}, {2, 1, 0}},
            "selected subset")) {
        return result;
    }
    if (int result = expectSelectedGeometryInsertion(
            {{1, 1, 0}, {2, 1, 0}},
            {{1, 1, 0}, {2, 1, 0}, {3, 1, 0}},
            "selected superset")) {
        return result;
    }
    if (int result = expectSelectedGeometryInsertion(
            {{5, 3, 0}},
            {{1, 1, 0}, {2, 1, 0}},
            "working-background click")) {
        return result;
    }
    if (int result = expectSelectedGeometryInsertion(
            {{1, 1, 0}, {5, 3, 0}},
            {{1, 1, 0}},
            "disconnected working node")) {
        return result;
    }
    return 0;
}

int testEnsureHandlesNoForegroundAndInconsistentWorkingNode() {
    auto workingImage = makeImage(3, 2, 1);
    workingImage->SetPixel({1, 1, 0}, 1);
    auto fixture = buildGraphFixture(workingImage);

    auto selectedBackground = makeImage(3, 2, 1);
    fixture.graphBase->pSelectedSegmentation = selectedBackground;
    const auto nextFreeIdBefore = fixture.graph->nextFreeId;
    auto result = fixture.graph->ensureSelectedSegmentationComponentInWorkingGraph(1, 1, 0);
    if (result.status != Graph::WorkingSegmentResolution::Status::NoForeground ||
        fixture.graph->nextFreeId != nextFreeIdBefore) {
        return failTest("A selected-background click should not mutate the graph.");
    }

    auto mismatchedSelected = makeImage(4, 2, 1);
    mismatchedSelected->SetPixel({1, 1, 0}, 7);
    fixture.graphBase->pSelectedSegmentation = mismatchedSelected;
    result = fixture.graph->ensureSelectedSegmentationComponentInWorkingGraph(1, 1, 0);
    if (result.status != Graph::WorkingSegmentResolution::Status::Failed ||
        fixture.graph->nextFreeId != nextFreeIdBefore) {
        return failTest("Mismatched selected and working regions should fail without insertion.");
    }

    auto selectedForeground = makeImage(3, 2, 1);
    selectedForeground->SetPixel({1, 1, 0}, 7);
    fixture.graphBase->pSelectedSegmentation = selectedForeground;
    fixture.graph->workingNodes.erase(1);
    result = fixture.graph->ensureSelectedSegmentationComponentInWorkingGraph(1, 1, 0);
    if (result.status != Graph::WorkingSegmentResolution::Status::Failed ||
        fixture.graph->nextFreeId != nextFreeIdBefore) {
        return failTest("A non-background working label without a WorkingNode should fail without insertion.");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    if (int result = testUtilityConnectivityModes()) {
        return result;
    }
    if (int result = testUtilityLargestComponentKeepsOriginalLabel()) {
        return result;
    }
    if (int result = testUtilityIgnoresBackground()) {
        return result;
    }
    if (int result = testGraphPreservesMergesAndSplitsWorkingOutput()) {
        return result;
    }
    if (int result = testEnsureReusesExactSelectedComponent()) {
        return result;
    }
    if (int result = testEnsureInsertsDifferentSelectedGeometry()) {
        return result;
    }
    if (int result = testEnsureHandlesNoForegroundAndInconsistentWorkingNode()) {
        return result;
    }

    std::cout << "Connected component split tests passed.\n";
    return 0;
}
