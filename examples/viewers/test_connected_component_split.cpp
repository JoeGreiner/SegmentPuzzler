#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/segment_handling/Graph.h"
#include "src/segment_handling/SeededWatershedSplit.h"
#include "src/segment_handling/graphBase.h"
#include "src/utils/ConnectedComponentLabelSplitter.h"

namespace {

using SegmentIdType = dataType::SegmentIdType;
using ImagePointer = dataType::SegmentsImageType::Pointer;
using segment_puzzler::connected_components::ConnectedComponentSplitOptions;
using segment_puzzler::connected_components::ConnectivityStencil;
using segment_puzzler::connected_components::countConnectedComponentsByLabel;
using segment_puzzler::connected_components::countConnectedComponentsByLabelInRegions;
using segment_puzzler::connected_components::splitDisconnectedLabelComponentsInPlace;
using segment_puzzler::connected_components::visitLabelComponent;

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

    const std::unordered_set<SegmentIdType> labelsToCount{1};
    if (countConnectedComponentsByLabel(sixImage, labelsToCount, ConnectivityStencil::SixConnected).at(1) != 2 ||
        countConnectedComponentsByLabel(sixImage, labelsToCount, ConnectivityStencil::Full).at(1) != 1) {
        return failTest("Connected-component counting should use the requested connectivity stencil.");
    }
    const std::unordered_map<SegmentIdType, dataType::SegmentsImageType::RegionType> regionsByLabel{
        {1, sixImage->GetLargestPossibleRegion()}};
    if (countConnectedComponentsByLabelInRegions(
            sixImage, regionsByLabel, ConnectivityStencil::SixConnected).at(1) != 2 ||
        countConnectedComponentsByLabelInRegions(
            sixImage, regionsByLabel, ConnectivityStencil::Full).at(1) != 1) {
        return failTest("ROI connected-component counting should preserve connectivity semantics.");
    }

    auto offsetImage = dataType::SegmentsImageType::New();
    dataType::SegmentsImageType::IndexType offsetStart{{5, -2, 3}};
    dataType::SegmentsImageType::SizeType offsetSize{{2, 2, 1}};
    dataType::SegmentsImageType::RegionType offsetRegion(offsetStart, offsetSize);
    offsetImage->SetRegions(offsetRegion);
    offsetImage->Allocate();
    offsetImage->FillBuffer(0);
    offsetImage->SetPixel({5, -2, 3}, 7);
    offsetImage->SetPixel({6, -1, 3}, 7);
    const std::unordered_map<SegmentIdType, dataType::SegmentsImageType::RegionType> offsetRegions{
        {7, offsetRegion}};
    if (countConnectedComponentsByLabelInRegions(
            offsetImage, offsetRegions, ConnectivityStencil::SixConnected).at(7) != 2) {
        return failTest("ROI connectivity should support non-zero image indices.");
    }

