#include <QCoreApplication>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include "src/viewers/Segment3DViewerDialog.h"
#include "projected_3d_cut_sample_utils.h"

namespace {

using Clock = std::chrono::steady_clock;
namespace sample = projected3dcutsample;

struct ProgramOptions {
    QString segmentsPath;
    int repeats = 1;
    int viewportSize = 1024;
    double strokeWidthPixels = 6.0;
};

double elapsedMs(const Clock::time_point &start, const Clock::time_point &end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void printUsage(const char *argv0) {
    std::cout << "Usage: " << argv0
              << " [--segments PATH] [--repeats N] [--viewport N] [--stroke-width PX]\n";
}

bool parseArgs(int argc, char **argv, ProgramOptions &options) {
    for (int index = 1; index < argc; ++index) {
        const QString arg = QString::fromLocal8Bit(argv[index]);
        if (arg == "--segments" && index + 1 < argc) {
            options.segmentsPath = QString::fromLocal8Bit(argv[++index]);
        } else if (arg == "--repeats" && index + 1 < argc) {
            options.repeats = std::max(1, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (arg == "--viewport" && index + 1 < argc) {
            options.viewportSize = std::max(128, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (arg == "--stroke-width" && index + 1 < argc) {
            options.strokeWidthPixels = std::max(1.0, QString::fromLocal8Bit(argv[++index]).toDouble());
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg.toStdString() << "\n";
            printUsage(argv[0]);
            return false;
        }
    }

    if (options.segmentsPath.isEmpty()) {
        options.segmentsPath = sample::defaultSampleSegmentsPath();
    }
    return true;
}

Projected3DCutProfile averageProfiles(const std::vector<Projected3DCutProfile> &profiles) {
    Projected3DCutProfile average;
    if (profiles.empty()) {
        return average;
    }

    average = profiles.front();
    const double count = static_cast<double>(profiles.size());
    average.collectTargetVoxelsMs = 0.0;
    average.rasterizeStrokeMaskMs = 0.0;
    average.projectAndClassifyTargetVoxelsMs = 0.0;
    average.connectedComponentsMs = 0.0;
    average.reassignCutVoxelsMs = 0.0;
    average.splitReplacementInitialsMs = 0.0;
    average.collectNeighborGroupsMs = 0.0;
    average.splitWorkingNodesMs = 0.0;
    average.removeOriginalNodesMs = 0.0;
    average.clearTargetRegionMs = 0.0;
    average.createReplacementInitialsMs = 0.0;
    average.materializeReplacementVoxelListsMs = 0.0;
    average.recomputeInitialEdgesMs = 0.0;
    average.rebuildWorkingNodesMs = 0.0;
    average.recalculateWorkingEdgesMs = 0.0;
    average.rewriteWorkingImageMs = 0.0;
    average.totalMs = 0.0;

    for (const auto &profile : profiles) {
        average.collectTargetVoxelsMs += profile.collectTargetVoxelsMs;
        average.rasterizeStrokeMaskMs += profile.rasterizeStrokeMaskMs;
        average.projectAndClassifyTargetVoxelsMs += profile.projectAndClassifyTargetVoxelsMs;
        average.connectedComponentsMs += profile.connectedComponentsMs;
        average.reassignCutVoxelsMs += profile.reassignCutVoxelsMs;
        average.splitReplacementInitialsMs += profile.splitReplacementInitialsMs;
        average.collectNeighborGroupsMs += profile.collectNeighborGroupsMs;
        average.splitWorkingNodesMs += profile.splitWorkingNodesMs;
        average.removeOriginalNodesMs += profile.removeOriginalNodesMs;
        average.clearTargetRegionMs += profile.clearTargetRegionMs;
        average.createReplacementInitialsMs += profile.createReplacementInitialsMs;
        average.materializeReplacementVoxelListsMs += profile.materializeReplacementVoxelListsMs;
        average.recomputeInitialEdgesMs += profile.recomputeInitialEdgesMs;
        average.rebuildWorkingNodesMs += profile.rebuildWorkingNodesMs;
        average.recalculateWorkingEdgesMs += profile.recalculateWorkingEdgesMs;
        average.rewriteWorkingImageMs += profile.rewriteWorkingImageMs;
        average.totalMs += profile.totalMs;
    }

    average.collectTargetVoxelsMs /= count;
    average.rasterizeStrokeMaskMs /= count;
    average.projectAndClassifyTargetVoxelsMs /= count;
    average.connectedComponentsMs /= count;
    average.reassignCutVoxelsMs /= count;
    average.splitReplacementInitialsMs /= count;
    average.collectNeighborGroupsMs /= count;
    average.splitWorkingNodesMs /= count;
    average.removeOriginalNodesMs /= count;
    average.clearTargetRegionMs /= count;
    average.createReplacementInitialsMs /= count;
    average.materializeReplacementVoxelListsMs /= count;
    average.recomputeInitialEdgesMs /= count;
    average.rebuildWorkingNodesMs /= count;
    average.recalculateWorkingEdgesMs /= count;
    average.rewriteWorkingImageMs /= count;
    average.totalMs /= count;
    return average;
}

std::vector<std::pair<std::string, double>> cutStageTimings(const Projected3DCutProfile &profile) {
    return {
        {"project and classify target voxels", profile.projectAndClassifyTargetVoxelsMs},
        {"rasterize stroke mask", profile.rasterizeStrokeMaskMs},
        {"connected components", profile.connectedComponentsMs},
        {"reassign cut voxels", profile.reassignCutVoxelsMs},
        {"split replacement initials", profile.splitReplacementInitialsMs},
        {"collect neighbor groups", profile.collectNeighborGroupsMs},
        {"split working nodes", profile.splitWorkingNodesMs},
        {"remove original nodes", profile.removeOriginalNodesMs},
        {"clear target region", profile.clearTargetRegionMs},
        {"create replacement initials", profile.createReplacementInitialsMs},
        {"materialize replacement voxel lists", profile.materializeReplacementVoxelListsMs},
        {"recompute initial edges", profile.recomputeInitialEdgesMs},
        {"rebuild working nodes", profile.rebuildWorkingNodesMs},
        {"recalculate working edges", profile.recalculateWorkingEdgesMs},
        {"rewrite working image", profile.rewriteWorkingImageMs}
    };
}

void printCutStageSummary(const Projected3DCutProfile &profile) {
    auto stages = cutStageTimings(profile);
    std::sort(stages.begin(), stages.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.second > rhs.second;
    });

    std::cout << "\nProjected cut profile (ms)\n";
    std::cout << "  target voxels: " << profile.targetVoxelCount << "\n";
    std::cout << "  provisional cut voxels: " << profile.provisionalCutVoxelCount << "\n";
    std::cout << "  final components: " << profile.finalComponentCount << "\n";
    std::cout << "  replacement initials: " << profile.replacementInitialCount << "\n";
    std::cout << "  cut total: " << profile.totalMs << "\n";
    std::cout << "  collect target voxels total: " << profile.collectTargetVoxelsMs << "\n";

    for (const auto &[name, milliseconds] : stages) {
        const double percent = profile.totalMs > 1e-9 ? (100.0 * milliseconds / profile.totalMs) : 0.0;
        std::cout << "  " << name << ": " << milliseconds << " ms (" << percent << "%)\n";
    }

    if (!stages.empty()) {
        std::cout << "  slowest cut stage: " << stages.front().first
                  << " (" << stages.front().second << " ms)\n";
    }
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
    const auto loadStart = Clock::now();
    auto originalSegmentsImage = ITKImageLoader<dataType::SegmentIdType>(segmentsPath);
    const double loadSegmentsMs = elapsedMs(loadStart, Clock::now());
    if (originalSegmentsImage.IsNull()) {
        std::cerr << "Failed to load segments image.\n";
        return 1;
    }

    const auto initialGraphBuildStart = Clock::now();
    auto initialFixture = sample::buildGraphFixture(originalSegmentsImage);
    const double initialGraphBuildMs = elapsedMs(initialGraphBuildStart, Clock::now());

    const sample::LargestSegmentInfo largestSegment = sample::findLargestWorkingSegment(*initialFixture.graph);
    if (largestSegment.label == 0 || largestSegment.voxelCount == 0) {
        std::cerr << "No working segment was found in the sample data.\n";
        return 1;
    }

    std::cout << "Loaded sample segments: " << segmentsPath.toStdString() << "\n";
    std::cout << "Load time: " << loadSegmentsMs << " ms\n";
    std::cout << "Initial graph build: " << initialGraphBuildMs << " ms\n";
    std::cout << "Largest working segment label: " << largestSegment.label
              << " voxels=" << largestSegment.voxelCount
              << " roi=[" << largestSegment.roi.minX << "," << largestSegment.roi.maxX
              << "]x[" << largestSegment.roi.minY << "," << largestSegment.roi.maxY
              << "]x[" << largestSegment.roi.minZ << "," << largestSegment.roi.maxZ << "]\n";

    const auto scenePrepareStart = Clock::now();
    auto preparedScene = Segment3DViewerDialog::prepareScene(
        initialFixture.graphBase->pWorkingSegmentsImage,
        {{largestSegment.label, 0xFFAAAAAAu}},
        largestSegment.roi);
    const double scenePrepareMs = elapsedMs(scenePrepareStart, Clock::now());

    vtkIdType scenePoints = 0;
    vtkIdType sceneCells = 0;
    if (!preparedScene.meshes.empty() && preparedScene.meshes.front().polyData != nullptr) {
        scenePoints = preparedScene.meshes.front().polyData->GetNumberOfPoints();
        sceneCells = preparedScene.meshes.front().polyData->GetNumberOfCells();
    }
    std::cout << "3D scene prepare: " << scenePrepareMs << " ms"
              << " points=" << scenePoints
              << " cells=" << sceneCells << "\n";

    const auto candidates = sample::buildCandidateCuts(
        largestSegment,
        originalSegmentsImage,
        options.viewportSize,
        options.strokeWidthPixels);
    if (candidates.empty()) {
        std::cerr << "No cut candidates were generated.\n";
        return 1;
    }

    sample::CandidateCut chosenCandidate;
    std::vector<Projected3DCutProfile> successfulProfiles;
    bool foundSuccessfulCut = false;

    for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        sample::GraphFixture *fixture = nullptr;
        if (candidateIndex == 0) {
            fixture = &initialFixture;
        } else {
            initialFixture = sample::buildGraphFixture(originalSegmentsImage);
            fixture = &initialFixture;
        }

        Projected3DCutProfile profile;
        const bool mutated = fixture->graph->splitWorkingNodeByProjected3DCut(candidates[candidateIndex].request, &profile);
        std::cout << "Candidate " << candidates[candidateIndex].name.toStdString()
                  << " -> " << (mutated ? "split" : "no split")
                  << " total=" << profile.totalMs << " ms\n";
        if (!mutated) {
            continue;
        }

        chosenCandidate = candidates[candidateIndex];
        successfulProfiles.push_back(profile);
        foundSuccessfulCut = true;
        break;
    }

    if (!foundSuccessfulCut) {
        std::cerr << "No diagonal candidate split the largest sample segment.\n";
        return 1;
    }

    for (int repetition = 1; repetition < options.repeats; ++repetition) {
        auto repeatFixture = sample::buildGraphFixture(originalSegmentsImage);
        Projected3DCutProfile repeatProfile;
        const bool mutated = repeatFixture.graph->splitWorkingNodeByProjected3DCut(chosenCandidate.request, &repeatProfile);
        if (!mutated) {
            std::cerr << "Chosen candidate failed on repetition " << repetition + 1 << ".\n";
            return 1;
        }
        successfulProfiles.push_back(repeatProfile);
    }

    const Projected3DCutProfile averageProfile = averageProfiles(successfulProfiles);

    std::cout << "\nChosen candidate: " << chosenCandidate.name.toStdString()
              << " repeats=" << successfulProfiles.size() << "\n";
    printCutStageSummary(averageProfile);

    std::vector<std::pair<std::string, double>> overallStages{
        {"load sample segments", loadSegmentsMs},
        {"build graph", initialGraphBuildMs},
        {"prepare 3D scene", scenePrepareMs},
        {"projected cut total", averageProfile.totalMs}
    };
    std::sort(overallStages.begin(), overallStages.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.second > rhs.second;
    });

    std::cout << "\nOverall stage summary (ms)\n";
    for (const auto &[name, milliseconds] : overallStages) {
        std::cout << "  " << name << ": " << milliseconds << "\n";
    }
    if (!overallStages.empty()) {
        std::cout << "Overall slowest stage: " << overallStages.front().first
                  << " (" << overallStages.front().second << " ms)\n";
    }

    return 0;
}
