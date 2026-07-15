#include <QCoreApplication>
#include <QFileInfo>

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "projected_3d_cut_sample_utils.h"

namespace {

namespace sample = projected3dcutsample;
using SegmentIdType = dataType::SegmentIdType;

struct ProgramOptions {
    QString segmentsPath;
    int viewportSize = 1024;
    double strokeWidthPixels = 6.0;
};

int failTest(const std::string &message) {
    std::cerr << "Assertion failed: " << message << "\n";
    return 1;
}

bool parseArgs(int argc, char **argv, ProgramOptions &options) {
    for (int index = 1; index < argc; ++index) {
        const QString arg = QString::fromLocal8Bit(argv[index]);
        if (arg == "--segments" && index + 1 < argc) {
            options.segmentsPath = QString::fromLocal8Bit(argv[++index]);
        } else if (arg == "--viewport" && index + 1 < argc) {
            options.viewportSize = std::max(128, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (arg == "--stroke-width" && index + 1 < argc) {
            options.strokeWidthPixels = std::max(1.0, QString::fromLocal8Bit(argv[++index]).toDouble());
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--segments PATH] [--viewport N] [--stroke-width PX]\n";
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg.toStdString() << "\n";
            return false;
        }
    }

    if (options.segmentsPath.isEmpty()) {
        options.segmentsPath = sample::defaultSampleSegmentsPath();
    }
    return true;
}

std::size_t countNonBackgroundVoxels(const dataType::SegmentsImageType::Pointer &image, SegmentIdType backgroundId) {
    const auto voxelCount = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *buffer = image->GetBufferPointer();
    std::size_t result = 0;
    for (std::size_t index = 0; index < voxelCount; ++index) {
        result += buffer[index] != backgroundId ? 1U : 0U;
    }
    return result;
}

bool imagesEqual(const dataType::SegmentsImageType::Pointer &left,
                 const dataType::SegmentsImageType::Pointer &right) {
    if (left.IsNull() || right.IsNull() ||
        left->GetLargestPossibleRegion() != right->GetLargestPossibleRegion()) {
        return false;
    }
    const auto voxelCount = left->GetLargestPossibleRegion().GetNumberOfPixels();
    return std::equal(left->GetBufferPointer(),
                      left->GetBufferPointer() + voxelCount,
                      right->GetBufferPointer());
}

bool labelIsConnectedInImage(const dataType::SegmentsImageType::Pointer &image, SegmentIdType label) {
    const auto size = image->GetLargestPossibleRegion().GetSize();
    const std::ptrdiff_t dimX = static_cast<std::ptrdiff_t>(size[0]);
    const std::ptrdiff_t dimY = static_cast<std::ptrdiff_t>(size[1]);
    const std::ptrdiff_t dimZ = static_cast<std::ptrdiff_t>(size[2]);
    const std::ptrdiff_t planeXY = dimX * dimY;
    const std::ptrdiff_t total = planeXY * dimZ;
    const auto *buffer = image->GetBufferPointer();

    std::ptrdiff_t seed = -1;
    std::size_t totalLabelCount = 0;
    for (std::ptrdiff_t index = 0; index < total; ++index) {
        if (buffer[index] != label) {
            continue;
        }
        ++totalLabelCount;
        if (seed < 0) {
            seed = index;
        }
    }
    if (seed < 0) {
        return false;
    }

    std::vector<unsigned char> visited(static_cast<std::size_t>(total), 0);
    std::vector<std::ptrdiff_t> open;
    open.reserve(totalLabelCount);
    open.push_back(seed);
    visited[static_cast<std::size_t>(seed)] = 1;

    std::size_t visitedLabelCount = 0;
    const std::array<std::ptrdiff_t, 6> offsetX{{1, -1, 0, 0, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetY{{0, 0, 1, -1, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetZ{{0, 0, 0, 0, 1, -1}};

    for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
        const std::ptrdiff_t current = open[queueIndex];
        ++visitedLabelCount;

        const std::ptrdiff_t z = current / planeXY;
        const std::ptrdiff_t remainder = current % planeXY;
        const std::ptrdiff_t y = remainder / dimX;
        const std::ptrdiff_t x = remainder % dimX;

        for (std::size_t direction = 0; direction < offsetX.size(); ++direction) {
            const std::ptrdiff_t nx = x + offsetX[direction];
            const std::ptrdiff_t ny = y + offsetY[direction];
            const std::ptrdiff_t nz = z + offsetZ[direction];
            if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ) {
                continue;
            }

            const std::ptrdiff_t neighbor = nx + ny * dimX + nz * planeXY;
            if (visited[static_cast<std::size_t>(neighbor)] != 0 || buffer[neighbor] != label) {
                continue;
            }
            visited[static_cast<std::size_t>(neighbor)] = 1;
            open.push_back(neighbor);
        }
    }

    return visitedLabelCount == totalLabelCount;
}

bool voxelListIsConnected(const std::vector<Voxel> &voxels, const Roi &roi) {
    if (voxels.empty()) {
        return false;
    }

    const int sizeX = roi.maxX - roi.minX + 1;
    const int sizeY = roi.maxY - roi.minY + 1;
    const int sizeZ = roi.maxZ - roi.minZ + 1;
    const std::size_t planeXY = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY);
    const std::size_t total = planeXY * static_cast<std::size_t>(sizeZ);

    auto linearIndex = [&](const Voxel &voxel) {
        return static_cast<std::size_t>(voxel.z - roi.minZ) * planeXY +
               static_cast<std::size_t>(voxel.y - roi.minY) * static_cast<std::size_t>(sizeX) +
               static_cast<std::size_t>(voxel.x - roi.minX);
    };

    std::vector<unsigned char> occupied(total, 0);
    for (const Voxel &voxel : voxels) {
        occupied[linearIndex(voxel)] = 1;
    }

    std::vector<unsigned char> visited(total, 0);
    std::vector<Voxel> open;
    open.reserve(voxels.size());
    open.push_back(voxels.front());
    visited[linearIndex(voxels.front())] = 1;

    std::size_t visitedCount = 0;
    for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
        const Voxel activeVoxel = open[queueIndex];
        ++visitedCount;

        const std::array<Voxel, 6> neighbors{{
            Voxel(activeVoxel.x + 1, activeVoxel.y, activeVoxel.z),
            Voxel(activeVoxel.x - 1, activeVoxel.y, activeVoxel.z),
            Voxel(activeVoxel.x, activeVoxel.y + 1, activeVoxel.z),
            Voxel(activeVoxel.x, activeVoxel.y - 1, activeVoxel.z),
            Voxel(activeVoxel.x, activeVoxel.y, activeVoxel.z + 1),
            Voxel(activeVoxel.x, activeVoxel.y, activeVoxel.z - 1)
        }};

        for (const Voxel &neighbor : neighbors) {
            if (neighbor.x < roi.minX || neighbor.x > roi.maxX ||
                neighbor.y < roi.minY || neighbor.y > roi.maxY ||
                neighbor.z < roi.minZ || neighbor.z > roi.maxZ) {
                continue;
            }

            const std::size_t neighborIndex = linearIndex(neighbor);
            if (occupied[neighborIndex] == 0 || visited[neighborIndex] != 0) {
                continue;
            }
            visited[neighborIndex] = 1;
            open.push_back(neighbor);
        }
    }

    return visitedCount == voxels.size();
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SegmentPuzzler"));

    ProgramOptions options;
    if (!parseArgs(argc, argv, options)) {
        return 1;
    }

    if (!QFileInfo::exists(options.segmentsPath)) {
        std::cerr << "Sample segments file not found: " << options.segmentsPath.toStdString() << "\n";
        std::cerr << "Expected cached sample data in the temp directory. Run the app sample download first,\n";
        std::cerr << "or pass --segments /path/to/Watershed_MC.tif\n";
        return 1;
    }

    QString segmentsPath = options.segmentsPath;
    auto originalSegmentsImage = ITKImageLoader<dataType::SegmentIdType>(segmentsPath);
    if (originalSegmentsImage.IsNull()) {
        std::cerr << "Failed to load segments image.\n";
        return 1;
    }

    auto referenceFixture = sample::buildGraphFixture(originalSegmentsImage);
    const sample::LargestSegmentInfo largestSegment = sample::findLargestWorkingSegment(*referenceFixture.graph);
    if (largestSegment.label == 0 || largestSegment.voxelCount == 0) {
        std::cerr << "No working segment was found in the sample data.\n";
        return 1;
    }

    const auto candidates = sample::buildCandidateCuts(
        largestSegment,
        originalSegmentsImage,
        options.viewportSize,
        options.strokeWidthPixels);
    if (candidates.empty()) {
        std::cerr << "No cut candidates were generated.\n";
        return 1;
    }

    {
        auto failedFixture = sample::buildGraphFixture(originalSegmentsImage);
        auto failedWorkingBefore =
            sample::duplicateSegmentsImage(failedFixture.graphBase->pWorkingSegmentsImage);
        auto failedSelected =
            sample::duplicateSegmentsImage(failedFixture.graphBase->pWorkingSegmentsImage);
        failedSelected->FillBuffer(failedFixture.graph->backgroundId);
        failedFixture.graphBase->pSelectedSegmentation = failedSelected;
        failedFixture.graphBase->selectedSegmentationMaxSegmentId =
            failedFixture.graph->getLargestIdInSegmentVolume(failedSelected);
        auto failedSelectedBefore = sample::duplicateSegmentsImage(failedSelected);
        const SegmentIdType failedSelectedMaxBefore =
            failedFixture.graphBase->selectedSegmentationMaxSegmentId;

        Projected3DCutRequest unsuccessfulRequest = candidates.front().request;
        unsuccessfulRequest.strokePixels = {{-100.0, -100.0}, {-50.0, -50.0}};
        std::vector<SegmentIdType> unsuccessfulLabels{largestSegment.label};
        if (failedFixture.graph->splitWorkingNodeByProjected3DCut(
                unsuccessfulRequest, nullptr, &unsuccessfulLabels) ||
            !unsuccessfulLabels.empty() ||
            !imagesEqual(failedWorkingBefore, failedFixture.graphBase->pWorkingSegmentsImage) ||
            !imagesEqual(failedSelectedBefore, failedSelected) ||
            failedFixture.graphBase->selectedSegmentationMaxSegmentId != failedSelectedMaxBefore) {
            return failTest("An unsuccessful projected cut should leave both segmentations and labels unchanged.");
        }
    }

    sample::GraphFixture successfulFixture;
    dataType::SegmentsImageType::Pointer beforeCutImage;
    sample::CandidateCut chosenCandidate;
    std::vector<SegmentIdType> cutResultWorkingLabels;
    bool foundSuccessfulCut = false;

    for (const auto &candidate : candidates) {
        auto candidateFixture = sample::buildGraphFixture(originalSegmentsImage);
        auto candidateBeforeImage = sample::duplicateSegmentsImage(candidateFixture.graphBase->pWorkingSegmentsImage);
        std::vector<SegmentIdType> candidateResultWorkingLabels;
        if (!candidateFixture.graph->splitWorkingNodeByProjected3DCut(
                candidate.request, nullptr, &candidateResultWorkingLabels)) {
            continue;
        }

        successfulFixture = std::move(candidateFixture);
        beforeCutImage = candidateBeforeImage;
        chosenCandidate = candidate;
        cutResultWorkingLabels = std::move(candidateResultWorkingLabels);
        foundSuccessfulCut = true;
        break;
    }

    if (!foundSuccessfulCut) {
        std::cerr << "No diagonal candidate split the largest sample segment.\n";
        return 1;
    }

    const auto postCutImage = successfulFixture.graphBase->pWorkingSegmentsImage;
    const SegmentIdType backgroundId = successfulFixture.graph->backgroundId;
    if (countNonBackgroundVoxels(beforeCutImage, backgroundId) !=
        countNonBackgroundVoxels(postCutImage, backgroundId)) {
        return failTest("Projected cut changed the total non-background voxel count.");
    }

    const auto voxelCount = beforeCutImage->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *beforeBuffer = beforeCutImage->GetBufferPointer();
    const auto *afterBuffer = postCutImage->GetBufferPointer();
    std::size_t changedVoxelCount = 0;
    std::unordered_set<SegmentIdType> resultingWorkingLabels;
    resultingWorkingLabels.reserve(8);

    for (std::size_t index = 0; index < voxelCount; ++index) {
        const SegmentIdType beforeLabel = beforeBuffer[index];
        const SegmentIdType afterLabel = afterBuffer[index];
        if (beforeLabel == largestSegment.label) {
            if (afterLabel == backgroundId) {
                return failTest("Projected cut left background holes inside the original target segment.");
            }
            resultingWorkingLabels.insert(afterLabel);
        }
        if (beforeLabel == afterLabel) {
            continue;
        }
        ++changedVoxelCount;
        if (beforeLabel != largestSegment.label) {
            return failTest("Projected cut changed a voxel outside the original target working segment.");
        }
    }

    if (changedVoxelCount == 0) {
        return failTest("Projected cut reported success but did not change any voxel labels.");
    }
    if (resultingWorkingLabels.size() < 2) {
        return failTest("Projected cut did not produce multiple working-segment labels.");
    }
    if (resultingWorkingLabels.count(largestSegment.label) == 0) {
        return failTest("Projected cut did not reuse the original target working label.");
    }
    if (cutResultWorkingLabels.size() != resultingWorkingLabels.size() ||
        cutResultWorkingLabels.empty() ||
        cutResultWorkingLabels.front() != largestSegment.label) {
        return failTest("Projected cut did not report all result labels with the preserved target first.");
    }
    for (const SegmentIdType workingLabel : cutResultWorkingLabels) {
        if (resultingWorkingLabels.count(workingLabel) == 0) {
            return failTest("Projected cut reported a label outside the split target.");
        }
    }

    std::unordered_set<SegmentIdType> seenInitialLabels;
    seenInitialLabels.reserve(16);
    for (const SegmentIdType workingLabel : resultingWorkingLabels) {
        if (successfulFixture.graph->workingNodes.count(workingLabel) == 0) {
            return failTest("Resulting working label is missing from the graph.");
        }
        if (!labelIsConnectedInImage(postCutImage, workingLabel)) {
            return failTest("A resulting working segment is not 6-connected in the working image.");
        }

        const auto &workingNode = successfulFixture.graph->workingNodes.at(workingLabel);
        for (const auto &initialEntry : workingNode->subInitialNodes) {
            const auto inserted = seenInitialLabels.insert(initialEntry.first);
            if (!inserted.second) {
                return failTest("A resulting initial node was attached to multiple working nodes.");
            }

            const auto &initialNode = initialEntry.second;
            if (initialNode == nullptr) {
                return failTest("A resulting working node contains a null initial-node pointer.");
            }
            if (initialNode->getCurrentWorkingNodeLabel() != workingLabel) {
                return failTest("A resulting initial node reports the wrong current working label.");
            }
            if (!voxelListIsConnected(initialNode->voxels, initialNode->roi)) {
                return failTest("A resulting initial node is not 6-connected.");
            }

            SegmentIdType imageLabel = backgroundId;
            for (const Voxel &voxel : initialNode->voxels) {
                const SegmentIdType voxelLabel = postCutImage->GetPixel({voxel.x, voxel.y, voxel.z});
                if (imageLabel == backgroundId) {
                    imageLabel = voxelLabel;
                } else if (voxelLabel != imageLabel) {
                    return failTest("A resulting initial node spans multiple working-image labels.");
                }
            }
            if (imageLabel != workingLabel) {
                return failTest("A resulting initial node does not map back to its owning working label.");
            }
        }
    }

    const auto selectedMaxBeforeMissingTransfer = successfulFixture.graphBase->selectedSegmentationMaxSegmentId;
    if (!successfulFixture.graph->transferWorkingNodesToSegmentation(cutResultWorkingLabels).empty() ||
        successfulFixture.graphBase->selectedSegmentationMaxSegmentId != selectedMaxBeforeMissingTransfer) {
        return failTest("Batch transfer without a selected segmentation should fail without consuming labels.");
    }

    auto selectedSegmentation = sample::duplicateSegmentsImage(postCutImage);
    selectedSegmentation->FillBuffer(backgroundId);
    successfulFixture.graphBase->pSelectedSegmentation = selectedSegmentation;
    successfulFixture.graphBase->selectedSegmentationMaxSegmentId =
        successfulFixture.graph->getLargestIdInSegmentVolume(selectedSegmentation);
    const SegmentIdType selectedMaxBeforeTransfer =
        successfulFixture.graphBase->selectedSegmentationMaxSegmentId;

    const auto assignedSegmentationLabels =
        successfulFixture.graph->transferWorkingNodesToSegmentation(cutResultWorkingLabels);
    if (assignedSegmentationLabels.size() != cutResultWorkingLabels.size()) {
        return failTest("Batch transfer did not assign one selected-segmentation label per cut result.");
    }
    for (std::size_t resultIndex = 0; resultIndex < cutResultWorkingLabels.size(); ++resultIndex) {
        const SegmentIdType expectedSegmentationLabel =
            static_cast<SegmentIdType>(selectedMaxBeforeTransfer + resultIndex + 1);
        if (assignedSegmentationLabels[resultIndex] != expectedSegmentationLabel) {
            return failTest("Batch transfer did not assign consecutive selected-segmentation labels.");
        }

        const auto &workingNode = successfulFixture.graph->workingNodes.at(cutResultWorkingLabels[resultIndex]);
        for (const auto &initialEntry : workingNode->subInitialNodes) {
            for (const Voxel &voxel : initialEntry.second->voxels) {
                if (selectedSegmentation->GetPixel({voxel.x, voxel.y, voxel.z}) != expectedSegmentationLabel) {
                    return failTest("Batch transfer did not copy a cut-result voxel into the selected segmentation.");
                }
            }
        }
    }
    if (successfulFixture.graphBase->selectedSegmentationMaxSegmentId != assignedSegmentationLabels.back()) {
        return failTest("Batch transfer did not update the selected-segmentation maximum label.");
    }
    if (successfulFixture.graphBase->selectedSegmentationMaxSegmentId !=
        static_cast<SegmentIdType>(selectedMaxBeforeTransfer + assignedSegmentationLabels.size())) {
        return failTest("Batch transfer did not increase the selected-segmentation maximum by the result count.");
    }

    const std::unordered_set<SegmentIdType> distinctAssignedLabels(
        assignedSegmentationLabels.begin(), assignedSegmentationLabels.end());
    if (distinctAssignedLabels.size() != assignedSegmentationLabels.size()) {
        return failTest("Cut results were not transferred under distinct selected-segmentation labels.");
    }

    std::unordered_map<SegmentIdType, SegmentIdType> selectedLabelByWorkingLabel;
    selectedLabelByWorkingLabel.reserve(cutResultWorkingLabels.size());
    for (std::size_t resultIndex = 0; resultIndex < cutResultWorkingLabels.size(); ++resultIndex) {
        selectedLabelByWorkingLabel.emplace(
            cutResultWorkingLabels[resultIndex], assignedSegmentationLabels[resultIndex]);
    }

    std::unordered_map<SegmentIdType, std::size_t> transferredVoxelCounts;
    transferredVoxelCounts.reserve(assignedSegmentationLabels.size());
    const auto *selectedBuffer = selectedSegmentation->GetBufferPointer();
    for (std::size_t voxelIndex = 0; voxelIndex < voxelCount; ++voxelIndex) {
        if (beforeBuffer[voxelIndex] != largestSegment.label) {
            if (selectedBuffer[voxelIndex] != backgroundId) {
                return failTest("Batch transfer changed a selected-segmentation voxel outside the cut target.");
            }
            continue;
        }

        const auto expectedLabelIt = selectedLabelByWorkingLabel.find(afterBuffer[voxelIndex]);
        if (expectedLabelIt == selectedLabelByWorkingLabel.end() ||
            selectedBuffer[voxelIndex] != expectedLabelIt->second) {
            return failTest("Batch transfer did not completely cover every projected-cut result voxel.");
        }
        ++transferredVoxelCounts[expectedLabelIt->second];
    }
    for (const SegmentIdType assignedLabel : assignedSegmentationLabels) {
        if (transferredVoxelCounts[assignedLabel] == 0) {
            return failTest("A projected-cut result received no voxels in the selected segmentation.");
        }
    }

    SegmentIdType missingWorkingLabel = std::numeric_limits<SegmentIdType>::max();
    while (successfulFixture.graph->workingNodes.count(missingWorkingLabel) > 0) {
        --missingWorkingLabel;
    }
    const SegmentIdType selectedMaxBeforeInvalidBatch =
        successfulFixture.graphBase->selectedSegmentationMaxSegmentId;
    const auto &firstTransferredNode =
        successfulFixture.graph->workingNodes.at(cutResultWorkingLabels.front());
    const Voxel firstTransferredVoxel = firstTransferredNode->subInitialNodes.begin()->second->voxels.front();
    const SegmentIdType firstTransferredVoxelLabelBefore =
        selectedSegmentation->GetPixel({firstTransferredVoxel.x, firstTransferredVoxel.y, firstTransferredVoxel.z});
    if (!successfulFixture.graph->transferWorkingNodesToSegmentation(
            {cutResultWorkingLabels.front(), missingWorkingLabel}).empty() ||
        successfulFixture.graphBase->selectedSegmentationMaxSegmentId != selectedMaxBeforeInvalidBatch ||
        selectedSegmentation->GetPixel(
            {firstTransferredVoxel.x, firstTransferredVoxel.y, firstTransferredVoxel.z}) !=
            firstTransferredVoxelLabelBefore) {
        return failTest("An invalid batch should fail before mutating labels or selected-segmentation voxels.");
    }
    if (!successfulFixture.graph->transferWorkingNodesToSegmentation(
            {cutResultWorkingLabels.front(), cutResultWorkingLabels.front()}).empty() ||
        successfulFixture.graphBase->selectedSegmentationMaxSegmentId != selectedMaxBeforeInvalidBatch) {
        return failTest("A duplicate-label batch should fail without consuming selected-segmentation labels.");
    }

    std::cout << "Projected 3D cut test passed.\n";
    std::cout << "Chosen candidate: " << chosenCandidate.name.toStdString() << "\n";
    std::cout << "Largest segment label: " << largestSegment.label
              << " voxels=" << largestSegment.voxelCount << "\n";
    std::cout << "Changed voxels: " << changedVoxelCount << "\n";
    std::cout << "Resulting working labels:";
    for (const SegmentIdType workingLabel : resultingWorkingLabels) {
        std::cout << " " << workingLabel;
    }
    std::cout << "\n";

    return 0;
}