    auto multiRegionImage = makeImage(4, 2, 1);
    multiRegionImage->SetPixel({0, 0, 0}, 1);
    multiRegionImage->SetPixel({0, 1, 0}, 1);
    multiRegionImage->SetPixel({2, 0, 0}, 2);
    multiRegionImage->SetPixel({3, 1, 0}, 2);
    const std::unordered_map<SegmentIdType, dataType::SegmentsImageType::RegionType> multiRegions{
        {1, multiRegionImage->GetLargestPossibleRegion()},
        {2, multiRegionImage->GetLargestPossibleRegion()}};
    const auto multiCounts = countConnectedComponentsByLabelInRegions(
        multiRegionImage, multiRegions, ConnectivityStencil::SixConnected);
    if (multiCounts.at(1) != 1 || multiCounts.at(2) != 2) {
        return failTest("ROI connectivity should count multiple labels independently.");
    }

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

int testSeededComponentTraversal() {
    auto image = makeImage(3, 3, 1);
    image->SetPixel({0, 0, 0}, 7);
    image->SetPixel({1, 0, 0}, 7);
    image->SetPixel({2, 1, 0}, 7); // Diagonal to the first component only.

    std::set<std::array<long, 3>> sixConnectedVoxels;
    const auto sixResult = visitLabelComponent(
        image,
        dataType::SegmentsImageType::IndexType{{0, 0, 0}},
        ConnectivityStencil::SixConnected,
        [&](const dataType::SegmentsImageType::IndexType &index) {
            sixConnectedVoxels.insert({index[0], index[1], index[2]});
            return true;
        });
    if (!sixResult.completed || sixResult.voxelCount != 2 ||
        sixConnectedVoxels != std::set<std::array<long, 3>>{{{0, 0, 0}}, {{1, 0, 0}}}) {
        return failTest("Seed traversal should visit only the selected 6-connected component.");
    }

    const auto fullResult = visitLabelComponent(
        image,
        dataType::SegmentsImageType::IndexType{{0, 0, 0}},
        ConnectivityStencil::Full,
        [](const dataType::SegmentsImageType::IndexType &) { return true; });
    if (!fullResult.completed || fullResult.voxelCount != 3) {
        return failTest("Full-connectivity seed traversal should include diagonal voxels.");
    }

    const auto stoppedResult = visitLabelComponent(
        image,
        dataType::SegmentsImageType::IndexType{{0, 0, 0}},
        ConnectivityStencil::SixConnected,
        [](const dataType::SegmentsImageType::IndexType &) { return false; });
    if (stoppedResult.completed || stoppedResult.voxelCount != 1) {
        return failTest("Seed traversal should report an early visitor stop.");
    }

    auto offsetImage = dataType::SegmentsImageType::New();
    dataType::SegmentsImageType::IndexType offsetStart{{5, -2, 3}};
    dataType::SegmentsImageType::SizeType offsetSize{{2, 1, 1}};
    dataType::SegmentsImageType::RegionType offsetRegion(offsetStart, offsetSize);
    offsetImage->SetRegions(offsetRegion);
    offsetImage->Allocate();
    offsetImage->FillBuffer(4);
    std::set<std::array<long, 3>> offsetVoxels;
    const auto offsetResult = visitLabelComponent(
        offsetImage,
        offsetStart,
        ConnectivityStencil::SixConnected,
        [&](const dataType::SegmentsImageType::IndexType &index) {
            offsetVoxels.insert({index[0], index[1], index[2]});
            return true;
        });
    if (!offsetResult.completed || offsetResult.voxelCount != 2 ||
        offsetVoxels.count({6, -2, 3}) != 1) {
        return failTest("Seed traversal should preserve non-zero ITK image indices.");
    }

    bool nullImageRejected = false;
    try {
        ImagePointer nullImage;
        static_cast<void>(visitLabelComponent(
            nullImage,
            dataType::SegmentsImageType::IndexType{{0, 0, 0}},
            ConnectivityStencil::SixConnected,
            [](const dataType::SegmentsImageType::IndexType &) { return true; }));
    } catch (const std::invalid_argument &) {
        nullImageRejected = true;
    }
    bool invalidSeedRejected = false;
    try {
        static_cast<void>(visitLabelComponent(
            image,
            dataType::SegmentsImageType::IndexType{{3, 0, 0}},
            ConnectivityStencil::SixConnected,
            [](const dataType::SegmentsImageType::IndexType &) { return true; }));
    } catch (const std::invalid_argument &) {
        invalidSeedRejected = true;
    }
    if (!nullImageRejected || !invalidSeedRejected) {
        return failTest("Seed traversal should reject null images and out-of-region seeds.");
    }

    bool oversizedGeometryRejected = false;
    try {
        dataType::SegmentsImageType::IndexType start{};
        dataType::SegmentsImageType::SizeType oversized{{
            std::numeric_limits<dataType::SegmentsImageType::SizeType::SizeValueType>::max(),
            2,
            1}};
        static_cast<void>(segment_puzzler::connected_components::detail::geometryForRegion(
            dataType::SegmentsImageType::RegionType(start, oversized)));
    } catch (const std::overflow_error &) {
        oversizedGeometryRejected = true;
    }
    if (!oversizedGeometryRejected) {
        return failTest("Component geometry should reject overflowing image dimensions.");
    }

    return 0;
}

int testFindPresentLabelsUsesCandidateSet() {
    auto image = makeImage(5, 1, 1);
    image->SetPixel({1, 0, 0}, 3);
    image->SetPixel({4, 0, 0}, 7);
    const auto present = utils::findPresentLabels(image, {2, 3, 7, 9});
    if (present != std::unordered_set<SegmentIdType>{3, 7}) {
        return failTest("Batched label presence should return only candidates found in the image.");
    }
    ImagePointer nullImage;
    if (!utils::findPresentLabels(nullImage, {}).empty()) {
        return failTest("An empty label candidate set should not require an image.");
    }
    bool nullImageRejected = false;
    try {
        static_cast<void>(utils::findPresentLabels(nullImage, {3}));
    } catch (const std::invalid_argument &) {
        nullImageRejected = true;
    }
    if (!nullImageRejected) {
        return failTest("Batched label presence should reject a null image for non-empty candidates.");
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

int testUtilityLimitsSplitToIncludedLabels() {
    auto image = makeImage(6, 2, 1);
    image->SetPixel({0, 0, 0}, 1);
    image->SetPixel({1, 0, 0}, 1);
    image->SetPixel({3, 0, 0}, 1);
    image->SetPixel({0, 1, 0}, 2);
    image->SetPixel({3, 1, 0}, 2);

    ConnectedComponentSplitOptions options;
    options.connectivity = ConnectivityStencil::SixConnected;
    options.includedLabels.insert(1);
    options.ignoredLabels.insert(0);
    options.nextFreeLabel = 3;
    const auto stats = splitDisconnectedLabelComponentsInPlace(image, options);

    if (stats.labelsVisited != 1 || stats.labelsSplit != 1
        || stats.componentsCreated != 1
        || stats.finalLabelsByOriginalLabel.at(1)
               != std::vector<SegmentIdType>{1, 3}) {
        return failTest("Targeted splitting should process only the included label.");
    }
    if (countLabel(image, 1) != 2 || countLabel(image, 3) != 1
        || countLabel(image, 2) != 2) {
        return failTest("Targeted splitting changed an unrelated disconnected label.");
    }
    return 0;
}

int testUtilityPreflightsLabelExhaustion() {
    const SegmentIdType maximumLabel = std::numeric_limits<SegmentIdType>::max();
    auto image = makeImage(7, 1, 1);
    image->SetPixel({0, 0, 0}, 1);
    image->SetPixel({2, 0, 0}, 1);
    image->SetPixel({4, 0, 0}, 1);
    image->SetPixel({6, 0, 0}, maximumLabel - 2);
    const auto before = copyImageBuffer(image);

    ConnectedComponentSplitOptions options;
    options.connectivity = ConnectivityStencil::SixConnected;
    options.includedLabels.insert(1);
    options.ignoredLabels.insert(0);
    options.nextFreeLabel = maximumLabel - 1;
    bool threw = false;
    try {
        static_cast<void>(splitDisconnectedLabelComponentsInPlace(image, options));
    } catch (const std::overflow_error &) {
        threw = true;
    }
    if (!threw || copyImageBuffer(image) != before) {
        return failTest("Label exhaustion must not partially relabel the image.");
    }
    return 0;
}

int testGraphPreservesMergesAndSplitsWorkingOutput() {
    auto image = makeImage(5, 3, 1);
    image->SetPixel({1, 1, 0}, 1);
    image->SetPixel({4, 1, 0}, 1);
    image->SetPixel({2, 1, 0}, 2);

    auto fixture = buildGraphFixture(image);
    auto edgeIt = fixture.graph->initialLabelPairToTwoSidedInitialEdge.find({1, 2});
    if (edgeIt == fixture.graph->initialLabelPairToTwoSidedInitialEdge.end()) {
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

int testRefinementSynchronizesOverwrittenNodeMetadata() {
    {
        auto workingImage = makeImage(8, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        workingImage->SetPixel({5, 0, 0}, 1);
        workingImage->SetPixel({2, 0, 0}, 2);
        workingImage->SetPixel({6, 0, 0}, 3);
        auto selectedImage = makeImage(8, 1, 1);
        selectedImage->SetPixel({1, 0, 0}, 9);

        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        const SegmentIdType insertedLabel = fixture.graph->nextFreeId;
        const auto inserted =
            fixture.graph->transferSegmentationSegmentToInitialSegment(1, 0, 0);
        const std::vector<Voxel> expectedSurvivingVoxels{Voxel(5, 0, 0)};
        if (inserted != insertedLabel || workingImage->GetPixel({5, 0, 0}) != 1 ||
            fixture.graph->initialNodes.count(1) != 1 ||
            fixture.graph->workingNodes.count(1) != 1 ||
            fixture.graph->initialNodes.at(1)->voxels != expectedSurvivingVoxels ||
            fixture.graph->initialNodes.at(1)->roi.minX != 5 ||
            fixture.graph->initialNodes.at(1)->roi.maxX != 5 ||
            fixture.graph->workingNodes.at(1)->roi.minX != 5 ||
            fixture.graph->workingNodes.at(1)->roi.maxX != 5) {
            return failTest(
                "A surviving overwritten node must refresh its voxels and ROIs exactly.");
        }

        const Graph::EdgePairIdType replacedLocalEdge{1, 2};
        const Graph::EdgePairIdType survivingRemoteEdge{1, 3};
        const Graph::EdgePairIdType newLocalEdge{2, insertedLabel};
        const auto survivingOneSidedEdge =
            fixture.graph->initialNodes.at(1)->neighborLabelToOneSidedInitialEdge.find(3);
        const auto newOneSidedEdge =
            fixture.graph->initialNodes.at(insertedLabel)->neighborLabelToOneSidedInitialEdge.find(2);
        if (fixture.graph->initialLabelPairToTwoSidedInitialEdge.count(replacedLocalEdge) != 0 ||
            fixture.graph->initialLabelPairToTwoSidedInitialEdge.count(survivingRemoteEdge) != 1 ||
            fixture.graph->initialLabelPairToTwoSidedInitialEdge.count(newLocalEdge) != 1 ||
            fixture.graph->workingLabelPairToWorkingEdge.count(replacedLocalEdge) != 0 ||
            fixture.graph->workingLabelPairToWorkingEdge.count(survivingRemoteEdge) != 1 ||
            fixture.graph->workingLabelPairToWorkingEdge.count(newLocalEdge) != 1 ||
            fixture.graph->initialNodes.at(1)->neighborLabelToOneSidedInitialEdge.size() != 1 ||
            survivingOneSidedEdge ==
                fixture.graph->initialNodes.at(1)->neighborLabelToOneSidedInitialEdge.end() ||
            survivingOneSidedEdge->second->voxels != std::vector<Voxel>{Voxel(5, 0, 0)} ||
            newOneSidedEdge ==
                fixture.graph->initialNodes.at(insertedLabel)->neighborLabelToOneSidedInitialEdge.end() ||
            newOneSidedEdge->second->voxels != std::vector<Voxel>{Voxel(1, 0, 0)} ||
            fixture.graph->initialLabelPairToTwoSidedInitialEdge.at(survivingRemoteEdge)->getVoxelCount() != 2 ||
            fixture.graph->initialLabelPairToTwoSidedInitialEdge.at(newLocalEdge)->getVoxelCount() != 2) {
            return failTest(
                "Refinement must remove the replaced edge and preserve remote and new edges.");
        }
    }

    {
        auto workingImage = makeImage(4, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        auto selectedImage = makeImage(4, 1, 1);
        selectedImage->SetPixel({1, 0, 0}, 9);

        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        const auto inserted =
            fixture.graph->transferSegmentationSegmentToInitialSegment(1, 0, 0);
        if (!inserted.has_value() || fixture.graph->initialNodes.count(1) != 0 ||
            fixture.graph->workingNodes.count(1) != 0 || countLabel(workingImage, 1) != 0) {
            return failTest("A fully replaced label should be removed after the authoritative scan.");
        }
    }
    return 0;
}

int testRefinementPrevalidationAndRoiStayMutationFree() {
    {
        auto workingImage = makeImage(4, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        auto selectedImage = makeImage(4, 1, 1);
        selectedImage->SetPixel({1, 0, 0}, 9);
        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        fixture.graph->workingNodes.at(1).reset();
        const auto before = copyImageBuffer(workingImage);
        const auto nextFreeIdBefore = fixture.graph->nextFreeId;

        const auto inserted =
            fixture.graph->transferSegmentationSegmentToInitialSegment(1, 0, 0);
        if (inserted.has_value() || copyImageBuffer(workingImage) != before ||
            fixture.graph->nextFreeId != nextFreeIdBefore ||
            fixture.graph->initialNodes.count(nextFreeIdBefore) != 0) {
            return failTest("Missing WorkingNode prevalidation should abort before graph mutation.");
        }
    }

    {
        auto workingImage = makeImage(4, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        auto refinementImage = makeImage(4, 1, 1);
        refinementImage->SetPixel({1, 0, 0}, 9);
        auto fixture = buildGraphFixture(workingImage);
        itkSignal<SegmentIdType> refinementSignal(refinementImage, false);
        refinementSignal.ROI_set = true;
        refinementSignal.ROI_fx = 2;
        refinementSignal.ROI_tx = 3;
        refinementSignal.ROI_fy = refinementSignal.ROI_ty = 0;
        refinementSignal.ROI_fz = refinementSignal.ROI_tz = 0;
        fixture.graphBase->pSelectedRefinement = refinementImage;
        fixture.graphBase->pSelectedRefinementSignal = &refinementSignal;
        const auto before = copyImageBuffer(workingImage);
        const auto nextFreeIdBefore = fixture.graph->nextFreeId;

        fixture.graph->refineWithSelectedRefinementAtPosition(1, 0, 0);
        if (copyImageBuffer(workingImage) != before ||
            fixture.graph->nextFreeId != nextFreeIdBefore) {
            return failTest("A seed outside the refinement ROI should not mutate the graph.");
        }
    }

    {
        auto workingImage = makeImage(4, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        workingImage->SetPixel({2, 0, 0}, 1);
        auto refinementImage = makeImage(4, 1, 1);
        refinementImage->SetPixel({1, 0, 0}, 9);
        refinementImage->SetPixel({2, 0, 0}, 9);
        auto fixture = buildGraphFixture(workingImage);
        itkSignal<SegmentIdType> refinementSignal(refinementImage, false);
        refinementSignal.ROI_set = true;
        refinementSignal.ROI_fx = refinementSignal.ROI_tx = 1;
        refinementSignal.ROI_fy = refinementSignal.ROI_ty = 0;
        refinementSignal.ROI_fz = refinementSignal.ROI_tz = 0;
        fixture.graphBase->pSelectedRefinement = refinementImage;
        fixture.graphBase->pSelectedRefinementSignal = &refinementSignal;
        const SegmentIdType expectedLabel = fixture.graph->nextFreeId;

        fixture.graph->refineWithSelectedRefinementAtPosition(1, 0, 0);
        if (workingImage->GetPixel({1, 0, 0}) != expectedLabel ||
            workingImage->GetPixel({2, 0, 0}) != expectedLabel ||
            fixture.graphBase->pSelectedRefinement != refinementImage ||
            fixture.graphBase->pSelectedRefinementSignal != &refinementSignal) {
            return failTest("The ROI should gate only the seed and must not clip its connected component.");
        }
    }

    {
        auto workingImage = makeImage(4, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        auto selectedImage = makeImage(4, 1, 1);
        selectedImage->SetPixel({1, 0, 0}, 9);
        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        fixture.graph->nextFreeId = std::numeric_limits<SegmentIdType>::max();
        const auto before = copyImageBuffer(workingImage);

        const auto inserted =
            fixture.graph->transferSegmentationSegmentToInitialSegment(1, 0, 0);
        if (inserted.has_value() || copyImageBuffer(workingImage) != before ||
            fixture.graph->initialNodes.count(std::numeric_limits<SegmentIdType>::max()) != 0) {
            return failTest("Label exhaustion should abort refinement before mutation.");
        }
    }
    return 0;
}

int testAutomaticSixConnected3DSplitPreparation() {
    auto workingImage = makeImage(5, 5, 5);
    auto selectedImage = makeImage(5, 5, 5);
    const std::vector<std::array<int, 3>> largestComponent{
        {1, 1, 0}, {1, 1, 1}, {1, 1, 2}};
    const std::vector<std::array<int, 3>> clickedComponent{
        {2, 2, 2}, {2, 2, 3}};
    for (const auto &voxel : largestComponent) {
        workingImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 1);
        selectedImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 10);
    }
    for (const auto &voxel : clickedComponent) {
        workingImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 1);
        selectedImage->SetPixel({voxel[0], voxel[1], voxel[2]}, 10);
    }

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 10;

    ConnectedComponentSplitOptions options;
    options.connectivity = ConnectivityStencil::SixConnected;
    options.includedLabels.insert(10);
    options.ignoredLabels.insert(0);
    options.nextFreeLabel = 11;
    const auto stats = splitDisconnectedLabelComponentsInPlace(selectedImage, options);
    if (stats.finalLabelsByOriginalLabel.at(10)
            != std::vector<SegmentIdType>{10, 11}
        || countLabel(selectedImage, 10) != largestComponent.size()
        || countLabel(selectedImage, 11) != clickedComponent.size()
        || selectedImage->GetPixel({2, 2, 2}) != 11) {
        return failTest(
            "Automatic 3D preparation should keep the largest component label and relabel the clicked smaller component.");
    }
    selectedImage->Modified();

    auto resolution =
        fixture.graph->inspectSelectedSegmentationComponentInWorkingGraph(2, 2, 2);
    if (resolution.status != Graph::WorkingSegmentResolution::Status::NeedsInsertion) {
        return failTest(
            "A Full-connected but 6-disconnected WorkingNode should require targeted insertion.");
    }
    resolution = fixture.graph->ensureSelectedSegmentationComponentInWorkingGraph(2, 2, 2);
    if (resolution.status != Graph::WorkingSegmentResolution::Status::Inserted
        || workingImage->GetPixel({2, 2, 2}) != resolution.workingLabel
        || workingImage->GetPixel({1, 1, 2}) == resolution.workingLabel) {
        return failTest(
            "Automatic 3D preparation should isolate and insert only the clicked 6-connected component.");
    }
    const auto reused =
        fixture.graph->inspectSelectedSegmentationComponentInWorkingGraph(2, 2, 2);
    if (reused.status != Graph::WorkingSegmentResolution::Status::ReusedExisting
        || reused.workingLabel != resolution.workingLabel) {
        return failTest(
            "The inserted clicked component should immediately be reusable as an exact WorkingNode.");
    }

    const auto session =
        segment_puzzler::prepareSeededWatershedSplit(selectedImage, 11);
    if (session.connectedComponentCount != 1
        || session.voxelCount != clickedComponent.size()) {
        return failTest(
            "The 3D split session should contain only the clicked 6-connected component.");
    }
    std::array<segment_puzzler::SeededWatershedSplitSession::IndexType, 2> seeds;
    for (std::size_t seedIndex = 0; seedIndex < seeds.size(); ++seedIndex) {
        for (unsigned int axis = 0; axis < 3; ++axis) {
            seeds[seedIndex][axis] =
                clickedComponent[seedIndex][axis] - session.globalOffset[axis];
        }
    }
    const auto split =
        segment_puzzler::computeSeededWatershedSplit(session, seeds);
    if (!split.valid()
        || split.voxelCounts[0] + split.voxelCounts[1]
               != clickedComponent.size()) {
        return failTest(
            "The prepared watershed partition should cover only the clicked component.");
    }
    return 0;
}

int testBulkSegmentationDeleteUsesOneGraphOperation() {
    auto selectedImage = makeImage(6, 1, 1);
    selectedImage->SetPixel({0, 0, 0}, 1);
    selectedImage->SetPixel({1, 0, 0}, 2);
    selectedImage->SetPixel({2, 0, 0}, 3);
    selectedImage->SetPixel({3, 0, 0}, 2);
    selectedImage->SetPixel({4, 0, 0}, 1);

    auto graphBase = std::make_shared<GraphBase>();
    graphBase->pSelectedSegmentation = selectedImage;
    Graph graph(graphBase, false);

    const std::unordered_set<SegmentIdType> labelsToDelete{0, 1, 2};
    if (graph.deleteSegmentationLabels(labelsToDelete) != 4 ||
        countLabel(selectedImage, 1) != 0 || countLabel(selectedImage, 2) != 0 ||
        countLabel(selectedImage, 3) != 1 || countLabel(selectedImage, 0) != 5) {
        return failTest("Bulk segmentation delete should clear every requested foreground label once.");
    }

    graph.deleteSegmentationLabel(3);
    if (countLabel(selectedImage, 3) != 0 || countLabel(selectedImage, 0) != 6) {
        return failTest("Single-label segmentation delete should remain compatible with the bulk path.");
    }
    return 0;
}

int testSelectedSegmentationLabelsBelowVoxelCountUsesExclusiveThreshold() {
    auto workingImage = makeImage(7, 1, 1);
    workingImage->SetPixel({1, 0, 0}, 1);
    auto selectedImage = makeImage(7, 1, 1);
    selectedImage->SetPixel({0, 0, 0}, 10);
    selectedImage->SetPixel({1, 0, 0}, 20);
    selectedImage->SetPixel({2, 0, 0}, 20);
    selectedImage->SetPixel({3, 0, 0}, 30);
    selectedImage->SetPixel({4, 0, 0}, 30);
    selectedImage->SetPixel({5, 0, 0}, 30);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->ignoredSegmentLabels.push_back(20);

    if (fixture.graph->selectedSegmentationLabelsBelowVoxelCount(3) !=
        std::vector<SegmentIdType>{10}) {
        return failTest(
            "Small-label scan should exclude ignored labels and labels exactly at the threshold.");
    }
    if (fixture.graph->selectedSegmentationLabelsBelowVoxelCount(4) !=
        std::vector<SegmentIdType>{10, 30}) {
        return failTest("Small-label scan should return sorted labels strictly below the threshold.");
    }
    return 0;
}

int testNeighborMergeChoosesSmallestExactNeighbor() {
    auto workingImage = makeImage(8, 1, 1);
    workingImage->SetPixel({1, 0, 0}, 1);
    workingImage->SetPixel({2, 0, 0}, 1);
    workingImage->SetPixel({3, 0, 0}, 2);
    workingImage->SetPixel({4, 0, 0}, 2);
    workingImage->SetPixel({5, 0, 0}, 2);
    workingImage->SetPixel({6, 0, 0}, 3);

    auto selectedImage = makeImage(8, 1, 1);
    selectedImage->SetPixel({1, 0, 0}, 10);
    selectedImage->SetPixel({2, 0, 0}, 10);
    selectedImage->SetPixel({3, 0, 0}, 20);
    selectedImage->SetPixel({4, 0, 0}, 20);
    selectedImage->SetPixel({5, 0, 0}, 20);
    selectedImage->SetPixel({6, 0, 0}, 30);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 30;

    Graph::SegmentationNeighborMergeOptions options;
    options.neighborSelection = Graph::SegmentationNeighborSelection::Smallest;
    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({20}, options);
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged || result.requiredInsertionCount != 0 ||
        result.mergeableSelectedLabelCount != 1 || result.consumedLabelCount != 2 ||
        result.mergedGroupCount != 1 || !result.dataChanged ||
        result.newLabelByConsumedLabel != std::map<SegmentIdType, SegmentIdType>{{20, 31}, {30, 31}} ||
        result.voxelCountByConsumedLabel != std::map<SegmentIdType, std::size_t>{{20, 3}, {30, 1}} ||
        result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{31, 4}}) {
        return failTest("Exact neighbor merge should merge the selected label with one result group.");
    }
    if (countLabel(selectedImage, 10) != 2 || countLabel(selectedImage, 20) != 0 ||
        countLabel(selectedImage, 30) != 0 || countLabel(selectedImage, 31) != 4) {
        return failTest("Neighbor merge did not choose the one-voxel neighbor over the two-voxel neighbor.");
    }
    const SegmentIdType mergedWorkingLabel = workingImage->GetPixel({3, 0, 0});
    if (mergedWorkingLabel == 0 || workingImage->GetPixel({6, 0, 0}) != mergedWorkingLabel) {
        return failTest("Exact neighbor merge did not merge the expected WorkingNodes.");
    }
    return 0;
}

int testNeighborMergeChoosesLargestExactNeighbor() {
    auto workingImage = makeImage(8, 1, 1);
    workingImage->SetPixel({1, 0, 0}, 1);
    workingImage->SetPixel({2, 0, 0}, 1);
    workingImage->SetPixel({3, 0, 0}, 2);
    workingImage->SetPixel({4, 0, 0}, 2);
    workingImage->SetPixel({5, 0, 0}, 2);
    workingImage->SetPixel({6, 0, 0}, 3);

    auto selectedImage = makeImage(8, 1, 1);
    selectedImage->SetPixel({1, 0, 0}, 10);
    selectedImage->SetPixel({2, 0, 0}, 10);
    selectedImage->SetPixel({3, 0, 0}, 20);
    selectedImage->SetPixel({4, 0, 0}, 20);
    selectedImage->SetPixel({5, 0, 0}, 20);
    selectedImage->SetPixel({6, 0, 0}, 30);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 30;

    Graph::SegmentationNeighborMergeOptions options;
    options.neighborSelection = Graph::SegmentationNeighborSelection::Largest;
    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({20}, options);
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged || result.requiredInsertionCount != 0 ||
        result.mergeableSelectedLabelCount != 1 || result.consumedLabelCount != 2 ||
        result.mergedGroupCount != 1 || !result.dataChanged ||
        result.newLabelByConsumedLabel != std::map<SegmentIdType, SegmentIdType>{{10, 31}, {20, 31}} ||
        result.voxelCountByConsumedLabel != std::map<SegmentIdType, std::size_t>{{10, 2}, {20, 3}} ||
        result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{31, 5}}) {
        return failTest("Largest-neighbor merge should choose the two-voxel neighbor.");
    }
    if (countLabel(selectedImage, 10) != 0 || countLabel(selectedImage, 20) != 0 ||
        countLabel(selectedImage, 30) != 1 || countLabel(selectedImage, 31) != 5) {
        return failTest("Largest-neighbor merge changed the wrong selected-segmentation labels.");
    }
    return 0;
}

int testNeighborMergeDefaultsToMostConnectedNeighbor() {
    auto workingImage = makeImage(7, 7, 1);
    auto selectedImage = makeImage(7, 7, 1);

    // Label 10 is the smallest neighbor and label 40 is the largest. Label 30
    // has a size between them, but shares the most faces with label 20.
    workingImage->SetPixel({2, 2, 0}, 1);
    selectedImage->SetPixel({2, 2, 0}, 10);
    for (int y = 2; y <= 4; ++y) {
        workingImage->SetPixel({3, y, 0}, 2);
        selectedImage->SetPixel({3, y, 0}, 20);
        workingImage->SetPixel({4, y, 0}, 3);
        selectedImage->SetPixel({4, y, 0}, 30);
    }
    workingImage->SetPixel({5, 3, 0}, 3);
    selectedImage->SetPixel({5, 3, 0}, 30);
    for (const std::array<int, 2> coordinate :
         {std::array<int, 2>{2, 4}, {1, 4}, {0, 4}, {0, 5}, {0, 6}, {1, 6}}) {
        workingImage->SetPixel({coordinate[0], coordinate[1], 0}, 4);
        selectedImage->SetPixel({coordinate[0], coordinate[1], 0}, 40);
    }

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 40;

    const Graph::SegmentationNeighborMergeOptions options;
    if (options.neighborSelection != Graph::SegmentationNeighborSelection::MostConnected) {
        return failTest("Most-connected neighbor selection should be the default.");
    }
    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({20}, options);
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged || result.mergeableSelectedLabelCount != 1 ||
        result.newLabelByConsumedLabel !=
            std::map<SegmentIdType, SegmentIdType>{{20, 41}, {30, 41}} ||
        result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{41, 7}} ||
        countLabel(selectedImage, 10) != 1 || countLabel(selectedImage, 40) != 6 ||
        countLabel(selectedImage, 41) != 7) {
        return failTest(
            "Most-connected selection should differ from both size-based strategies.");
    }
    return 0;
}

int testMostConnectedMergeRefreshesSparseColorPresentation() {
    auto workingImage = makeImage(7, 7, 1);
    auto selectedImage = makeImage(7, 7, 1);

    workingImage->SetPixel({2, 2, 0}, 1);
    selectedImage->SetPixel({2, 2, 0}, 10);
    for (int y = 2; y <= 4; ++y) {
        workingImage->SetPixel({3, y, 0}, 2);
        selectedImage->SetPixel({3, y, 0}, 20);
        workingImage->SetPixel({4, y, 0}, 3);
        selectedImage->SetPixel({4, y, 0}, 30);
    }
    workingImage->SetPixel({5, 3, 0}, 3);
    selectedImage->SetPixel({5, 3, 0}, 30);

    auto fixture = buildGraphFixture(workingImage);
    itkSignal<SegmentIdType> workingSignal(workingImage, false);
    workingSignal.setCategoricalColorMode();
    workingSignal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Boundaries);
    itkSignal<SegmentIdType> selectedSignal(selectedImage, false);
    selectedSignal.setCategoricalColorMode();
    selectedSignal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Boundaries);
    fixture.graphBase->pWorkingSegments = &workingSignal;
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->pSelectedSegmentationSignal = &selectedSignal;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 30;

