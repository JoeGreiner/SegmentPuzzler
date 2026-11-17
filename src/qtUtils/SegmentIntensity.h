#pragma once

#include <itkImage.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace segment_puzzler {

using IntensityBuffer = std::variant<const char *, const signed char *, const unsigned char *,
    const short *, const unsigned short *, const int *, const unsigned int *, const long *,
    const unsigned long *, const long long *, const unsigned long long *, const float *, const double *>;

template<class... Pointers>
void getIntensityBuffer(const itk::ImageBase<3> &image, std::variant<Pointers...> &buffer) {
    const auto tryType = [&](auto pointer) {
        using Pixel = std::remove_cv_t<std::remove_pointer_t<decltype(pointer)>>;
        const auto *typed = dynamic_cast<const itk::Image<Pixel, 3> *>(&image);
        if (!typed || !typed->GetBufferPointer()) return false;
        buffer = typed->GetBufferPointer();
        return true;
    };
    if (!(tryType(Pointers{}) || ...)) throw std::invalid_argument("Expected a buffered scalar intensity image.");
}

template<class Pixel, class LabelObject>
std::array<double, 7> measureSegmentIntensity(const Pixel *pixels, const itk::ImageBase<3> &image,
                                              const LabelObject &label, bool quantiles) {
    std::array<double, 7> result;
    result.fill(std::numeric_limits<double>::quiet_NaN());
    const auto size = label.GetNumberOfPixels();
    std::vector<Pixel> samples;
    std::vector<std::size_t> histogram;
    int histogramOrigin = 0;
    if (quantiles) {
        if constexpr (std::is_integral_v<Pixel> && sizeof(Pixel) <= 2) {
            const std::size_t bins = 1u << (8 * sizeof(Pixel));
            if (size >= bins) {
                histogram.resize(bins);
                histogramOrigin = std::numeric_limits<Pixel>::lowest();
            }
        }
        if (histogram.empty()) samples.reserve(size);
    }

    std::size_t count = 0;
    long double sum = 0, mean = 0;
    auto minimum = std::numeric_limits<Pixel>::max();
    auto maximum = std::numeric_limits<Pixel>::lowest();
    auto index = image.GetBufferedRegion().GetIndex();
    for (typename LabelObject::ConstLineIterator it(&label); !it.IsAtEnd(); ++it) {
        const auto &line = it.GetLine();
        for (unsigned axis = 0; axis < LabelObject::ImageDimension; ++axis)
            index[axis] = line.GetIndex()[axis];
        const auto *begin = pixels + image.ComputeOffset(index);
        for (const auto *p = begin; p != begin + line.GetLength(); ++p) {
            const Pixel value = *p;
            if constexpr (std::is_floating_point_v<Pixel>) {
                if (!std::isfinite(value)) continue;
            }
            ++count;
            if constexpr (std::is_same_v<Pixel, double>) {
                const long double delta = static_cast<long double>(value) - mean;
                mean += std::isfinite(delta) ? delta / count : value / count - mean / count;
            } else {
                sum += value;
            }
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            if (quantiles) {
                if (!histogram.empty()) ++histogram[static_cast<int>(value) - histogramOrigin];
                else samples.push_back(value);
            }
        }
    }
    if (!count) return result;
    if constexpr (!std::is_same_v<Pixel, double>) mean = sum / count;
    result[0] = static_cast<double>(mean);
    result[1] = static_cast<double>(minimum);
    result[2] = static_cast<double>(maximum);
    if (quantiles) {
        std::size_t bin = 0, beforeBin = 0, first = 0, column = 3;
        for (std::size_t percent : {1, 5, 95, 99}) {
            // Zero-based ceil(count * percent / 100) - 1, without overflowing the product.
            const auto rank = (count / 100) * percent + ((count % 100) * percent + 99) / 100 - 1;
            if (histogram.empty()) {
                std::nth_element(samples.begin() + first, samples.begin() + rank, samples.end());
                result[column++] = static_cast<double>(samples[rank]);
                first = rank;
            } else {
                while (rank - beforeBin >= histogram[bin]) beforeBin += histogram[bin++];
                result[column++] = static_cast<double>(bin) + histogramOrigin;
            }
        }
    }
    return result;
}

} // namespace segment_puzzler
