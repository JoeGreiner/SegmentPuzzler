#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "src/viewers/itkSignal.h"

namespace {

using Label = std::uint32_t;
using Image = itk::Image<Label, 3>;
using Clock = std::chrono::steady_clock;
constexpr double maximumOverheadPercent = 35.0;

struct ComparisonTiming {
    double denseMilliseconds = 0.0;
    double sparseMilliseconds = 0.0;
    double sparseToDenseRatio = 0.0;
};

volatile std::uint64_t renderedColorChecksum = 0;

Image::Pointer makeBlockLabelImage(Label &maximumLabel) {
    auto image = Image::New();
    Image::IndexType start{};
    start.Fill(0);
    Image::SizeType size{{192, 160, 96}};
    image->SetRegions(Image::RegionType(start, size));
    image->Allocate();

    Label *buffer = image->GetBufferPointer();
    maximumLabel = 0;
    for (std::size_t z = 0; z < size[2]; ++z) {
        for (std::size_t y = 0; y < size[1]; ++y) {
            for (std::size_t x = 0; x < size[0]; ++x) {
                const Label label = static_cast<Label>(
                    1 + x / 32 + (y / 32) * 6 + (z / 16) * 6 * 5);
                buffer[x + y * size[0] + z * size[0] * size[1]] = label;
                maximumLabel = std::max(maximumLabel, label);
            }
        }
    }
    return image;
}

void renderDenseReference(const Image *image,
                          unsigned int sliceIndex,
                          unsigned int sliceAxis,
                          bool renderBoundaries,
                          const std::vector<QRgb> &colors,
                          std::vector<quint32> &output) {
    const auto size = image->GetLargestPossibleRegion().GetSize();
    const auto dims = slice_geometry::makeDimensions(size[0], size[1], size[2]);
    const auto width = static_cast<unsigned long>(slice_geometry::sliceWidth(sliceAxis, dims));
    const auto height = static_cast<unsigned long>(slice_geometry::sliceHeight(sliceAxis, dims));
    output.resize(static_cast<std::size_t>(width) * height);

    unsigned long baseOffset = 0;
    unsigned long columnStride = 0;
    unsigned long rowStride = 0;
    switch (sliceAxis) {
        case 0:
            baseOffset = sliceIndex;
            columnStride = size[0] * size[1];
            rowStride = size[0];
            break;
        case 1:
            baseOffset = sliceIndex * size[0];
            columnStride = 1;
            rowStride = size[0] * size[1];
            break;
        case 2:
            baseOffset = sliceIndex * size[0] * size[1];
            columnStride = 1;
            rowStride = size[0];
            break;
        default:
            throw std::logic_error("unsupported slice axis");
    }

    const Label *imageBuffer = image->GetBufferPointer();
    constexpr QRgb transparent = 0;
    for (unsigned long row = 0; row < height; ++row) {
        const unsigned long imageRowOffset = baseOffset + row * rowStride;
        const unsigned long outputRowOffset = row * width;
        for (unsigned long column = 0; column < width; ++column) {
            const unsigned long imageOffset = imageRowOffset + column * columnStride;
            const Label label = imageBuffer[imageOffset];
            if (!renderBoundaries) {
                output[outputRowOffset + column] = colors.at(label);
                continue;
            }
            const bool hasFourNeighbors =
                row > 0 && row + 1 < height && column > 0 && column + 1 < width;
            const bool isBoundary =
                !hasFourNeighbors ||
                imageBuffer[imageOffset - rowStride] != label ||
                imageBuffer[imageOffset + rowStride] != label ||
                imageBuffer[imageOffset - columnStride] != label ||
                imageBuffer[imageOffset + columnStride] != label;
            output[outputRowOffset + column] = isBoundary ? colors.at(label) : transparent;
        }
    }
}

void consume(const std::vector<quint32> &buffer) {
    std::uint64_t checksum = renderedColorChecksum;
    for (const QRgb color : buffer) {
        checksum = (checksum * 1099511628211ULL) ^ color;
    }
    renderedColorChecksum = checksum;
}

template<typename Render>
double medianMilliseconds(Render &&render, std::vector<quint32> &buffer) {
    constexpr int warmupRuns = 12;
    constexpr int measuredRuns = 51;
    for (int run = 0; run < warmupRuns; ++run) {
        render();
        consume(buffer);
    }

    std::vector<double> timings;
    timings.reserve(measuredRuns);
    for (int run = 0; run < measuredRuns; ++run) {
        const auto start = Clock::now();
        render();
        const auto end = Clock::now();
        timings.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
        consume(buffer);
    }
    std::sort(timings.begin(), timings.end());
    return timings[timings.size() / 2];
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);