    // Reproduce the reported presentation boundary without constructing
    // thousands of intermediate graph edits.
    fixture.graph->nextFreeId = 11971;
    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
        {20}, Graph::SegmentationNeighborMergeOptions{});
    if (result.status != Graph::SegmentationNeighborMergeResult::Status::Merged) {
        return failTest("Synthetic most-connected merge should complete.");
    }
    const SegmentIdType mergedWorkingLabel = workingImage->GetPixel({3, 3, 0});
    if (mergedWorkingLabel != 11971) {
        return failTest("Synthetic merge should create the reported first out-of-range Working label.");
    }

    if (!fixture.graphBase->rebuildEdgeColorPresentation()) {
        return failTest("GUI-side edge color presentation rebuild should succeed.");
    }

    const SegmentIdType mergedSelectedLabel = result.newLabelByConsumedLabel.at(20);
    const std::array<unsigned int, 3> sliceIndices{{3, 3, 0}};
    for (unsigned int axis = 0; axis < 3; ++axis) {
        std::vector<quint32> workingBuffer;
        const QImage workingSlice = workingSignal.calculateSliceQImage(
            sliceIndices[axis], axis, &workingBuffer);
        const QRgb workingColor = workingSignal.colorForLabel(mergedWorkingLabel);
        if (workingSlice.isNull() ||
            std::find(workingBuffer.begin(), workingBuffer.end(), workingColor) == workingBuffer.end()) {
            return failTest("Boundary rendering should show the new sparse Working label on every axis.");
        }

        std::vector<quint32> selectedBuffer;
        const QImage selectedSlice = selectedSignal.calculateSliceQImage(
            sliceIndices[axis], axis, &selectedBuffer);
        const QRgb selectedColor = selectedSignal.colorForLabel(mergedSelectedLabel);
        if (selectedSlice.isNull() ||
            std::find(selectedBuffer.begin(), selectedBuffer.end(), selectedColor) == selectedBuffer.end()) {
            return failTest("Boundary rendering should show the merged Selected label on every axis.");
        }
    }
    return 0;
}

