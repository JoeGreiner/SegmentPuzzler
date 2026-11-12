#include <QCoreApplication>
#include <QEventLoop>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <future>
#include <unordered_map>
#include <vector>

#include "src/viewers/itkSignal.h"
#include "src/qtUtils/SliderUpdateTimer.h"

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

int testLabelBoundaryWidths() {
    using Image = itk::Image<unsigned int, 3>;
    auto image = Image::New();
    const Image::SizeType size{{17, 15, 13}};
    image->SetRegions(Image::RegionType(Image::IndexType{{0, 0, 0}}, size));
    image->Allocate();
    for (int z = 0; z < 13; ++z) {
        for (int y = 0; y < 15; ++y) {
            for (int x = 0; x < 17; ++x) {
                unsigned int label = x >= 9 ? 2 : 1;
                if (std::abs(x - 5) + std::abs(y - 6) + std::abs(z - 5) <= 3) label = 3;
                if (x == 4 && y == 7) label = 0;
                image->SetPixel({x, y, z}, label);
            }
        }
    }

    itkSignal<unsigned int> signal(image, false);
    signal.setCategoricalColorMode();
    signal.setValueColorToTransparent(0);
    signal.setAlpha(83);
    if (signal.getLabelBoundaryWidth() != 1) return fail("default outline width should stay at one pixel");
    signal.setLabelBoundaryWidth(-3);
    if (signal.getLabelBoundaryWidth() != 1) return fail("outline width should have a lower bound");
    signal.setLabelBoundaryWidth(100);
    if (signal.getLabelBoundaryWidth() != itkSignalBase::MaximumLabelBoundaryWidth) {
        return fail("outline width should have an upper bound");
    }

    signal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Boundaries);
    const int horizontalAxes[] = {2, 0, 0};
    const int verticalAxes[] = {1, 2, 1};
    std::vector<quint32> buffer;
    for (int axis = 0; axis < 3; ++axis) {
        const int horizontalAxis = horizontalAxes[axis];
        const int verticalAxis = verticalAxes[axis];
        for (int slice : {0, static_cast<int>(size[axis] / 2), static_cast<int>(size[axis] - 1)}) {
            for (int width : {1, 2, 4, itkSignalBase::MaximumLabelBoundaryWidth}) {
                signal.setLabelBoundaryWidth(width);
                const QImage rendered = signal.calculateSliceQImage(slice, axis, &buffer);
                for (int y = 0; y < rendered.height(); ++y) {
                    for (int x = 0; x < rendered.width(); ++x) {
                        Image::IndexType index;
                        index[axis] = slice;
                        index[horizontalAxis] = x;
                        index[verticalAxis] = y;
                        const unsigned int label = image->GetPixel(index);
                        bool isOutline = false;
                        // Independent reference: a thick outline contains pixels within the
                        // requested in-plane Manhattan distance of another label or the exterior.
                        for (int dy = -width; dy <= width && !isOutline; ++dy) {
                            for (int dx = -width; dx <= width && !isOutline; ++dx) {
                                if (std::abs(dx) + std::abs(dy) > width) continue;
                                auto neighbor = index;
                                neighbor[horizontalAxis] += dx;
                                neighbor[verticalAxis] += dy;
                                isOutline = !image->GetLargestPossibleRegion().IsInside(neighbor) ||
                                            image->GetPixel(neighbor) != label;
                            }
                        }
                        const QRgb expected = isOutline ? signal.colorForLabel(label) : qRgba(0, 0, 0, 0);
                        if (rendered.pixel(x, y) != expected) {
                            return fail("outline mismatch on axis " + std::to_string(axis) +
                                        " at width " + std::to_string(width));
                        }
                    }
                }
            }
        }
    }

    signal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Filled);
    const QImage filledWide = signal.calculateSliceQImage(6, 2, &buffer).copy();
    signal.setLabelBoundaryWidth(1);
    if (signal.calculateSliceQImage(6, 2, &buffer) != filledWide) {
        return fail("outline width should not affect filled segments");
    }
    signal.setLabelBoundaryWidth(4);
    signal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Boundaries);
    if (signal.getLabelBoundaryWidth() != 4) return fail("outline width should survive render mode changes");

    auto tinyImage = makeImage<unsigned int>(7);
    itkSignal<unsigned int> tinySignal(tinyImage, false);
    tinySignal.setCategoricalColorMode();
    tinySignal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Boundaries);
    tinySignal.setLabelBoundaryWidth(itkSignalBase::MaximumLabelBoundaryWidth);
    return expectCenterColorOnAllAxes(tinySignal, tinySignal.colorForLabel(7), "outline wider than segment");
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


