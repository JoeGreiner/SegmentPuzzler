#include <QCoreApplication>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "src/viewers/itkSignal.h"

namespace {

int fail(const std::string &message) {
    std::cerr << "Assertion failed: " << message << '\n';
    return 1;
}

template<typename Pixel>
typename itk::Image<Pixel, 3>::Pointer makeImage(Pixel fillValue) {
    using Image = itk::Image<Pixel, 3>;
    auto image = Image::New();
    typename Image::IndexType start{};
    start.Fill(0);
    typename Image::SizeType size{{3, 3, 3}};
    typename Image::RegionType region(start, size);
    image->SetRegions(region);
    image->Allocate();
    image->FillBuffer(fillValue);
    return image;
}

template<typename Pixel>
int expectCenterColorOnAllAxes(itkSignal<Pixel> &signal,
                               QRgb expectedColor,
                               const std::string &context) {
    for (unsigned int axis = 0; axis < 3; ++axis) {
        std::vector<quint32> buffer;
        const QImage image = signal.calculateSliceQImage(1, axis, &buffer);
        if (image.isNull() || image.width() != 3 || image.height() != 3 || buffer.size() < 9) {
            return fail(context + ": invalid rendered image for axis " + std::to_string(axis));
        }
        if (buffer[4] != expectedColor) {
            return fail(context + ": center color mismatch for axis " + std::to_string(axis));
        }
    }
    return 0;
}

int testReportedBoundaryFailureAndFilledAxes() {
    auto image = makeImage<unsigned int>(1197U);
    itkSignal<unsigned int> signal(image, false);
    signal.setCategoricalColorMode();

    constexpr unsigned int newMergeLabel = 11971U;
    image->SetPixel({1, 1, 1}, newMergeLabel);
    image->Modified();
    const QRgb expectedColor = signal.colorForLabel(newMergeLabel);

    signal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Filled);
    if (int result = expectCenterColorOnAllAxes(signal, expectedColor, "filled merge label")) {
        return result;
    }

    signal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Boundaries);
    if (int result = expectCenterColorOnAllAxes(signal, expectedColor, "boundary merge label")) {
        return result;
    }

    const auto snapshot = signal.labelColorSnapshot();
    if (snapshot.edgeColors == nullptr || snapshot.edgeColors->size() != 0 ||
        snapshot.overrides == nullptr || !snapshot.overrides->empty()) {
        return fail("categorical storage should not scale with the maximum label");
    }
    return 0;
}

int testMaximumUnsignedLabels() {
    auto image32 = makeImage<unsigned int>(0U);
    itkSignal<unsigned int> signal32(image32, false);
    signal32.setCategoricalColorMode();
    constexpr unsigned int maximum32 = std::numeric_limits<unsigned int>::max();
    image32->SetPixel({1, 1, 1}, maximum32);
    if (int result = expectCenterColorOnAllAxes(
            signal32, signal32.colorForLabel(maximum32), "UINT32_MAX label")) {
        return result;
    }

    auto image64 = makeImage<unsigned long long>(0ULL);
    itkSignal<unsigned long long> signal64(image64, false);
    signal64.setCategoricalColorMode();
    constexpr unsigned long long maximum64 = std::numeric_limits<unsigned long long>::max();
    image64->SetPixel({1, 1, 1}, maximum64);
    if (int result = expectCenterColorOnAllAxes(
            signal64, signal64.colorForLabel(maximum64), "UINT64_MAX label")) {
        return result;
    }
    signal64.setValueColorToTransparent(maximum64);
    if (qAlpha(signal64.colorForLabel(maximum64)) != 0) {
        return fail("UINT64_MAX should also be a valid sparse override key");
    }

    using SparseImage = itk::Image<unsigned long long, 3>;
    auto sparseImage = SparseImage::New();
    SparseImage::IndexType sparseStart{};
    sparseStart.Fill(0);
    SparseImage::SizeType sparseSize{{40, 1, 1}};
    sparseImage->SetRegions(SparseImage::RegionType(sparseStart, sparseSize));
    sparseImage->Allocate();
    for (std::uint64_t index = 0; index < sparseSize[0]; ++index) {
        sparseImage->SetPixel(
            {static_cast<long>(index), 0, 0},
            (index + 1U) << 48U);
    }
    itkSignal<unsigned long long> sparseSignal(sparseImage, false);
    sparseSignal.setCategoricalColorMode();
    std::vector<quint32> sparseBuffer;
    const QImage sparseSlice = sparseSignal.calculateSliceQImage(0, 2, &sparseBuffer);
    if (sparseSlice.isNull() || sparseBuffer.size() != sparseSize[0]) {
        return fail("widely separated 64-bit labels should render with bounded sparse caching");
    }
    return 0;
}