int testMostConnectedNeighborUsesStableLabelTieBreak() {
    auto workingImage = makeImage(3, 3, 1);
    auto selectedImage = makeImage(3, 3, 1);
    workingImage->SetPixel({1, 1, 0}, 2);
    selectedImage->SetPixel({1, 1, 0}, 20);
    workingImage->SetPixel({0, 1, 0}, 3);
    selectedImage->SetPixel({0, 1, 0}, 30);
    workingImage->SetPixel({2, 1, 0}, 1);
    selectedImage->SetPixel({2, 1, 0}, 10);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 30;

    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
        {20}, Graph::SegmentationNeighborMergeOptions{});
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged ||
        result.newLabelByConsumedLabel !=
            std::map<SegmentIdType, SegmentIdType>{{10, 31}, {20, 31}} ||
        countLabel(selectedImage, 30) != 1 || countLabel(selectedImage, 31) != 2) {
        return failTest(
            "Most-connected ties should deterministically choose the smaller neighbor label.");
    }
    return 0;
}

int testNeighborMergeRejectsInvalidSelectionWithoutMutation() {
    auto workingImage = makeImage(2, 1, 1);
    auto selectedImage = makeImage(2, 1, 1);
    workingImage->SetPixel({0, 0, 0}, 1);
    workingImage->SetPixel({1, 0, 0}, 2);
    selectedImage->SetPixel({0, 0, 0}, 10);
    selectedImage->SetPixel({1, 0, 0}, 20);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 20;
    const auto workingBefore = copyImageBuffer(workingImage);
    const auto selectedBefore = copyImageBuffer(selectedImage);
    const auto nextFreeIdBefore = fixture.graph->nextFreeId;
    const auto workingNodeCountBefore = fixture.graph->workingNodes.size();
    const auto initialNodeCountBefore = fixture.graph->initialNodes.size();

    Graph::SegmentationNeighborMergeOptions options;
    options.neighborSelection = static_cast<Graph::SegmentationNeighborSelection>(999);
    if (Graph::segmentationNeighborSelectionName(options.neighborSelection) != nullptr) {
        return failTest("An invalid neighbor-selection value should have no canonical name.");
    }
    const auto result =
        fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({10}, options);
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Failed || result.dataChanged ||
        copyImageBuffer(workingImage) != workingBefore ||
        copyImageBuffer(selectedImage) != selectedBefore ||
        fixture.graph->nextFreeId != nextFreeIdBefore ||
        fixture.graph->workingNodes.size() != workingNodeCountBefore ||
        fixture.graph->initialNodes.size() != initialNodeCountBefore) {
        return failTest(
            "An invalid neighbor-selection value should fail before mutating images or graph state.");
    }
    return 0;
}