    Label maximumLabel = 0;
    const auto image = makeBlockLabelImage(maximumLabel);
    itkSignal<Label> signal(image, false);
    signal.setCategoricalColorMode();

    std::vector<QRgb> denseColors(static_cast<std::size_t>(maximumLabel) + 1);
    for (std::size_t label = 0; label < denseColors.size(); ++label) {
        denseColors[label] = signal.colorForLabel(label);
    }

    const auto size = image->GetLargestPossibleRegion().GetSize();
    const unsigned int sliceIndices[3]{
        static_cast<unsigned int>(size[0] / 2),
        static_cast<unsigned int>(size[1] / 2),
        static_cast<unsigned int>(size[2] / 2)};
    bool withinLimit = true;
    signal.setLabelRenderMode(itkSignalBase::LabelRenderMode::Filled);
    std::cout << "axis,dense_median_ms,sparse_median_ms,median_ratio,overhead_percent\n";
    for (unsigned int axis = 0; axis < 3; ++axis) {
        std::vector<quint32> denseBuffer;
        std::vector<quint32> sparseBuffer;
        denseBuffer.reserve(static_cast<std::size_t>(size[0]) * size[1]);
        sparseBuffer.reserve(static_cast<std::size_t>(size[0]) * size[1]);

        auto measureDense = [&]() {
            return medianMilliseconds(
                [&]() {
                    renderDenseReference(
                        image.GetPointer(), sliceIndices[axis], axis,
                        false, denseColors, denseBuffer);
                },
                denseBuffer);
        };
        auto measureSparse = [&]() {
            return medianMilliseconds(
                [&]() {
                    const QImage rendered = signal.calculateSliceQImage(
                        sliceIndices[axis], axis, &sparseBuffer);
                    if (rendered.isNull()) {
                        throw std::runtime_error("sparse renderer returned an empty image");
                    }
                },
                sparseBuffer);
        };

        std::array<ComparisonTiming, 3> comparisons;
        for (std::size_t comparisonIndex = 0;
             comparisonIndex < comparisons.size();
             ++comparisonIndex) {
            ComparisonTiming &comparison = comparisons[comparisonIndex];
            if (comparisonIndex == 1) {
                comparison.sparseMilliseconds = measureSparse();
                comparison.denseMilliseconds = measureDense();
            } else {
                comparison.denseMilliseconds = measureDense();
                comparison.sparseMilliseconds = measureSparse();
            }

            if (denseBuffer != sparseBuffer) {
                std::cerr << "dense reference and sparse renderer differ for axis "
                          << axis << " in comparison " << comparisonIndex + 1 << '\n';
                return 2;
            }
            comparison.sparseToDenseRatio =
                comparison.sparseMilliseconds / comparison.denseMilliseconds;
        }

        std::sort(
            comparisons.begin(),
            comparisons.end(),
            [](const ComparisonTiming &left, const ComparisonTiming &right) {
                return left.sparseToDenseRatio < right.sparseToDenseRatio;
            });
        const ComparisonTiming &medianComparison = comparisons[1];
        const double overheadPercent =
            (medianComparison.sparseToDenseRatio - 1.0) * 100.0;
        withinLimit = withinLimit &&
            overheadPercent <= maximumOverheadPercent;
        std::cout << axis << ',' << std::fixed << std::setprecision(4)
                  << medianComparison.denseMilliseconds << ','
                  << medianComparison.sparseMilliseconds << ','
                  << medianComparison.sparseToDenseRatio << ','
                  << overheadPercent << '\n';
    }

    if (!withinLimit) {
        std::cerr << "Sparse slice rendering exceeded the "
                  << std::defaultfloat << maximumOverheadPercent
                  << "% median overhead limit.\n";
        return 1;
    }
    std::cout << "Sparse slice rendering stayed within the "
              << std::defaultfloat << maximumOverheadPercent
              << "% median overhead limit.\n";
    return renderedColorChecksum == 0xFFFFFFFFFFFFFFFFULL ? 3 : 0;
}
