#include "src/itkImageFilters/itkWatershedHelpers.h"

#include <itkScalarConnectedComponentImageFilter.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using LabelImage = dataType::SegmentsImageType;
using BoundaryImage = itk::Image<unsigned char, 3>;
using Label = dataType::SegmentIdType;

template <typename ImageType>
typename ImageType::Pointer makeImage(const itk::Size<3> &size,
                                      typename ImageType::PixelType fillValue = {}) {
    auto image = ImageType::New();
    typename ImageType::RegionType region;
    region.SetSize(size);
    image->SetRegions(region);
    image->Allocate();
    image->FillBuffer(fillValue);
    return image;
}

std::size_t linearIndex(const itk::Size<3> &size, int x, int y, int z) {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(y) * size[0] +
           static_cast<std::size_t>(z) * size[0] * size[1];
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool imagesEqual(LabelImage::Pointer first, LabelImage::Pointer second) {
    if (first.IsNull() || second.IsNull() ||
        first->GetLargestPossibleRegion().GetSize() != second->GetLargestPossibleRegion().GetSize()) {
        return false;
    }
    const std::size_t voxelCount = first->GetLargestPossibleRegion().GetNumberOfPixels();
    return std::equal(
        first->GetBufferPointer(),
        first->GetBufferPointer() + voxelCount,
        second->GetBufferPointer());
}

bool splitMapsEqual(const BoundaryConsistentPartitionResult::SplitComponentMap &first,
                    const BoundaryConsistentPartitionResult::SplitComponentMap &second) {
    if (first.size() != second.size()) {
        return false;
    }
    for (const auto &entry : first) {
        const auto match = second.find(entry.first);
        if (match == second.end() || match->second != entry.second) {
            return false;
        }
    }
    return true;
}

void requireSameResult(const BoundaryConsistentPartitionResult &reference,
                       const BoundaryConsistentPartitionResult &candidate,
                       const std::string &context) {
    require(imagesEqual(reference.displayLabels, candidate.displayLabels),
            context + ": display labels differ");
    require(splitMapsEqual(reference.splitComponentIds, candidate.splitComponentIds),
            context + ": split-component maps differ");
}

void requireValidPartition(LabelImage::Pointer labels,
                           BoundaryImage::Pointer boundaries,
                           const BoundaryConsistentPartitionResult &partition) {
    const std::size_t voxelCount = labels->GetLargestPossibleRegion().GetNumberOfPixels();
    auto activeMask = makeImage<BoundaryImage>(labels->GetLargestPossibleRegion().GetSize());
    const Label *labelBuffer = labels->GetBufferPointer();
    const unsigned char *boundaryBuffer = boundaries->GetBufferPointer();
    unsigned char *maskBuffer = activeMask->GetBufferPointer();
    for (std::size_t index = 0; index < voxelCount; ++index) {
        maskBuffer[index] = labelBuffer[index] != 0 && boundaryBuffer[index] == 0 ? 1 : 0;
    }

    using ReferenceFilter = itk::ScalarConnectedComponentImageFilter<LabelImage, LabelImage, BoundaryImage>;
    auto referenceFilter = ReferenceFilter::New();
    referenceFilter->SetInput(labels);
    referenceFilter->SetMaskImage(activeMask);
    referenceFilter->SetDistanceThreshold(0);
    referenceFilter->SetFullyConnected(false);
    referenceFilter->Update();

    const Label *componentBuffer = referenceFilter->GetOutput()->GetBufferPointer();
    const Label *displayBuffer = partition.displayLabels->GetBufferPointer();
    std::unordered_map<Label, Label> componentToDisplayLabel;
    std::unordered_map<Label, Label> displayLabelToComponent;
    std::unordered_map<Label, std::vector<Label>> componentsByOriginalLabel;
    Label maximumOriginalLabel = 0;

    for (std::size_t index = 0; index < voxelCount; ++index) {
        maximumOriginalLabel = std::max(maximumOriginalLabel, labelBuffer[index]);
        if (maskBuffer[index] == 0) {
            require(displayBuffer[index] == 0, "inactive voxel is nonzero in the display partition");
            continue;
        }

        const Label component = componentBuffer[index];
        const Label displayLabel = displayBuffer[index];
        require(component != 0 && displayLabel != 0, "active voxel is missing a component label");

        const auto componentInsertion = componentToDisplayLabel.emplace(component, displayLabel);
        require(componentInsertion.second || componentInsertion.first->second == displayLabel,
                "one reference component received multiple display labels");
        const auto displayInsertion = displayLabelToComponent.emplace(displayLabel, component);
        require(displayInsertion.second || displayInsertion.first->second == component,
                "disconnected reference components share one display label");

        auto &originalComponents = componentsByOriginalLabel[labelBuffer[index]];
        if (std::find(originalComponents.begin(), originalComponents.end(), component) == originalComponents.end()) {
            originalComponents.push_back(component);
        }
    }

    for (const auto &entry : componentsByOriginalLabel) {
        const Label originalLabel = entry.first;
        const auto &components = entry.second;
        const auto splitEntry = partition.splitComponentIds.find(originalLabel);
        if (components.size() == 1) {
            require(componentToDisplayLabel.at(components.front()) == originalLabel,
                    "unsplit component did not retain its original label");
            require(splitEntry == partition.splitComponentIds.end(),
                    "unsplit label appears in the split map");
            continue;
        }

        require(splitEntry != partition.splitComponentIds.end(),
                "split original label is missing from the split map");
        std::vector<Label> expectedSplitLabels;
        expectedSplitLabels.reserve(components.size());
        for (Label component : components) {
            const Label displayLabel = componentToDisplayLabel.at(component);
            require(displayLabel > maximumOriginalLabel,
                    "split component reused an original label");
            expectedSplitLabels.push_back(displayLabel);
        }
        require(splitEntry->second == expectedSplitLabels,
                "split map order does not follow deterministic component order");
    }
}

void testKnownSplitAcrossSlabs() {
    itk::Size<3> size;
    size[0] = 5;
    size[1] = 4;
    size[2] = 7;
    auto labels = makeImage<LabelImage>(size);
    auto boundaries = makeImage<BoundaryImage>(size);
    Label *labelBuffer = labels->GetBufferPointer();
    unsigned char *boundaryBuffer = boundaries->GetBufferPointer();

    for (int z = 0; z < 7; ++z) {
        labelBuffer[linearIndex(size, 0, 0, z)] = 1;
        labelBuffer[linearIndex(size, 2, 1, z)] = 2;
    }
    labelBuffer[linearIndex(size, 4, 3, 2)] = 3;
    boundaryBuffer[linearIndex(size, 2, 1, 3)] = 1;

    const auto serial = deriveBoundaryConsistentPartition(
        labels, boundaries, WatershedRunOptions{}, false, DistanceMapAlgorithm::Maurer, 1);
    const auto parallel = deriveBoundaryConsistentPartition(
        labels, boundaries, WatershedRunOptions{}, false, DistanceMapAlgorithm::Maurer, 16);
    requireSameResult(serial, parallel, "known split");
    requireValidPartition(labels, boundaries, parallel);

    const Label *display = parallel.displayLabels->GetBufferPointer();
    for (int z = 0; z < 7; ++z) {
        require(display[linearIndex(size, 0, 0, z)] == 1,
                "component crossing slab boundaries changed label");
    }
    for (int z = 0; z < 3; ++z) {
        require(display[linearIndex(size, 2, 1, z)] == 4,
                "lower split component has an unexpected label");
    }
    require(display[linearIndex(size, 2, 1, 3)] == 0,
            "boundary voxel was not cleared");
    for (int z = 4; z < 7; ++z) {
        require(display[linearIndex(size, 2, 1, z)] == 5,
                "upper split component has an unexpected label");
    }
    require(display[linearIndex(size, 4, 3, 2)] == 3,
            "unsplit component changed label");

    const auto splitEntry = parallel.splitComponentIds.find(2);
    require(splitEntry != parallel.splitComponentIds.end(), "split label is missing from the result map");
    require(splitEntry->second == std::vector<Label>({4, 5}),
            "split label mapping is not deterministic");
}

void testRepeatedParallelEquivalence() {
    itk::Size<3> size;
    size[0] = 19;
    size[1] = 13;
    size[2] = 11;
    auto labels = makeImage<LabelImage>(size);
    auto boundaries = makeImage<BoundaryImage>(size);
    Label *labelBuffer = labels->GetBufferPointer();
    unsigned char *boundaryBuffer = boundaries->GetBufferPointer();

    for (int z = 0; z < static_cast<int>(size[2]); ++z) {
        for (int y = 0; y < static_cast<int>(size[1]); ++y) {
            for (int x = 0; x < static_cast<int>(size[0]); ++x) {
                const std::size_t index = linearIndex(size, x, y, z);
                labelBuffer[index] = static_cast<Label>(
                    1 + ((x / 3) + 2 * (y / 4) + 3 * (z / 3)) % 7);
                if ((17 * x + 11 * y + 5 * z) % 29 == 0) {
                    boundaryBuffer[index] = 1;
                }
            }
        }
    }

    const std::vector<Label> originalLabels(
        labels->GetBufferPointer(),
        labels->GetBufferPointer() + labels->GetLargestPossibleRegion().GetNumberOfPixels());
    const auto serial = deriveBoundaryConsistentPartition(
        labels, boundaries, WatershedRunOptions{}, false, DistanceMapAlgorithm::Maurer, 1);
    requireValidPartition(labels, boundaries, serial);
    for (int repetition = 0; repetition < 20; ++repetition) {
        const auto parallel = deriveBoundaryConsistentPartition(
            labels, boundaries, WatershedRunOptions{}, false, DistanceMapAlgorithm::Maurer, 32);
        requireSameResult(serial, parallel, "parallel repetition " + std::to_string(repetition));
    }
    require(std::equal(originalLabels.begin(), originalLabels.end(), labels->GetBufferPointer()),
            "boundary relabeling modified the canonical input labels");
}

void testMissingAndMismatchedBoundaryImages() {
    itk::Size<3> size;
    size.Fill(3);
    auto labels = makeImage<LabelImage>(size, 7);
    BoundaryImage::Pointer noBoundaries;
    const auto unchanged = deriveBoundaryConsistentPartition(
        labels, noBoundaries, WatershedRunOptions{}, false, DistanceMapAlgorithm::Maurer, 8);
    require(imagesEqual(labels, unchanged.displayLabels),
            "missing boundary image changed display labels");
    require(unchanged.splitComponentIds.empty(),
            "missing boundary image produced split labels");

    itk::Size<3> mismatchedSize = size;
    mismatchedSize[2] = 2;
    auto mismatchedBoundaries = makeImage<BoundaryImage>(mismatchedSize);
    bool threw = false;
    try {
        (void)deriveBoundaryConsistentPartition(
            labels, mismatchedBoundaries, WatershedRunOptions{}, false, DistanceMapAlgorithm::Maurer, 8);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    require(threw, "mismatched boundary shape was accepted");
}

} // namespace

int main() {
    try {
        setWatershedLogSink([](const std::string &) {});
        testKnownSplitAcrossSlabs();
        testRepeatedParallelEquivalence();
        testMissingAndMismatchedBoundaryImages();
        setWatershedLogSink({});
        std::cout << "Boundary component relabel tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        setWatershedLogSink({});
        std::cerr << "Boundary component relabel test failed: " << error.what() << '\n';
        return 1;
    }
}