int testNeighborMergeRequiresConfirmationBeforeInsertion() {
    auto workingImage = makeImage(6, 1, 1);
    workingImage->SetPixel({1, 0, 0}, 1);
    workingImage->SetPixel({2, 0, 0}, 1);
    workingImage->SetPixel({3, 0, 0}, 2);
    workingImage->SetPixel({4, 0, 0}, 2);

    auto selectedImage = makeImage(6, 1, 1);
    selectedImage->SetPixel({2, 0, 0}, 10);
    selectedImage->SetPixel({3, 0, 0}, 20);
    selectedImage->SetPixel({4, 0, 0}, 20);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 20;
    const auto workingBefore = copyImageBuffer(workingImage);
    const auto selectedBefore = copyImageBuffer(selectedImage);
    const auto nextFreeIdBefore = fixture.graph->nextFreeId;
    const auto workingNodeCountBefore = fixture.graph->workingNodes.size();

    using Status = Graph::SegmentationNeighborMergeResult::Status;
    auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
        {10}, Graph::SegmentationNeighborMergeOptions{});
    if (result.status != Status::NeedsInsertionConfirmation || result.requiredInsertionCount != 1 ||
        result.dataChanged) {
        return failTest("A selected subset of a WorkingNode should require insertion confirmation.");
    }
    if (copyImageBuffer(workingImage) != workingBefore || copyImageBuffer(selectedImage) != selectedBefore ||
        fixture.graph->nextFreeId != nextFreeIdBefore ||
        fixture.graph->workingNodes.size() != workingNodeCountBefore) {
        return failTest("Insertion preflight must not mutate either segmentation or the WorkingGraph.");
    }

    Graph::SegmentationNeighborMergeOptions insertionOptions;
    insertionOptions.allowInsertion = true;
    result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({10}, insertionOptions);
    if (result.status != Status::Merged || result.requiredInsertionCount != 1 ||
        result.mergedGroupCount != 1) {
        return failTest("Confirmed insertion should complete the neighbor merge.");
    }
    if (countLabel(selectedImage, 10) != 0 || countLabel(selectedImage, 20) != 0 ||
        countLabel(selectedImage, 21) != 3 || selectedImage->GetPixel({1, 0, 0}) != 0) {
        return failTest("Confirmed insertion merge produced unexpected selected-segmentation voxels.");
    }
    return 0;
}

