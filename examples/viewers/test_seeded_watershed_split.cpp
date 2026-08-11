#include "src/segment_handling/SeededWatershedSplit.h"

#include <itkImageFileReader.h>
#include <itkImageRegionConstIterator.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using Image = dataType::SegmentsImageType;
using Index = Image::IndexType;

void fillBox(Image *image, const Index &from, const Index &to) {
    for (int z = from[2]; z <= to[2]; ++z) {
        for (int y = from[1]; y <= to[1]; ++y) {
            for (int x = from[0]; x <= to[0]; ++x) {
                image->SetPixel({x, y, z}, 1);
            }
        }
    }
}

int fail(const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 9) {
        using Reader = itk::ImageFileReader<Image>;
        auto reader = Reader::New();
        reader->SetFileName(argv[1]);
        reader->Update();
        const auto label = static_cast<dataType::SegmentIdType>(std::stoul(argv[2]));
        const auto session = segment_puzzler::prepareSeededWatershedSplit(
            reader->GetOutput(), label);
        std::array<Index, 2> seeds;
        for (std::size_t seed = 0; seed < seeds.size(); ++seed) {
            for (unsigned int axis = 0; axis < 3; ++axis) {
                seeds[seed][axis] = std::stoll(argv[3 + 3 * seed + axis]);
            }
        }
        const auto result = segment_puzzler::computeSeededWatershedSplit(session, seeds);
        if (!result.valid()) {
            std::cerr << "FAIL: " << result.error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "PASS: label " << label << " seeded watershed split "
                  << result.voxelCounts[0] << " + " << result.voxelCounts[1]
                  << " = " << session.voxelCount << " voxels\n";
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        return fail("expected either no arguments or: image label x1 y1 z1 x2 y2 z2");
    }

    Image::SizeType size{{50, 24, 16}};
    Image::IndexType start;
    start.Fill(0);
    auto image = Image::New();
    image->SetRegions({start, size});
    image->Allocate();
    image->FillBuffer(0);

    fillBox(image, {4, 5, 4}, {19, 18, 11});
    fillBox(image, {30, 5, 4}, {45, 18, 11});
    fillBox(image, {20, 10, 7}, {29, 13, 8});

    auto session = segment_puzzler::prepareSeededWatershedSplit(image, 1);
    if (session.connectedComponentCount != 1 || session.voxelCount == 0) {
        return fail("synthetic dumbbell should be one connected component");
    }
    const auto smoothedLandscapeHash = session.landscapeHash;
    segment_puzzler::updateSeededWatershedSplitLandscape(session, 0.0);
    if (session.landscapeSmoothingSigmaPixels != 0.0
        || session.landscapeHash == smoothedLandscapeHash) {
        return fail("smoothing slider should update the watershed landscape");
    }
    segment_puzzler::updateSeededWatershedSplitLandscape(
        session, segment_puzzler::kDefaultSeededSplitSmoothingSigmaPixels);
    if (session.landscapeHash != smoothedLandscapeHash) {
        return fail("restoring the default smoothing should reproduce the landscape");
    }

    const auto leftSeed = segment_puzzler::seededSplitMaximumAlongWorldRay(
        session, {10.0, 11.0, -10.0}, {10.0, 11.0, 30.0});
    const auto rightSeed = segment_puzzler::seededSplitMaximumAlongWorldRay(
        session, {38.0, 11.0, -10.0}, {38.0, 11.0, 30.0});
    if (!leftSeed.has_value() || !rightSeed.has_value() || leftSeed == rightSeed) {
        return fail("could not place two distinct seeds");
    }
    const auto leftSeedWorld =
        segment_puzzler::seededSplitSeedWorldPoint(session, leftSeed.value());
    const auto rightSeedWorld =
        segment_puzzler::seededSplitSeedWorldPoint(session, rightSeed.value());
    if (leftSeedWorld[0] != 10.0 || leftSeedWorld[1] != 11.0
        || rightSeedWorld[0] != 38.0 || rightSeedWorld[1] != 11.0) {
        return fail("seed should remain on its viewing ray");
    }

    const auto result = segment_puzzler::computeSeededWatershedSplit(
        session, {leftSeed.value(), rightSeed.value()});
    if (!result.valid()) {
        std::cerr << "FAIL: " << result.error << '\n';
        return EXIT_FAILURE;
    }
    if (result.markers.IsNull()) {
        return fail("watershed markers should be retained for debugging");
    }
    if (result.floodMetrics.finalizedVoxelCount != session.voxelCount) {
        return fail("masked flood should finalize exactly the segment voxels");
    }
    if (result.voxelCounts[0] == 0 || result.voxelCounts[1] == 0
        || result.voxelCounts[0] + result.voxelCounts[1] != session.voxelCount) {
        return fail("watershed partition should be non-empty and exhaustive");
    }

    auto extraSeed = [&](Index seed, int direction) {
        seed[0] += direction;
        return seed;
    };
    segment_puzzler::SeededSplitSeedGroups seedGroups{{
        {leftSeed.value(), extraSeed(leftSeed.value(), 1)},
        {rightSeed.value(), extraSeed(rightSeed.value(), -1)}}};
    const auto multiSeedResult =
        segment_puzzler::computeSeededWatershedSplit(session, seedGroups);
    if (!multiSeedResult.valid()
        || multiSeedResult.markerVoxelCounts != std::array<std::size_t, 2>{2, 2}) {
        return fail("watershed should accept multiple seeds in each marker class");
    }

    const auto middleSeed = segment_puzzler::seededSplitMaximumAlongWorldRay(
        session, {25.0, 11.0, -10.0}, {25.0, 11.0, 30.0});
    if (!middleSeed.has_value()) {
        return fail("could not place a separator seed in the synthetic bridge");
    }
    const segment_puzzler::SeededSplitSeedGroups disconnectedSeedGroups{{
        {leftSeed.value(), rightSeed.value()}, {middleSeed.value()}}};
    segment_puzzler::SeededWatershedSplitOptions disconnectedOptions;
    const auto rejectedDisconnectedResult =
        segment_puzzler::computeSeededWatershedSplit(
            session,
            disconnectedSeedGroups,
            disconnectedOptions);
    if (rejectedDisconnectedResult.valid()
        || !rejectedDisconnectedResult.hasDisconnectedParts()) {
        return fail("disconnected watershed parts should be rejected by default");
    }
    disconnectedOptions.allowDisconnectedParts = true;
    const auto allowedDisconnectedResult =
        segment_puzzler::computeSeededWatershedSplit(
            session,
            disconnectedSeedGroups,
            disconnectedOptions);
    if (!allowedDisconnectedResult.valid()
        || !allowedDisconnectedResult.hasDisconnectedParts()) {
        return fail("disconnected watershed parts should be accepted when requested");
    }

    Index upperLeft = leftSeed.value();
    Index lowerLeft = leftSeed.value();
    Index upperRight = rightSeed.value();
    Index lowerRight = rightSeed.value();
    upperLeft[1] -= 4;
    lowerLeft[1] += 4;
    upperRight[1] -= 4;
    lowerRight[1] += 4;
    const segment_puzzler::SeededSplitSeedGroups separatedSeedGroups{{
        {upperLeft, lowerLeft}, {upperRight, lowerRight}}};
    segment_puzzler::SeededWatershedSplitOptions connectedOptions;
    connectedOptions.connectSeeds = true;
    const auto connectedSeedResult = segment_puzzler::computeSeededWatershedSplit(
        session, separatedSeedGroups, connectedOptions);
    if (!connectedSeedResult.valid()
        || connectedSeedResult.connectionVoxelCounts[0] == 0
        || connectedSeedResult.connectionVoxelCounts[1] == 0
        || connectedSeedResult.connectedComponentCounts
               != std::array<std::size_t, 2>{1, 1}) {
        return fail("connected seeds should produce two connected marker-controlled parts");
    }

    Image::SizeType lineSize{{15, 3, 3}};
    auto lineImage = Image::New();
    lineImage->SetRegions({start, lineSize});
    lineImage->Allocate();
    lineImage->FillBuffer(0);
    fillBox(lineImage, {1, 1, 1}, {13, 1, 1});
    const auto lineSession = segment_puzzler::prepareSeededWatershedSplit(lineImage, 1);
    const segment_puzzler::SeededSplitSeedGroups crossingSeedGroups{{
        {{2, 1, 1}, {12, 1, 1}}, {{7, 1, 1}}}};
    const auto crossingSeedResult = segment_puzzler::computeSeededWatershedSplit(
        lineSession, crossingSeedGroups, connectedOptions);
    if (crossingSeedResult.valid()
        || crossingSeedResult.error.find("overlap") == std::string::npos) {
        return fail("overlapping red and blue seed connections should be rejected");
    }

    Image::SizeType bridgeSize{{90, 30, 20}};
    auto bridgeImage = Image::New();
    bridgeImage->SetRegions({start, bridgeSize});
    bridgeImage->Allocate();
    bridgeImage->FillBuffer(0);
    fillBox(bridgeImage, {3, 4, 4}, {54, 25, 15});
    fillBox(bridgeImage, {55, 13, 8}, {60, 16, 11});
    fillBox(bridgeImage, {61, 4, 4}, {83, 25, 15});

    const auto bridgeSession = segment_puzzler::prepareSeededWatershedSplit(
        bridgeImage, 1);
    const auto largeLobeSeed = segment_puzzler::seededSplitMaximumAlongWorldRay(
        bridgeSession, {12.0, 15.0, -10.0}, {12.0, 15.0, 30.0});
    const auto smallLobeSeed = segment_puzzler::seededSplitMaximumAlongWorldRay(
        bridgeSession, {74.0, 15.0, -10.0}, {74.0, 15.0, 30.0});
    if (!largeLobeSeed.has_value() || !smallLobeSeed.has_value()) {
        return fail("could not place seeds in the thin-bridge object");
    }
    const auto bridgeResult = segment_puzzler::computeSeededWatershedSplit(
        bridgeSession, {largeLobeSeed.value(), smallLobeSeed.value()});
    if (!bridgeResult.valid()) {
        std::cerr << "FAIL: thin-bridge split: " << bridgeResult.error << '\n';
        return EXIT_FAILURE;
    }
    std::size_t wrongLargeLobeVoxels = 0;
    std::size_t wrongSmallLobeVoxels = 0;
    itk::ImageRegionConstIterator<Image> bridgePartitionIt(
        bridgeResult.partition, bridgeResult.partition->GetLargestPossibleRegion());
    for (bridgePartitionIt.GoToBegin(); !bridgePartitionIt.IsAtEnd(); ++bridgePartitionIt) {
        const auto globalX = bridgePartitionIt.GetIndex()[0]
                             + bridgeSession.globalOffset[0];
        wrongLargeLobeVoxels += globalX <= 54 && bridgePartitionIt.Get() == 2;
        wrongSmallLobeVoxels += globalX >= 61 && bridgePartitionIt.Get() == 1;
    }
    if (wrongLargeLobeVoxels != 0 || wrongSmallLobeVoxels != 0) {
        return fail("compact watershed should split the synthetic object at its thin bridge");
    }

    std::cout << "PASS: seeded watershed split " << result.voxelCounts[0]
              << " + " << result.voxelCounts[1] << " = " << session.voxelCount
              << " voxels\n";
    return EXIT_SUCCESS;
}