template<typename Pixel>
int testContinuousRendering(typename itk::Image<Pixel, 3>::SizeType size = {{257, 259, 3}}) {
    using Image = itk::Image<Pixel, 3>;
    auto image = Image::New();
    // One plane exceeds the 16-bit table threshold; the others use direct conversion.
    image->SetRegions(size);
    image->Allocate();
    for (std::size_t i = 0; i < image->GetLargestPossibleRegion().GetNumberOfPixels(); ++i) {
        const long long range = static_cast<long long>(std::numeric_limits<Pixel>::max()) -
                                std::numeric_limits<Pixel>::lowest() + 1;
        image->GetBufferPointer()[i] = static_cast<Pixel>(static_cast<long long>(i % range) + std::numeric_limits<Pixel>::lowest());
    }
    itkSignal<Pixel> signal(image, false);
    const std::array<std::pair<double, double>, 6> ranges{{
        {-100, 1000}, {0, 0}, {100, 100}, {200, 100},
        {std::numeric_limits<double>::quiet_NaN(), 100}, {-32768, 65535}}};
    for (const auto blend : {itkSignalBase::BlendMode::SourceOver, itkSignalBase::BlendMode::Additive}) {
        signal.setBlendMode(blend);
        for (std::size_t setting = 0; setting < ranges.size(); ++setting) {
            const auto [lower, upper] = ranges[setting];
            const int alpha = setting % 2 ? 83 : 255;
            const QColor tint = setting % 2 ? QColor(17, 129, 243) : QColor(Qt::white);
            signal.setNorm(lower, upper);
            signal.setAlpha(alpha);
            signal.setMainColor(tint);
            for (unsigned int axis = 0; axis < 3; ++axis) {
                for (unsigned int slice : {0U, static_cast<unsigned int>(size[axis] / 2), static_cast<unsigned int>(size[axis] - 1)}) {
                    std::vector<quint32> buffer;
                    const auto rendered = signal.calculateSliceQImage(slice, axis, &buffer);
                    for (int row = 0; row < rendered.height(); ++row) {
                        for (int column = 0; column < rendered.width(); ++column) {
                            typename Image::IndexType index;
                            index[axis] = slice;
                            index[axis == 0 ? 2 : 0] = column;
                            index[axis == 2 ? 1 : axis == 1 ? 2 : 1] = row;
                            const double value = image->GetPixel(index);
                            double normalized = 0;
                            if (std::isfinite(lower) && std::isfinite(upper) && upper >= lower) {
                                normalized = upper == lower ? (value != 0 && value >= lower ? 255 : 0)
                                    : std::clamp((value - lower) * 255.0 / (upper - lower), 0.0, 255.0);
                            }
                            const QRgb expected = blend == itkSignalBase::BlendMode::Additive
                                ? qRgba(static_cast<unsigned char>(normalized * (tint.red() / 255.)),
                                        static_cast<unsigned char>(normalized * (tint.green() / 255.)),
                                        static_cast<unsigned char>(normalized * (tint.blue() / 255.)), alpha)
                                : qRgba(tint.red(), tint.green(), tint.blue(),
                                        static_cast<unsigned char>(normalized * (alpha / 255.)));
                            if (rendered.pixel(column, row) != expected) return fail("continuous scalar rendering changed");
                        }
                    }
                }
            }
        }
    }
    // Changing mode must bypass the intensity table, including signed labels.
    signal.setCategoricalColorMode();
    for (unsigned int axis = 0; axis < 3; ++axis) {
        std::vector<quint32> buffer;
        const auto rendered = signal.calculateSliceQImage(0, axis, &buffer);
        for (int row = 0; row < rendered.height(); ++row) {
            for (int column = 0; column < rendered.width(); ++column) {
                typename Image::IndexType index;
                index[axis] = 0;
                index[axis == 0 ? 2 : 0] = column;
                index[axis == 2 ? 1 : axis == 1 ? 2 : 1] = row;
                const auto key = static_cast<std::uint64_t>(static_cast<std::int64_t>(image->GetPixel(index)));
                if (rendered.pixel(column, row) != signal.colorForLabel(key)) return fail("tiled label color changed");
            }
        }
    }
    return 0;
}


int testSliderUpdateTimer() {
    const auto wait = [] {
        QEventLoop events;
        QTimer::singleShot(40, &events, &QEventLoop::quit);
        events.exec();
    };
    int value = 0, rendered = -1, calls = 0;
    const auto render = [&] { rendered = value; ++calls; };
    SliderUpdateTimer update(nullptr, render);
    for (int next = 1; next <= 30; ++next) {
        value = next;
        update.request(true);
    }
    if (calls != 0) return fail("slider drag was not coalesced");
    wait();
    if (calls != 1 || rendered != 30) return fail("slider timer missed the latest value");
    value = 31;
    update.request(true);
    update.flush();
    if (calls != 2 || rendered != 31) return fail("slider release missed the final value");
    update.request(true);
    update.request(false);
    if (calls != 3) return fail("non-drag slider update was not immediate");
    { SliderUpdateTimer temporary(nullptr, render); temporary.request(true); }
    wait();
    if (calls != 3) return fail("cancelled slider timer fired");
    return 0;
}