int testNeighborMergeRepairsWorkingNodeSplitByInsertion() {
    auto workingImage = makeImage(8, 5, 3);
    workingImage->SetPixel({2, 2, 1}, 1);
    workingImage->SetPixel({3, 2, 1}, 2);
    workingImage->SetPixel({4, 2, 1}, 3);
    workingImage->SetPixel({5, 2, 1}, 3);

    auto fixture = buildGraphFixture(workingImage);
    const auto edge = fixture.graph->initialLabelPairToTwoSidedInitialEdge.find({1, 2});
    if (edge == fixture.graph->initialLabelPairToTwoSidedInitialEdge.end()) {
        return failTest("Expected an initial edge for the exact merged WorkingNode fixture.");
    }
    fixture.graph->mergeEdge(edge->second.get());

    auto selectedImage = makeImage(8, 5, 3);
    selectedImage->SetPixel({2, 2, 1}, 10);
    selectedImage->SetPixel({3, 2, 1}, 10);
    selectedImage->SetPixel({4, 2, 1}, 20);
    selectedImage->SetPixel({5, 2, 1}, 30);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 30;

    Graph::SegmentationNeighborMergeOptions options;
    options.allowInsertion = true;
    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({10}, options);
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged || result.requiredInsertionCount != 1 ||
        result.mergedGroupCount != 1 || countLabel(selectedImage, 31) != 3 ||
        countLabel(selectedImage, 10) != 0 || countLabel(selectedImage, 20) != 0 ||
        countLabel(selectedImage, 30) != 1) {
        return failTest(
            "Neighbor merge should restore an exact WorkingNode split by a nearby insertion.");
    }

    const SegmentIdType mergedWorkingLabel = workingImage->GetPixel({2, 2, 1});
    if (mergedWorkingLabel == 0 || workingImage->GetPixel({3, 2, 1}) != mergedWorkingLabel ||
        workingImage->GetPixel({4, 2, 1}) != mergedWorkingLabel ||
        workingImage->GetPixel({5, 2, 1}) == mergedWorkingLabel) {
        return failTest("Repaired neighbor merge produced unexpected Working Segments.");
    }
    return 0;
}