int testSignedIntegerRendering() {
    auto image = makeImage<short>(static_cast<short>(-10));
    image->SetPixel({1, 1, 1}, static_cast<short>(10));
    itkSignal<short> signal(image, false);

    for (unsigned int axis = 0; axis < 3; ++axis) {
        std::vector<quint32> buffer;
        signal.calculateSliceQImage(1, axis, &buffer);
        if (buffer.size() < 9 || qAlpha(buffer[0]) != 0 || qAlpha(buffer[4]) != signal.getAlpha()) {
            return fail("signed continuous rendering should normalize negative values directly");
        }
    }

    signal.setCategoricalColorMode();
    const auto negativeKey = static_cast<std::uint64_t>(static_cast<std::int64_t>(-10));
    std::vector<quint32> buffer;
    signal.calculateSliceQImage(1, 2, &buffer);
    if (buffer[0] != signal.colorForLabel(negativeKey)) {
        return fail("negative categorical values should use a stable 64-bit key");
    }
    return 0;
}

int testPaletteStabilityOverridesAndSnapshots() {
    auto firstImage = makeImage<unsigned int>(7U);
    auto secondImage = makeImage<unsigned int>(7U);
    itkSignal<unsigned int> first(firstImage, false);
    itkSignal<unsigned int> second(secondImage, false);
    first.setCategoricalColorMode();
    second.setCategoricalColorMode();

    if (first.colorForLabel(7) != second.colorForLabel(7)) {
        return fail("the default categorical palette should be deterministic");
    }

    const auto originalSnapshot = first.labelColorSnapshot();
    const QRgb originalColor = originalSnapshot.colorForLabel(7);
    first.setValueColorToBlack(7);
    const auto blackSnapshot = first.labelColorSnapshot();
    if (blackSnapshot.colorForLabel(7) != qRgba(0, 0, 0, 255) ||
        originalSnapshot.colorForLabel(7) != originalColor) {
        return fail("publishing an override must not mutate an older snapshot");
    }

    first.setValueColorToTransparent(7);
    if (first.colorForLabel(7) != qRgba(0, 0, 0, 0)) {
        return fail("the most recent value-color override should win");
    }

    const auto seedBeforeRandomize = first.labelColorSnapshot().paletteSeed;
    first.randomizeCategoricalPalette();
    const auto randomized = first.labelColorSnapshot();
    if (randomized.paletteSeed == seedBeforeRandomize ||
        randomized.colorForLabel(7) != qRgba(0, 0, 0, 0)) {
        return fail("randomizing should change the seed while preserving overrides");
    }

    bool foundChangedGeneratedColor = false;
    for (std::uint64_t label = 8; label < 32; ++label) {
        foundChangedGeneratedColor = foundChangedGeneratedColor ||
            originalSnapshot.colorForLabel(label) != randomized.colorForLabel(label);
    }
    if (!foundChangedGeneratedColor) {
        return fail("randomizing should change generated categorical colors");
    }
    return 0;
}

