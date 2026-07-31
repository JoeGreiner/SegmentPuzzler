#include <QCoreApplication>

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "src/segment_handling/Graph.h"
#include "src/segment_handling/feature.h"
#include "src/segment_handling/graphBase.h"

namespace {

using SegmentIdType = dataType::SegmentIdType;
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

void appendVoxels(std::ostringstream &out, const std::vector<Voxel> &input) {
    std::vector<Voxel> voxels = input;
    std::sort(voxels.begin(), voxels.end());
    for (const Voxel &voxel : voxels) {
        out << voxel.x << ',' << voxel.y << ',' << voxel.z << ';';
    }
}

void appendEdge(std::ostringstream &out, const InitialEdge &edge) {
    const Roi &roi = edge.getRoi();
    out << edge.numId << '|'
        << roi.minX << ',' << roi.minY << ',' << roi.minZ << ','
        << roi.maxX << ',' << roi.maxY << ',' << roi.maxZ << '|';
    appendVoxels(out, edge.voxels);
    out << '|';
    for (const auto &feature : edge.edgeFeatures) {
        out << feature->filterName << ':';
        for (float value : feature->values) {
            out << value << ',';
        }
        out << ';';
    }
}

std::string graphFingerprint(int threadCount) {
    auto graphBase = std::make_shared<GraphBase>();
    graphBase->pWorkingSegmentsImage = makeTestImage();
    auto graph = std::make_unique<Graph>(graphBase, false);
    graphBase->pGraph = graph.get();
    graph->setPointerToIgnoredSegmentLabels(&graphBase->ignoredSegmentLabels);
    graph->constructFromVolume(graphBase->pWorkingSegmentsImage, threadCount);

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

    for (const auto &entry : graph->initialOneSidedEdges) {
        out << "O" << entry.first.first << ',' << entry.first.second << ':';
        appendEdge(out, *entry.second);
        out << '\n';
    }
    for (const auto &entry : graph->initialTwoSidedEdges) {
        out << "T" << entry.first.first << ',' << entry.first.second << ':';
        appendEdge(out, *entry.second);
        out << '\n';
    }
    return out.str();
}

// This test checks result equivalence and repeatability. Actual OpenMP thread use is
// reported by the production scan log because the runtime may legally reduce a team.
int testRequestedParallelBuildMatchesSerial() {
    const std::string serial = graphFingerprint(1);
#ifdef USE_OMP
    constexpr int parallelThreadCount = 4;
#else
    constexpr int parallelThreadCount = 1;
#endif
    for (int repetition = 0; repetition < 5; ++repetition) {
        if (graphFingerprint(parallelThreadCount) != serial) {
            return failTest("Parallel graph construction differs from the serial result.");
        }
    }
    return 0;
}

int testFeatureEnabledBuildMatchesSerial() {
    FeatureList::edgeFeaturesList.emplace_back(std::make_unique<NumberOfVoxels>());
    const std::string serial = graphFingerprint(1);
    const std::string requestedParallel = graphFingerprint(4);
    FeatureList::edgeFeaturesList.clear();
    if (requestedParallel != serial) {
        return failTest("Edge-feature fallback differs from the serial result.");
    }
    return 0;
}

int testEmptyNodeProducesNoEdges() {
    auto graphBase = std::make_shared<GraphBase>();
    InitialNode node(graphBase, makeTestImage(), 41);
    node.computeOnesidedSurfaceAndEdges({0});
    if (!node.onesidedEdges.empty()) {
        return failTest("An empty initial node should not produce one-sided edges.");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    if (!FeatureList::edgeFeaturesList.empty()) {
        return failTest("Edge feature registry must be empty when the test starts.");
    }
    if (int result = testRequestedParallelBuildMatchesSerial()) {
        return result;
    }
    if (int result = testFeatureEnabledBuildMatchesSerial()) {
        return result;
    }
    if (int result = testEmptyNodeProducesNoEdges()) {
        return result;
    }

    std::cout << "Initial-edge result-equivalence tests passed.\n";
    return 0;
}