int testNeighborMergeCountPreflightRejectsDifferentVoxelSet() {
    auto workingImage = makeImage(6, 1, 1);
    workingImage->SetPixel({1, 0, 0}, 1);
    workingImage->SetPixel({2, 0, 0}, 1);
    workingImage->SetPixel({3, 0, 0}, 2);
    workingImage->SetPixel({4, 0, 0}, 2);

    auto selectedImage = makeImage(6, 1, 1);
    selectedImage->SetPixel({2, 0, 0}, 10);
    selectedImage->SetPixel({3, 0, 0}, 10);
    selectedImage->SetPixel({4, 0, 0}, 20);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 20;
    const auto workingBefore = copyImageBuffer(workingImage);
    const auto selectedBefore = copyImageBuffer(selectedImage);

    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
        {10}, Graph::SegmentationNeighborMergeOptions{});
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::NeedsInsertionConfirmation ||
        result.requiredInsertionCount != 2 || result.dataChanged ||
        copyImageBuffer(workingImage) != workingBefore ||
        copyImageBuffer(selectedImage) != selectedBefore) {
        return failTest(
            "Count-based merge preflight must reject an equal-sized WorkingNode with different voxels.");
    }
    return 0;
}

int testNeighborMergeCombinesSelectedChainsOnce() {
    auto workingImage = makeImage(8, 1, 1);
    auto selectedImage = makeImage(8, 1, 1);
    for (int x = 1; x <= 3; ++x) {
        workingImage->SetPixel({x, 0, 0}, 1);
        selectedImage->SetPixel({x, 0, 0}, 10);
    }
    for (int x = 4; x <= 5; ++x) {
        workingImage->SetPixel({x, 0, 0}, 2);
        selectedImage->SetPixel({x, 0, 0}, 20);
    }
    workingImage->SetPixel({6, 0, 0}, 3);
    selectedImage->SetPixel({6, 0, 0}, 30);

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 30;

    Graph::SegmentationNeighborMergeOptions options;
    options.neighborSelection = Graph::SegmentationNeighborSelection::Smallest;
    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors({10, 20}, options);
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged || result.mergeableSelectedLabelCount != 2 ||
        result.consumedLabelCount != 3 || result.mergedGroupCount != 1 ||
        result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{31, 6}}) {
        return failTest("A selected merge chain should produce one deduplicated result group.");
    }
    if (countLabel(selectedImage, 10) != 0 || countLabel(selectedImage, 20) != 0 ||
        countLabel(selectedImage, 30) != 0 || countLabel(selectedImage, 31) != 6) {
        return failTest("Selected merge chain was not transferred as one fresh segmentation label.");
    }
    return 0;
}

int testNeighborMergeDeduplicatesMutualSelection() {
    auto workingImage = makeImage(6, 1, 1);
    auto selectedImage = makeImage(6, 1, 1);
    for (int x = 1; x <= 2; ++x) {
        workingImage->SetPixel({x, 0, 0}, 1);
        selectedImage->SetPixel({x, 0, 0}, 10);
    }
    for (int x = 3; x <= 4; ++x) {
        workingImage->SetPixel({x, 0, 0}, 2);
        selectedImage->SetPixel({x, 0, 0}, 20);
    }

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 20;

    const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
        {10, 20}, Graph::SegmentationNeighborMergeOptions{});
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    if (result.status != Status::Merged || result.mergeableSelectedLabelCount != 2 ||
        result.consumedLabelCount != 2 || result.mergedGroupCount != 1 ||
        result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{21, 4}} ||
        countLabel(selectedImage, 21) != 4) {
        return failTest("Mutually selected neighbors should merge once without swapping labels.");
    }
    return 0;
}

int testNeighborMergePropagatesOnlyNewSmallLabels() {
    auto workingImage = makeImage(10, 1, 1);
    auto selectedImage = makeImage(10, 1, 1);
    const std::array<SegmentIdType, 8> workingLabels{{1, 2, 2, 3, 3, 4, 0, 5}};
    const std::array<SegmentIdType, 8> selectedLabels{{10, 20, 20, 30, 30, 40, 0, 50}};
    for (std::size_t offset = 0; offset < workingLabels.size(); ++offset) {
        const int x = static_cast<int>(offset + 1);
        workingImage->SetPixel({x, 0, 0}, workingLabels[offset]);
        selectedImage->SetPixel({x, 0, 0}, selectedLabels[offset]);
    }

    auto fixture = buildGraphFixture(workingImage);
    fixture.graphBase->pSelectedSegmentation = selectedImage;
    fixture.graphBase->selectedSegmentationMaxSegmentId = 50;

    constexpr std::size_t threshold = 6;
    auto candidates = fixture.graph->selectedSegmentationLabelsBelowVoxelCount(threshold);
    std::size_t deferredNoNeighborCount = 0;
    std::size_t iterationCount = 0;
    while (!candidates.empty()) {
        Graph::SegmentationNeighborMergeOptions options;
        options.neighborSelection = Graph::SegmentationNeighborSelection::Smallest;
        const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
            candidates,
            options);
        using Status = Graph::SegmentationNeighborMergeResult::Status;
        if (result.status != Status::Merged) {
            return failTest("Incremental neighbor-merge fixture should merge every scheduled round.");
        }
        deferredNoNeighborCount += result.skippedNoNeighborCount;
        candidates.clear();
        for (const auto &[label, voxelCount] : result.voxelCountByNewLabel) {
            if (voxelCount < threshold) {
                candidates.push_back(label);
            }
        }
        ++iterationCount;
    }

    if (iterationCount != 2 || deferredNoNeighborCount != 1 ||
        countLabel(selectedImage, 50) != 1 || countLabel(selectedImage, 53) != 6 ||
        fixture.graph->selectedSegmentationLabelsBelowVoxelCount(threshold) !=
            std::vector<SegmentIdType>{50}) {
        return failTest(
            "Only new below-threshold merge results should be propagated; isolated labels stay deferred.");
    }
    return 0;
}