int testIncrementalEdgeColorsAndMissingStatuses() {
    auto image = makeImage<unsigned int>(0U);
    itkSignal<unsigned int> signal(image, false);
    signal.setEdgeColorMode();

    std::unordered_map<unsigned int, char> edgeStatuses{{42U, 7}, {43U, 8}};
    const std::unordered_map<char, QRgb> statusColors{
        {7, qRgb(255, 0, 0)},
        {8, qRgb(0, 255, 0)}};
    signal.rebuildEdgeColorTable(edgeStatuses, statusColors);
    const auto oldSnapshot = signal.labelColorSnapshot();
    if (qRed(oldSnapshot.colorForLabel(42)) != 255 ||
        qAlpha(oldSnapshot.colorForLabel(42)) != signal.getAlpha() ||
        qGreen(oldSnapshot.colorForLabel(43)) != 255 ||
        oldSnapshot.edgeColors == nullptr || oldSnapshot.edgeColors->size() != 2) {
        return fail("edge colors should contain one structurally stable entry per edge");
    }

    edgeStatuses[42] = 8;
    if (!signal.updateEdgeColorTable({42U}, edgeStatuses, statusColors)) {
        return fail("an existing edge should update without rebuilding the table");
    }
    const auto incrementallyUpdated = signal.labelColorSnapshot();
    if (incrementallyUpdated.edgeColors != oldSnapshot.edgeColors ||
        qGreen(incrementallyUpdated.colorForLabel(42)) != 255 ||
        qGreen(oldSnapshot.colorForLabel(42)) != 255) {
        return fail("incremental edge updates should atomically reuse the published table");
    }

    edgeStatuses[43] = 9;
    if (!signal.updateEdgeColorTable({43U}, edgeStatuses, statusColors) ||
        qAlpha(signal.colorForLabel(43)) != 0) {
        return fail("a missing status color should update an existing edge to transparent");
    }

    edgeStatuses[42] = 7;
    edgeStatuses.emplace(44U, 7);
    if (signal.updateEdgeColorTable({42U, 44U}, edgeStatuses, statusColors) ||
        qGreen(signal.colorForLabel(42)) != 255) {
        return fail("an unknown edge should reject the whole incremental update");
    }

    signal.rebuildEdgeColorTable(edgeStatuses, statusColors);
    const auto rebuiltSnapshot = signal.labelColorSnapshot();
    if (rebuiltSnapshot.edgeColors == oldSnapshot.edgeColors ||
        rebuiltSnapshot.edgeColors == nullptr || rebuiltSnapshot.edgeColors->size() != 3 ||
        qRed(rebuiltSnapshot.colorForLabel(42)) != 255 ||
        qRed(rebuiltSnapshot.colorForLabel(44)) != 255 ||
        qAlpha(rebuiltSnapshot.colorForLabel(43)) != 0) {
        return fail("a structural edge change should rebuild the complete table");
    }

    signal.setAlpha(200);
    if (qAlpha(signal.colorForLabel(42)) != 200 ||
        qAlpha(rebuiltSnapshot.colorForLabel(42)) == 200) {
        return fail("edge alpha updates should publish a new immutable snapshot");
    }
    signal.setValueColorToTransparent(42);
    if (qAlpha(signal.colorForLabel(42)) != 0) {
        return fail("explicit overrides should take precedence over edge status colors");
    }
    return 0;
}

int testConcurrentSnapshotPublication() {
    auto image = makeImage<unsigned int>(91U);
    itkSignal<unsigned int> signal(image, false);
    signal.setCategoricalColorMode();
    const LabelColorSnapshot snapshotBeforeUpdates = signal.labelColorSnapshot();
    const QRgb colorBeforeUpdates = snapshotBeforeUpdates.colorForLabel(91);

    std::atomic<bool> writerFinished{false};
    std::thread writer([&]() {
        for (int update = 0; update < 2000; ++update) {
            signal.randomizeCategoricalPalette();
        }
        writerFinished.store(true, std::memory_order_release);
    });

    std::uint64_t observedSeed = snapshotBeforeUpdates.paletteSeed;
    while (!writerFinished.load(std::memory_order_acquire)) {
        const LabelColorSnapshot current = signal.labelColorSnapshot();
        observedSeed ^= current.paletteSeed;
        static_cast<void>(current.colorForLabel(91));
    }
    writer.join();

    if (snapshotBeforeUpdates.colorForLabel(91) != colorBeforeUpdates ||
        signal.labelColorSnapshot().paletteSeed == snapshotBeforeUpdates.paletteSeed ||
        observedSeed == std::numeric_limits<std::uint64_t>::max()) {
        return fail("snapshot publication should be atomic and preserve older snapshots");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);

    if (int result = testReportedBoundaryFailureAndFilledAxes()) return result;
    if (int result = testMaximumUnsignedLabels()) return result;
    if (int result = testSignedIntegerRendering()) return result;
    if (int result = testPaletteStabilityOverridesAndSnapshots()) return result;
    if (int result = testIncrementalEdgeColorsAndMissingStatuses()) return result;
    if (int result = testConcurrentSnapshotPublication()) return result;

    std::cout << "itkSignal sparse color lookup tests passed\n";
    return 0;
}