int testRawSliceCache() {
    using Image = itk::Image<unsigned short, 3>;
    auto image = Image::New();
    image->SetRegions(Image::SizeType{{257, 259, 263}});
    image->Allocate();
    image->FillBuffer(100);
    itkSignal<unsigned short> signal(image, false);
    signal.setNorm(0, 1000);
    for (unsigned int axis = 0; axis < 3; ++axis) {
        image->FillBuffer(100);
        std::vector<quint32> pixels;
        signal.calculateSliceQImage(0, axis, &pixels);
        const auto original = pixels;
        // A display-only refresh reuses the snapshot, even if an unannounced
        // edit changed the buffer. Full refreshes must always observe that edit.
        image->GetBufferPointer()[0] = 500;
        signal.calculateSliceQImage(0, axis, &pixels, true);
        if (pixels != original) return fail("raw slice was not reused");
        signal.calculateSliceQImage(0, axis, &pixels);
        if (pixels[0] == original[0]) return fail("full refresh missed an in-place edit");
        image->GetBufferPointer()[0] = 800;
        image->Modified();
        const auto beforeModified = pixels;
        signal.calculateSliceQImage(0, axis, &pixels, true);
        if (pixels[0] == beforeModified[0]) return fail("ITK Modified did not invalidate the raw slice");
        signal.setNorm(0, 900);
        signal.calculateSliceQImage(0, axis, &pixels, true);
        const auto recolored = pixels;
        signal.calculateSliceQImage(0, axis, &pixels);
        if (pixels != recolored) return fail("cached contrast differs from a fresh slice");
        signal.calculateSliceQImage(1, axis, &pixels, true);
        const auto moved = pixels;
        signal.calculateSliceQImage(1, axis, &pixels);
        if (pixels != moved) return fail("cache reused the wrong slice index");
    }
    std::vector<std::future<std::vector<quint32>>> work;
    // Include concurrent requests on the same axis, as well as all three axes.
    for (unsigned int axis : {0U, 1U, 2U, 0U}) {
        work.push_back(std::async(std::launch::async, [&signal, axis] {
            std::vector<quint32> pixels;
            signal.calculateSliceQImage(axis, axis, &pixels, true);
            return pixels;
        }));
    }
    for (unsigned int i = 0; i < work.size(); ++i) {
        const auto pixels = work[i].get();
        std::vector<quint32> reference;
        signal.calculateSliceQImage(i % 3, i % 3, &reference);
        if (pixels != reference) return fail("parallel cached rendering differs");
    }
    auto replacement = Image::New();
    replacement->SetRegions(image->GetLargestPossibleRegion());
    replacement->Allocate();
    replacement->FillBuffer(700);
    signal.updateImage(replacement);
    for (unsigned int axis = 0; axis < 3; ++axis) {
        std::vector<quint32> pixels, reference;
        signal.calculateSliceQImage(0, axis, &pixels, true);
        signal.calculateSliceQImage(0, axis, &reference);
        if (pixels != reference) return fail("cache survived image replacement");
    }
    signal.releaseImage();
    std::vector<quint32> pixels;
    if (!signal.calculateSliceQImage(0, 0, &pixels, true).isNull()) return fail("released image rendered");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);

    if (int result = testSliderUpdateTimer()) return result;
    if (int result = testRawSliceCache()) return result;
    if (int result = testContinuousRendering<unsigned char>()) return result;
    if (int result = testContinuousRendering<signed char>()) return result;
    if (int result = testContinuousRendering<unsigned short>()) return result;
    if (int result = testContinuousRendering<short>()) return result;
    if (int result = testContinuousRendering<unsigned short>({{3, 257, 259}})) return result;
    if (int result = testReportedBoundaryFailureAndFilledAxes()) return result;
    if (int result = testLabelBoundaryWidths()) return result;
    if (int result = testMaximumUnsignedLabels()) return result;
    if (int result = testSignedIntegerRendering()) return result;
    if (int result = testPaletteStabilityOverridesAndSnapshots()) return result;
    if (int result = testIncrementalEdgeColorsAndMissingStatuses()) return result;
    if (int result = testConcurrentSnapshotPublication()) return result;

    std::cout << "itkSignal sparse color lookup tests passed\n";
    return 0;
}