int testNeighborMergeSkipsIsolatedAndRecoversDisconnectedLabels() {
    using Status = Graph::SegmentationNeighborMergeResult::Status;
    {
        auto workingImage = makeImage(5, 1, 1);
        workingImage->SetPixel({1, 0, 0}, 1);
        workingImage->SetPixel({3, 0, 0}, 2);
        auto selectedImage = makeImage(5, 1, 1);
        selectedImage->SetPixel({1, 0, 0}, 10);
        selectedImage->SetPixel({3, 0, 0}, 20);
        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        fixture.graphBase->selectedSegmentationMaxSegmentId = 20;
        const auto selectedBefore = copyImageBuffer(selectedImage);

        const auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
            {10}, Graph::SegmentationNeighborMergeOptions{});
        if (result.status != Status::NothingToMerge || result.skippedNoNeighborCount != 1 ||
            copyImageBuffer(selectedImage) != selectedBefore) {
            return failTest("An isolated selected label should be skipped without mutation.");
        }
    }
    {
        auto workingImage = makeImage(4, 1, 1);
        workingImage->SetPixel({0, 0, 0}, 1);
        workingImage->SetPixel({1, 0, 0}, 2);
        workingImage->SetPixel({2, 0, 0}, 1);
        auto selectedImage = makeImage(4, 1, 1);
        selectedImage->SetPixel({0, 0, 0}, 10);
        selectedImage->SetPixel({1, 0, 0}, 20);
        selectedImage->SetPixel({2, 0, 0}, 10);
        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        fixture.graphBase->selectedSegmentationMaxSegmentId = 20;
        const auto workingBefore = copyImageBuffer(workingImage);
        const auto selectedBefore = copyImageBuffer(selectedImage);

        auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
            {10}, Graph::SegmentationNeighborMergeOptions{});
        if (result.status != Status::NeedsConnectedComponentConfirmation ||
            result.disconnectedLabelCount != 1 || result.disconnectedRegionCount != 2 ||
            copyImageBuffer(workingImage) != workingBefore || copyImageBuffer(selectedImage) != selectedBefore ||
            fixture.graph->nextFreeId != 3 || fixture.graph->workingNodes.size() != 2) {
            return failTest("A disconnected involved label should request confirmation without mutation.");
        }

        Graph::SegmentationNeighborMergeOptions connectedComponentOptions;
        connectedComponentOptions.allowInsertion = true;
        connectedComponentOptions.allowConnectedComponentSplit = true;
        result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
            {10}, connectedComponentOptions);
        if (result.status != Status::Merged || !result.dataChanged ||
            result.disconnectedLabelCount != 1 || result.disconnectedRegionCount != 2 ||
            result.mergedGroupCount != 1 ||
            result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{22, 3}} ||
            countLabel(selectedImage, 22) != 3 ||
            countLabel(selectedImage, 10) != 0 || countLabel(selectedImage, 20) != 0 ||
            countLabel(selectedImage, 21) != 0) {
            return failTest("Confirmed Connected Components should reinsert and merge every connected region.");
        }
    }
    {
        auto workingImage = makeImage(5, 1, 1);
        workingImage->SetPixel({0, 0, 0}, 1);
        workingImage->SetPixel({1, 0, 0}, 2);
        workingImage->SetPixel({3, 0, 0}, 2);
        auto selectedImage = makeImage(5, 1, 1);
        selectedImage->SetPixel({0, 0, 0}, 10);
        selectedImage->SetPixel({1, 0, 0}, 20);
        selectedImage->SetPixel({3, 0, 0}, 20);
        auto fixture = buildGraphFixture(workingImage);
        fixture.graphBase->pSelectedSegmentation = selectedImage;
        fixture.graphBase->selectedSegmentationMaxSegmentId = 20;

        auto result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
            {10}, Graph::SegmentationNeighborMergeOptions{});
        if (result.status != Status::NeedsConnectedComponentConfirmation ||
            result.disconnectedLabelCount != 1 || result.disconnectedRegionCount != 2) {
            return failTest("A disconnected chosen neighbor should also request confirmation.");
        }

        Graph::SegmentationNeighborMergeOptions connectedComponentOptions;
        connectedComponentOptions.allowInsertion = true;
        connectedComponentOptions.allowConnectedComponentSplit = true;
        result = fixture.graph->mergeSelectedSegmentationLabelsWithNeighbors(
            {10}, connectedComponentOptions);
        if (result.status != Status::Merged || result.disconnectedLabelCount != 1 ||
            result.disconnectedRegionCount != 2 ||
            result.voxelCountByNewLabel != std::map<SegmentIdType, std::size_t>{{22, 2}} ||
            fixture.graph->selectedSegmentationLabelsBelowVoxelCount(2) !=
                std::vector<SegmentIdType>{21} ||
            countLabel(selectedImage, 21) != 1 ||
            countLabel(selectedImage, 22) != 2 || countLabel(selectedImage, 10) != 0 ||
            countLabel(selectedImage, 20) != 0) {
            return failTest("A disconnected chosen neighbor should split before its adjacent region is merged.");
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    if (int result = testUtilityConnectivityModes()) {
        return result;
    }
    if (int result = testSeededComponentTraversal()) {
        return result;
    }
    if (int result = testFindPresentLabelsUsesCandidateSet()) {
        return result;
    }
    if (int result = testUtilityLargestComponentKeepsOriginalLabel()) {
        return result;
    }
    if (int result = testUtilityIgnoresBackground()) {
        return result;
    }
    if (int result = testUtilityLimitsSplitToIncludedLabels()) {
        return result;
    }
    if (int result = testUtilityPreflightsLabelExhaustion()) {
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
    if (int result = testRefinementSynchronizesOverwrittenNodeMetadata()) {
        return result;
    }
    if (int result = testRefinementPrevalidationAndRoiStayMutationFree()) {
        return result;
    }
    if (int result = testAutomaticSixConnected3DSplitPreparation()) {
        return result;
    }
    if (int result = testBulkSegmentationDeleteUsesOneGraphOperation()) {
        return result;
    }
    if (int result = testSelectedSegmentationLabelsBelowVoxelCountUsesExclusiveThreshold()) {
        return result;
    }
    if (int result = testNeighborMergeChoosesSmallestExactNeighbor()) {
        return result;
    }
    if (int result = testNeighborMergeChoosesLargestExactNeighbor()) {
        return result;
    }
    if (int result = testNeighborMergeDefaultsToMostConnectedNeighbor()) {
        return result;
    }
    if (int result = testMostConnectedMergeRefreshesSparseColorPresentation()) {
        return result;
    }
    if (int result = testMostConnectedNeighborUsesStableLabelTieBreak()) {
        return result;
    }
    if (int result = testNeighborMergeRejectsInvalidSelectionWithoutMutation()) {
        return result;
    }
    if (int result = testNeighborMergeRequiresConfirmationBeforeInsertion()) {
        return result;
    }
    if (int result = testNeighborMergeRepairsWorkingNodeSplitByInsertion()) {
        return result;
    }
    if (int result = testNeighborMergeCountPreflightRejectsDifferentVoxelSet()) {
        return result;
    }
    if (int result = testNeighborMergeCombinesSelectedChainsOnce()) {
        return result;
    }
    if (int result = testNeighborMergeDeduplicatesMutualSelection()) {
        return result;
    }
    if (int result = testNeighborMergePropagatesOnlyNewSmallLabels()) {
        return result;
    }
    if (int result = testNeighborMergeSkipsIsolatedAndRecoversDisconnectedLabels()) {
        return result;
    }

    std::cout << "Connected component split tests passed.\n";
    return 0;
}
