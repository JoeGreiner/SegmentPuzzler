#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <streamvbyte.h>

namespace segment_puzzler::detail {

template<typename TPixel>
struct StreamVBytePlane {
    static_assert(std::is_unsigned_v<TPixel> && sizeof(TPixel) <= sizeof(std::uint32_t));
    using Plane = StreamVBytePlane;
    struct RowEdit { std::size_t z, row, begin, end; TPixel value; };
    struct Line { std::uint32_t offset, runs; };
    TPixel constant{};
    std::vector<Line> lines;
    std::vector<std::uint8_t> encoded;

    explicit StreamVBytePlane(TPixel value = {}) : constant(value) {}
    bool uniform() const { return lines.empty(); }
    std::size_t bytes() const { return lines.capacity() * sizeof(Line) + encoded.capacity(); }

    void append(const std::vector<std::uint32_t> &values, std::vector<std::uint8_t> &scratch) {
        if (encoded.size() > UINT32_MAX || values.size() > UINT32_MAX - 3)
            throw std::overflow_error("StreamVByte row limit exceeded");
        lines.push_back({static_cast<std::uint32_t>(encoded.size()), static_cast<std::uint32_t>(values.size() / 2)});
        scratch.resize(streamvbyte_max_compressedbytes(static_cast<std::uint32_t>(values.size())));
        const auto count = streamvbyte_encode(values.data(), static_cast<std::uint32_t>(values.size()), scratch.data());
        encoded.insert(encoded.end(), scratch.begin(), scratch.begin() + count);
    }
    void decode(std::size_t row, std::vector<std::uint32_t> &values) const {
        const auto line = lines[row];
        values.resize(2 * std::size_t(line.runs));
        streamvbyte_decode(encoded.data() + line.offset, values.data(), static_cast<std::uint32_t>(values.size()));
    }
    void finish() {
        // SIMD decoding can read beyond the last encoded row.
        encoded.resize(encoded.size() + STREAMVBYTE_PADDING);
        encoded.shrink_to_fit();
    }
    static Plane buildColumns(const TPixel *pixels, std::size_t width, std::size_t height) {
        std::vector<std::size_t> positions(width, 1);
        for (std::size_t y = 1; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
                positions[x] += pixels[y * width + x] != pixels[(y - 1) * width + x];
        std::vector<std::size_t> offsets(width + 1);
        for (std::size_t x = 0; x < width; ++x) offsets[x + 1] = offsets[x] + positions[x];
        std::vector<TPixel> labels(offsets.back());
        std::vector<std::uint32_t> ends(offsets.back());
        std::copy_n(offsets.data(), width, positions.data());
        for (std::size_t y = 1; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
                if (pixels[y * width + x] != pixels[(y - 1) * width + x]) {
                    const auto output = positions[x]++;
                    labels[output] = pixels[(y - 1) * width + x];
                    ends[output] = static_cast<std::uint32_t>(y);
                }
        for (std::size_t x = 0; x < width; ++x) {
            labels[positions[x]] = pixels[(height - 1) * width + x];
            ends[positions[x]] = static_cast<std::uint32_t>(height);
        }

        Plane plane;
        plane.lines.reserve(width);
        std::vector<std::uint32_t> values;
        std::vector<std::uint8_t> scratch;
        for (std::size_t row = 0; row < width; ++row) {
            const auto start = offsets[row], count = offsets[row + 1] - start;
            values.resize(2 * count);
            for (std::size_t i = 0; i < count; ++i) {
                values[2 * i] = labels[start + i];
                values[2 * i + 1] = ends[start + i] - (i == 0 ? 0 : ends[start + i - 1]);
            }
            plane.append(values, scratch);
        }
        plane.finish();
        return plane;
    }
    static Plane build(const TPixel *pixels, std::size_t width, std::size_t height) {
        Plane plane(pixels[0]);
        if (std::all_of(pixels, pixels + width * height, [&](TPixel value) { return value == pixels[0]; })) return plane;
        plane.lines.reserve(height);
        std::vector<std::uint32_t> values;
        std::vector<std::uint8_t> scratch;
        for (std::size_t row = 0; row < height; ++row) {
            values.clear();
            const auto *line = pixels + row * width;
            for (std::size_t begin = 0; begin < width;) {
                auto end = begin + 1;
                while (end < width && line[end] == line[begin]) ++end;
                values.push_back(line[begin]);
                values.push_back(static_cast<std::uint32_t>(end - begin));
                begin = end;
            }
            plane.append(values, scratch);
        }
        plane.finish();
        return plane;
    }
    template<typename Visit>
    void visit(std::size_t row, std::size_t begin, std::size_t end, Visit visitRun,
               std::vector<std::uint32_t> &scratch) const {
        if (begin == end) return;
        if (uniform()) { visitRun(constant, begin, end); return; }
        decode(row, scratch);
        std::size_t position = 0;
        for (std::size_t i = 0; position < end; i += 2) {
            const auto stop = position + scratch[i + 1];
            if (stop > begin) visitRun(static_cast<TPixel>(scratch[i]), std::max(position, begin), std::min(stop, end));
            position = stop;
        }
    }
    Plane edited(std::size_t width, std::size_t height, const RowEdit *first, const RowEdit *last) const {
        Plane result;
        result.lines.reserve(height);
        result.encoded.reserve(encoded.size());
        std::vector<std::uint32_t> source, values;
        std::vector<std::uint8_t> scratch;
        for (std::size_t row = 0; row < height; ++row) {
            if (!uniform() && (first == last || first->row != row)) {
                if (result.encoded.size() > UINT32_MAX) throw std::overflow_error("StreamVByte row limit exceeded");
                result.lines.push_back({static_cast<std::uint32_t>(result.encoded.size()), lines[row].runs});
                const auto end = row + 1 == lines.size() ? encoded.size() - STREAMVBYTE_PADDING : lines[row + 1].offset;
                result.encoded.insert(result.encoded.end(), encoded.begin() + lines[row].offset, encoded.begin() + end);
                continue;
            }
            if (uniform()) source = {constant, static_cast<std::uint32_t>(width)};
            else decode(row, source);
            values.clear();
            const auto append = [&](std::uint32_t label, std::size_t length) {
                if (!length) return;
                if (!values.empty() && values[values.size() - 2] == label) values.back() += static_cast<std::uint32_t>(length);
                else { values.push_back(label); values.push_back(static_cast<std::uint32_t>(length)); }
            };
            std::size_t run = 0, runStart = 0;
            const auto copyRange = [&](std::size_t begin, std::size_t end) {
                while (begin < end) {
                    const auto stop = runStart + source[run + 1];
                    if (stop > begin) {
                        const auto next = std::min(stop, end);
                        append(source[run], next - begin);
                        begin = next;
                    }
                    if (stop <= end) { runStart = stop; run += 2; }
                }
            };
            std::size_t position = 0;
            while (first != last && first->row == row) {
                copyRange(position, first->begin);
                append(first->value, first->end - first->begin);
                position = first++->end;
            }
            copyRange(position, width);
            result.append(values, scratch);
        }
        result.finish();
        if (std::all_of(result.lines.begin(), result.lines.end(), [](const Line &line) { return line.runs == 1; })) {
            result.decode(0, values);
            const auto value = values[0];
            for (std::size_t row = 1; row < height; ++row) {
                result.decode(row, values);
                if (values[0] != value) return result;
            }
            return Plane(static_cast<TPixel>(value));
        }
        return result;
    }
};

} // namespace segment_puzzler::detail
