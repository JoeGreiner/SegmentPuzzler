#include "VolumeRegionIO.h"
#include "src/utils/LabelScan.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QtEndian>
#include <itk_zlib.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace volume_io {
namespace {
constexpr std::size_t blockBytes = 1024 * 1024;
constexpr const char *contextKey = "SegmentPuzzler.VolumeContext.v1";

[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }

std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) fail("Volume size overflows.");
    return a * b;
}

class ByteReader {
public:
    ByteReader(const QString &path, qint64 compressedOffset)
        : file(path), hash(QCryptographicHash::Sha256), compressed(compressedOffset >= 0) {
        if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
        initialSize = file.size();
        initialModified = QFileInfo(file).lastModified();
        if (compressed) {
            qint64 remaining = compressedOffset;
            while (remaining) {
                const auto bytes = file.read(std::min<qint64>(remaining, blockBytes));
                if (bytes.isEmpty()) fail("Truncated volume header.");
                hash.addData(bytes);
                remaining -= bytes.size();
            }
            if (inflateInit2(&stream, 15 + 16) != Z_OK) fail("Cannot initialize gzip decompression.");
        }
    }
    ~ByteReader() { if (compressed) inflateEnd(&stream); }
    std::size_t read(unsigned char *destination, std::size_t count) {
        if (!compressed) {
            const qint64 bytes = file.read(reinterpret_cast<char *>(destination), count);
            if (bytes < 0) fail(file.errorString());
            hash.addData(reinterpret_cast<const char *>(destination), bytes);
            return bytes;
        }
        std::size_t produced = 0;
        while (produced < count && !ended) {
            if (!stream.avail_in) {
                const auto bytes = file.read(reinterpret_cast<char *>(input.data()), input.size());
                if (bytes < 0) fail(file.errorString());
                if (!bytes) fail("Truncated gzip volume.");
                hash.addData(reinterpret_cast<const char *>(input.data()), bytes);
                stream.next_in = input.data();
                stream.avail_in = bytes;
            }
            stream.next_out = destination + produced;
            stream.avail_out = static_cast<uInt>(std::min<std::size_t>(count - produced, std::numeric_limits<uInt>::max()));
            const auto available = stream.avail_out;
            const int status = inflate(&stream, Z_NO_FLUSH);
            produced += available - stream.avail_out;
            if (status == Z_STREAM_END) {
                ended = true;
                if (stream.avail_in || !file.atEnd()) fail("Concatenated or trailing gzip streams are not supported for region I/O.");
            } else if (status != Z_OK) {
                fail("Invalid gzip volume or checksum.");
            }
        }
        return produced;
    }
    void exact(unsigned char *destination, std::size_t count) {
        std::size_t offset = 0;
        while (offset < count) {
            const auto bytes = read(destination + offset, count - offset);
            if (!bytes) fail("Truncated volume data.");
            offset += bytes;
        }
    }
    QByteArray fingerprint() const {
        QFileInfo current(file.fileName());
        if (current.size() != initialSize || current.lastModified() != initialModified)
            fail("The volume changed while it was being read. Please load it again.");
        return hash.result().toHex();
    }
private:
    QFile file;
    QCryptographicHash hash;
    bool compressed;
    bool ended = false;
    qint64 initialSize;
    QDateTime initialModified;
    z_stream stream{};
    std::array<unsigned char, 128 * 1024> input{};
};

class ByteWriter {
public:
    ByteWriter(QSaveFile &destination, bool gzip, QCryptographicHash &outputHash)
        : file(destination), hash(outputHash), compressed(gzip) {
        if (compressed && deflateInit2(&stream, 1, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            fail("Cannot initialize gzip compression.");
    }
    ~ByteWriter() { if (compressed) deflateEnd(&stream); }
    void write(const unsigned char *data, std::size_t size) {
        if (!compressed) { raw(data, size); return; }
        stream.next_in = const_cast<Bytef *>(data);
        stream.avail_in = static_cast<uInt>(size);
        while (stream.avail_in) pump(Z_NO_FLUSH);
    }
    void finish() {
        if (compressed) while (pump(Z_FINISH) != Z_STREAM_END) {}
    }
private:
    void raw(const unsigned char *data, std::size_t size) {
        if (file.write(reinterpret_cast<const char *>(data), size) != static_cast<qint64>(size)) fail(file.errorString());
        hash.addData(reinterpret_cast<const char *>(data), size);
    }
    int pump(int flush) {
        stream.next_out = output.data();
        stream.avail_out = output.size();
        const int status = deflate(&stream, flush);
        if (status != Z_OK && status != Z_STREAM_END) fail("Failed to compress volume.");
        raw(output.data(), output.size() - stream.avail_out);
        return status;
    }
    QSaveFile &file;
    QCryptographicHash &hash;
    bool compressed;
    z_stream stream{};
    std::array<unsigned char, 128 * 1024> output{};
};

struct Layout {
    itk::ImageIOBase::Pointer info;
    Coordinates size{};
    QByteArray rawPrefix;
    qint64 compressedOffset = -1;
    std::uint64_t payloadOffset = 0;
    std::size_t pixelBytes = 0;
    bool swapBytes = false;
    bool integer = true;
    bool signedInteger = false;
    bool scaled = false;
};

template<typename Value>
Value headerValue(const QByteArray &header, int offset, bool swap) {
    Value value;
    std::memcpy(&value, header.constData() + offset, sizeof(Value));
    if (swap) std::reverse(reinterpret_cast<unsigned char *>(&value), reinterpret_cast<unsigned char *>(&value) + sizeof(Value));
    return value;
}

std::optional<Layout> inspectLayout(const QString &path) {
    const QString suffix = path.toLower();
    const bool nrrd = suffix.endsWith(".nrrd");
    const bool nifti = suffix.endsWith(".nii") || suffix.endsWith(".nii.gz");
    if (!nrrd && !nifti) return std::nullopt;
    Layout layout;
    layout.info = imageInformation(path);
    if (layout.info->GetNumberOfDimensions() != 3 || layout.info->GetPixelType() != itk::IOPixelEnum::SCALAR ||
        layout.info->GetNumberOfComponents() != 1) return std::nullopt;
    layout.size = dimensions(*layout.info);
    layout.pixelBytes = layout.info->GetComponentSize();
    const auto type = layout.info->GetComponentType();
    layout.integer = type != itk::IOComponentEnum::FLOAT && type != itk::IOComponentEnum::DOUBLE;
    layout.signedInteger = type == itk::IOComponentEnum::CHAR || type == itk::IOComponentEnum::SHORT ||
        type == itk::IOComponentEnum::INT || type == itk::IOComponentEnum::LONG || type == itk::IOComponentEnum::LONGLONG;
    if (nrrd) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
        QByteArray encoding;
        QByteArray endian;
        bool complete = false;
        constexpr qsizetype maximumHeaderBytes = 64 * 1024 * 1024;
        while (!file.atEnd() && layout.rawPrefix.size() < maximumHeaderBytes) {
            const auto line = file.readLine(maximumHeaderBytes - layout.rawPrefix.size());
            layout.rawPrefix += line;
            const auto trimmed = line.trimmed();
            if (trimmed.isEmpty()) { complete = true; break; }
            if (trimmed.startsWith('#')) continue;
            const int colon = trimmed.indexOf(':');
            if (colon < 0 || trimmed.mid(colon, 2) == ":=") continue;
            const auto key = trimmed.left(colon).trimmed().toLower();
            const auto value = trimmed.mid(colon + 1).trimmed().toLower();
            if (key == "encoding") encoding = value;
            if (key == "endian") endian = value;
            if (key == "data file" || key == "datafile") return std::nullopt;
            if ((key == "byte skip" || key == "byteskip" || key == "line skip" || key == "lineskip") && value != "0")
                return std::nullopt;
        }
        if (!complete) fail("Missing or excessively large NRRD header.");
        if (encoding == "gzip" || encoding == "gz") {
            layout.compressedOffset = layout.rawPrefix.size();
        } else if (encoding == "raw") {
            layout.payloadOffset = layout.rawPrefix.size();
            layout.rawPrefix.clear();
        } else return std::nullopt;
        layout.swapBytes = layout.pixelBytes > 1 &&
            ((endian == "little") != (Q_BYTE_ORDER == Q_LITTLE_ENDIAN));
    } else {
        if (suffix.endsWith(".gz")) layout.compressedOffset = 0;
        ByteReader reader(path, layout.compressedOffset);
        QByteArray header(352, '\0');
        reader.exact(reinterpret_cast<unsigned char *>(header.data()), header.size());
        const auto magic = headerValue<std::int32_t>(header, 0, false);
        layout.swapBytes = magic != 348;
        if (headerValue<std::int32_t>(header, 0, layout.swapBytes) != 348 || header.mid(344, 4) != QByteArray("n+1\0", 4))
            return std::nullopt;
        const float offset = headerValue<float>(header, 108, layout.swapBytes);
        if (!std::isfinite(offset) || offset < 352 || offset != std::floor(offset) || offset >= std::ldexp(1.0, 63))
            fail("Invalid NIfTI data offset.");
        layout.payloadOffset = static_cast<std::uint64_t>(offset);
        const float slope = headerValue<float>(header, 112, layout.swapBytes);
        const float intercept = headerValue<float>(header, 116, layout.swapBytes);
        layout.scaled = slope != 0 && (slope != 1 || intercept != 0);
        // ITK may promote scaled data to float; raw payload width is the header's bitpix.
        layout.pixelBytes = headerValue<std::int16_t>(header, 72, layout.swapBytes) / 8;
    }
    if (layout.pixelBytes != 1 && layout.pixelBytes != 2 && layout.pixelBytes != 4 && layout.pixelBytes != 8)
        return std::nullopt;
    multiply(voxelCount(layout.size), layout.pixelBytes);
    return layout;
}

using BlockVisitor = std::function<void(unsigned char *, std::uint64_t, std::size_t)>;

QByteArray visitPayload(const QString &path, const Layout &layout, const BlockVisitor &visit,
                       QSaveFile *destination = nullptr, QByteArray *writtenFingerprint = nullptr) {
    ByteReader input(path, layout.compressedOffset);
    std::unique_ptr<ByteWriter> output;
    QCryptographicHash outputHash(QCryptographicHash::Sha256);
    if (destination) {
        if (!layout.rawPrefix.isEmpty() && destination->write(layout.rawPrefix) != layout.rawPrefix.size())
            fail(destination->errorString());
        outputHash.addData(layout.rawPrefix);
        output = std::make_unique<ByteWriter>(*destination, layout.compressedOffset >= 0, outputHash);
    }
    std::vector<unsigned char> block(blockBytes);
    std::uint64_t prefixRemaining = layout.payloadOffset;
    while (prefixRemaining) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(prefixRemaining, block.size()));
        input.exact(block.data(), count);
        if (output) output->write(block.data(), count);
        prefixRemaining -= count;
    }
    const auto total = voxelCount(layout.size);
    for (std::uint64_t index = 0; index < total;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(total - index, block.size() / layout.pixelBytes));
        input.exact(block.data(), count * layout.pixelBytes);
        visit(block.data(), index, count);
        if (output) output->write(block.data(), count * layout.pixelBytes);
        index += count;
    }
    for (std::size_t count; (count = input.read(block.data(), block.size())) != 0;) {
        if (output) output->write(block.data(), count);
    }
    if (output) output->finish();
    if (writtenFingerprint) *writtenFingerprint = outputHash.result().toHex();
    return input.fingerprint();
}

std::uint64_t integerValue(const unsigned char *bytes, const Layout &layout) {
    std::array<unsigned char, 8> data{};
    std::copy_n(bytes, layout.pixelBytes, data.data());
    if (layout.swapBytes) std::reverse(data.begin(), data.begin() + layout.pixelBytes);
    std::uint64_t value = 0;
    switch (layout.pixelBytes) {
        case 1: value = data[0]; break;
        case 2: { std::uint16_t number; std::memcpy(&number, data.data(), 2); value = number; break; }
        case 4: { std::uint32_t number; std::memcpy(&number, data.data(), 4); value = number; break; }
        case 8: std::memcpy(&value, data.data(), 8); break;
    }
    if (layout.signedInteger && (value & (std::uint64_t(1) << (layout.pixelBytes * 8 - 1))))
        fail("Negative segmentation labels are not supported.");
    return value;
}

void storeInteger(unsigned char *bytes, std::uint64_t value, const Layout &layout) {
    switch (layout.pixelBytes) {
        case 1: { const auto number = static_cast<std::uint8_t>(value); std::memcpy(bytes, &number, 1); break; }
        case 2: { const auto number = static_cast<std::uint16_t>(value); std::memcpy(bytes, &number, 2); break; }
        case 4: { const auto number = static_cast<std::uint32_t>(value); std::memcpy(bytes, &number, 4); break; }
        case 8: std::memcpy(bytes, &value, 8); break;
    }
    if (layout.swapBytes) std::reverse(bytes, bytes + layout.pixelBytes);
}

// Visit contiguous intersections of a flat I/O block with the crop's X rows.
template<typename Visitor>
void intersectRows(std::uint64_t first, std::size_t count, const Coordinates &full,
                   const CropRegion &crop, Visitor visit) {
    const std::uint64_t end = first + count;
    const std::uint64_t plane = multiply(full[0], full[1]);
    const auto firstZ = std::max(crop.start[2], first / plane);
    const auto lastZ = std::min(crop.start[2] + crop.size[2], (end - 1) / plane + 1);
    for (auto z = firstZ; z < lastZ; ++z) {
        const auto firstY = std::max(crop.start[1], first > z * plane ? (first - z * plane) / full[0] : 0);
        const auto lastY = std::min(crop.start[1] + crop.size[1], end > (z + 1) * plane ? full[1] : (end - 1 - z * plane) / full[0] + 1);
        for (auto y = firstY; y < lastY; ++y) {
            const auto row = z * plane + y * full[0] + crop.start[0];
            const auto begin = std::max(first, row);
            const auto finish = std::min(end, row + crop.size[0]);
            if (begin < finish) {
                const auto cropIndex = ((z - crop.start[2]) * crop.size[1] + y - crop.start[1]) * crop.size[0] + begin - row;
                visit(static_cast<std::size_t>(begin - first), cropIndex, static_cast<std::size_t>(finish - begin));
            }
        }
    }
}

QJsonArray coordinateJson(const Coordinates &values) {
    QJsonArray array;
    for (auto value : values) array.append(QString::number(value));
    return array;
}

std::uint64_t unsignedJson(const QJsonValue &value) {
    bool valid = false;
    const auto number = value.toString().toULongLong(&valid);
    if (!valid) fail("Invalid crop recovery number.");
    return number;
}

Coordinates parseCoordinates(const QJsonValue &value) {
    const auto array = value.toArray();
    if (array.size() != 3) fail("Invalid crop recovery coordinates.");
    return {{unsignedJson(array[0]), unsignedJson(array[1]), unsignedJson(array[2])}};
}

Label parseLabel(const QJsonValue &value) {
    const auto label = unsignedJson(value);
    if (label > std::numeric_limits<Label>::max()) fail("Invalid recovery label.");
    return static_cast<Label>(label);
}

} // namespace

QString canonicalPath(const QString &path) {
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    return canonical.isEmpty() ? file.absoluteFilePath() : canonical;
}

QByteArray fileFingerprint(const QString &path) {
    ByteReader reader(path, -1);
    std::vector<unsigned char> buffer(blockBytes);
    while (reader.read(buffer.data(), buffer.size())) {}
    return reader.fingerprint();
}

itk::ImageIOBase::Pointer imageInformation(const QString &path) {
    auto info = itk::ImageIOFactory::CreateImageIO(path.toStdString().c_str(), itk::IOFileModeEnum::ReadMode);
    if (!info) fail("No image reader is available for " + path);
    info->SetFileName(path.toStdString());
    info->ReadImageInformation();
    return info;
}

Coordinates dimensions(const itk::ImageIOBase &info) {
    if (info.GetNumberOfDimensions() != 2 && info.GetNumberOfDimensions() != 3)
        fail("Only 2D and 3D volumes are supported.");
    Coordinates size{{1, 1, 1}};
    for (unsigned int axis = 0; axis < info.GetNumberOfDimensions(); ++axis) size[axis] = info.GetDimensions(axis);
    return size;
}

std::uint64_t voxelCount(const Coordinates &size) {
    return multiply(multiply(size[0], size[1]), size[2]);
}

void validateRegion(const CropRegion &region, const Coordinates &size) {
    for (unsigned int axis = 0; axis < 3; ++axis) {
        if (!region.size[axis] || region.start[axis] > size[axis] || region.size[axis] > size[axis] - region.start[axis])
            fail("The requested region does not fit inside the volume.");
        if (region.size[axis] > std::numeric_limits<int>::max() || region.start[axis] > std::numeric_limits<itk::IndexValueType>::max())
            fail("The requested region exceeds supported image coordinates.");
    }
    if (voxelCount(region.size) > std::numeric_limits<std::size_t>::max()) fail("The crop is too large.");
}

bool supportsBoundedRead(const QString &path, const CropRegion &region) {
    auto layout = inspectLayout(path);
    if (layout && !layout->scaled) { validateRegion(region, layout->size); return true; }
    auto info = imageInformation(path);
    validateRegion(region, dimensions(*info));
    info->SetUseStreamedReading(true);
    itk::ImageIORegion requested(info->GetNumberOfDimensions());
    for (unsigned int axis = 0; axis < info->GetNumberOfDimensions(); ++axis) {
        requested.SetIndex(axis, region.start[axis]);
        requested.SetSize(axis, region.size[axis]);
    }
    const auto actual = info->GenerateStreamableReadRegionFromRequestedRegion(requested);
    // Some readers expand crops to whole planes; cap the extra read size.
    const auto bytes = multiply(actual.GetNumberOfPixels(), info->GetComponentSize() * info->GetNumberOfComponents());
    const auto cropBytes = multiply(voxelCount(region.size), info->GetComponentSize() * info->GetNumberOfComponents());
    return actual == requested || (info->CanStreamRead() && bytes <= std::max<std::uint64_t>(cropBytes, 64 * 1024 * 1024));
}

std::shared_ptr<VolumeContext> makeContext(const QString &path, const itk::ImageIOBase &info,
                                          const ImageLoadOptions &options) {
    auto context = std::make_shared<VolumeContext>();
    context->sourcePath = canonicalPath(path);
    context->originalSize = dimensions(info);
    context->crop = options.crop;
    for (unsigned int axis = 0; axis < info.GetNumberOfDimensions(); ++axis) {
        context->spacing[axis] = info.GetSpacing(axis);
        context->origin[axis] = info.GetOrigin(axis);
        for (unsigned int row = 0; row < info.GetNumberOfDimensions(); ++row)
            context->direction[row * 3 + axis] = info.GetDirection(axis)[row];
    }
    return context;
}

std::optional<RegionData> readRegion(const QString &path, const ImageLoadOptions &options) {
    auto layout = inspectLayout(path);
    if (!layout) return std::nullopt;
    if (layout->scaled) {
        if (options.labelImage && options.crop) fail("Region label I/O requires identity NIfTI value scaling.");
        return std::nullopt;
    }
    if (options.labelImage && !layout->integer) {
        if (options.crop) fail("Region label I/O requires an integer source volume.");
        return std::nullopt;
    }
    RegionData result;
    result.information = layout->info;
    result.region = options.crop.value_or(CropRegion{{{0, 0, 0}}, layout->size});
    validateRegion(result.region, layout->size);
    const auto bytes = multiply(voxelCount(result.region.size), layout->pixelBytes);
    if (bytes > result.pixels.max_size()) fail("The crop is too large to allocate.");
    result.pixels.resize(bytes);
    result.context = makeContext(path, *layout->info, options);
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0;
    result.context->sourceFingerprint = visitPayload(path, *layout, [&](unsigned char *block, std::uint64_t first, std::size_t count) {
        if (options.labelImage) {
            for (std::size_t index = 0; index < count; ++index) {
                const auto value = integerValue(block + index * layout->pixelBytes, *layout);
                if (value > std::numeric_limits<Label>::max()) fail("Label values exceed the supported 32-bit range.");
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
        intersectRows(first, count, layout->size, result.region, [&](std::size_t blockIndex, std::uint64_t cropIndex, std::size_t length) {
            auto *destination = result.pixels.data() + cropIndex * layout->pixelBytes;
            std::memcpy(destination, block + blockIndex * layout->pixelBytes, length * layout->pixelBytes);
            if (layout->swapBytes) for (std::size_t index = 0; index < length; ++index)
                std::reverse(destination + index * layout->pixelBytes, destination + (index + 1) * layout->pixelBytes);
        });
    });
    if (options.labelImage) result.context->background = static_cast<Label>(options.backgroundIsHighest ? maximum : minimum);
    return result;
}

void initializeLabels(VolumeContext &context, const Label *pixels, std::size_t count,
                      bool rememberOriginalLabels) {
    if (!context.background) context.background = 0;
    context.nextLabel = std::max(context.nextLabel, segment_puzzler::scanLabels(
        pixels, count, *context.background, rememberOriginalLabels ? &context.originalLabels : nullptr));
}

std::uint64_t nextAvailableLabel(std::uint64_t next, const std::unordered_set<Label> &reserved) {
    const auto limit = std::numeric_limits<Label>::max();
    while (next <= limit && reserved.count(static_cast<Label>(next))) ++next;
    if (next > limit) throw std::overflow_error("No free segment IDs remain.");
    return next;
}

Label firstLabelForReservation(const VolumeContext &context, std::size_t count) {
    auto next = context.nextLabel;
    if (context.background && next <= *context.background && count > std::uint64_t(*context.background) - next)
        next = std::uint64_t(*context.background) + 1;
    const std::uint64_t limit = std::numeric_limits<Label>::max();
    if (!count || next > limit || count > limit - next + 1)
        throw std::overflow_error("No free segment IDs remain.");
    return static_cast<Label>(next);
}

Label reserveLabels(VolumeContext &context, std::size_t count) {
    const auto first = firstLabelForReservation(context, count);
    context.nextLabel = std::uint64_t(first) + count;
    return first;
}

void useLabel(VolumeContext &context, Label label) {
    if (label != context.background.value_or(0))
        context.nextLabel = std::max(context.nextLabel, std::uint64_t(label) + 1);
}

void validateLabelSource(const QString &path, const itk::ImageIOBase &info, bool allowFullRead) {
    using Type = itk::IOComponentEnum;
    const auto type = info.GetComponentType();
    // Check for data loss when casting signed, floating-point or wider types.
    if (type == Type::UCHAR || type == Type::USHORT || type == Type::UINT) return;
    if (auto layout = inspectLayout(path); layout && layout->integer && !layout->scaled) {
        visitPayload(path, *layout, [&](unsigned char *block, std::uint64_t, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i)
                if (integerValue(block + i * layout->pixelBytes, *layout) > std::numeric_limits<Label>::max())
                    fail("Label values exceed the supported non-negative 32-bit range.");
        });
        return;
    }
    using Image = itk::Image<double, 3>;
    auto reader = itk::ImageFileReader<Image>::New();
    reader->SetFileName(path.toStdString());
    reader->UseStreamingOn();
    reader->UpdateOutputInformation();
    const auto full = dimensions(info);
    const bool streaming = reader->GetImageIO()->CanStreamRead();
    if (!streaming && !allowFullRead) fail("Validating labels requires the full-read fallback for this format.");
    const auto depth = streaming ? std::uint64_t(1) : full[2];
    for (std::uint64_t z = 0; z < full[2]; z += depth) {
        Image::RegionType region;
        region.SetIndex({{0, 0, static_cast<Image::IndexValueType>(z)}});
        region.SetSize({{full[0], full[1], std::min(depth, full[2] - z)}});
        reader->GetOutput()->SetRequestedRegion(region);
        reader->Update();
        const auto *image = reader->GetOutput();
        const auto *pixels = image->GetBufferPointer();
        for (std::size_t i = 0; i < image->GetBufferedRegion().GetNumberOfPixels(); ++i)
            if (!std::isfinite(pixels[i]) || pixels[i] < 0 || pixels[i] > std::numeric_limits<Label>::max() || std::floor(pixels[i]) != pixels[i])
                fail("Label values must be non-negative 32-bit integers.");
    }
}

std::pair<Label, Label> sourceLabelRange(const QString &path, bool allowFullRead) {
    const auto info = imageInformation(path);
    const auto full = dimensions(*info);
    Coordinates tile{{std::min<std::uint64_t>(full[0], 1024), std::min<std::uint64_t>(full[1], 128), 8}};
    tile[2] = std::min(tile[2], full[2]);
    if (!supportsBoundedRead(path, {{{0, 0, 0}}, tile})) {
        if (!allowFullRead) fail("Determining the source background requires a full read for this format. Confirm the loading fallback first.");
        tile = full;
    }
    auto reader = itk::ImageFileReader<dataType::SegmentsImageType>::New();
    reader->SetFileName(path.toStdString());
    reader->UseStreamingOn();
    reader->UpdateOutputInformation();
    std::pair<Label, Label> range{std::numeric_limits<Label>::max(), 0};
    for (std::uint64_t z = 0; z < full[2]; z += tile[2])
        for (std::uint64_t y = 0; y < full[1]; y += tile[1])
            for (std::uint64_t x = 0; x < full[0]; x += tile[0]) {
                const Coordinates start{{x, y, z}};
                dataType::SegmentsImageType::RegionType region;
                for (unsigned int axis = 0; axis < 3; ++axis) {
                    region.SetIndex(axis, start[axis]);
                    region.SetSize(axis, std::min(tile[axis], full[axis] - start[axis]));
                }
                reader->GetOutput()->SetRequestedRegion(region);
                reader->Update();
                const auto *image = reader->GetOutput();
                const auto *pixels = image->GetBufferPointer();
                const auto extrema = std::minmax_element(pixels, pixels + image->GetBufferedRegion().GetNumberOfPixels());
                range.first = std::min(range.first, *extrema.first);
                range.second = std::max(range.second, *extrema.second);
            }
    return range;
}

const TargetBinding *findBinding(const VolumeContext &context, const QString &path,
                                const Coordinates &start, LabelPolicy policy, Label background) {
    const auto canonical = canonicalPath(path);
    for (const auto &binding : context.targets)
        if (binding.path == canonical && binding.start == start && binding.policy == policy && binding.background == background)
            return &binding;
    return nullptr;
}

RegionWriteResult writeRegionToVolume(const dataType::SegmentsImageType *image, const QString &targetPath,
                                      const Coordinates &targetStart, const RegionWriteOptions &options) {
    if (!image || !image->GetBufferPointer() || image->GetBufferedRegion() != image->GetLargestPossibleRegion())
        fail("An independently buffered image is required for region export.");
    const QString path = canonicalPath(targetPath);
    QLockFile lock(path + ".segmentpuzzler.lock");
    if (!lock.tryLock(0)) fail("Another SegmentPuzzler operation is writing this volume.");
    const auto layout = inspectLayout(path);
    if (!layout || !layout->integer || layout->scaled)
        fail("Insertion requires a scalar 3D integer NRRD (raw/gzip, single file) or NIfTI-1 (.nii/.nii.gz) with identity scaling.");
    CropRegion crop;
    crop.start = targetStart;
    for (unsigned int axis = 0; axis < 3; ++axis) crop.size[axis] = image->GetBufferedRegion().GetSize(axis);
    validateRegion(crop, layout->size);
    std::uint64_t maximum = 0;
    const auto fingerprint = visitPayload(path, *layout, [&](unsigned char *data, std::uint64_t, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = integerValue(data + index * layout->pixelBytes, *layout);
            if (value != options.targetBackground) maximum = std::max(maximum, value);
        }
    });
    const auto context = options.context ? options.context : std::make_shared<VolumeContext>();
    if (path == context->sourcePath && !context->sourceFingerprint.isEmpty() && fingerprint != context->sourceFingerprint)
        fail("The source volume has changed since it was loaded. Reload it before inserting.");
    const auto *previous = findBinding(*context, path, targetStart, options.labelPolicy, options.targetBackground);
    if (previous && previous->fingerprint != fingerprint) fail("The previous insertion target has changed. Its saved ID mapping is no longer valid.");
    // Keep IDs reserved even after another insertion has overwritten their last voxels.
    for (const auto &binding : context->targets) {
        if (binding.path != path) continue;
        if (binding.fingerprint != fingerprint) fail("An earlier insertion target has changed. Reload it before reusing its ID mappings.");
        for (const auto &entry : binding.labels)
            if (entry.second != binding.background) maximum = std::max<std::uint64_t>(maximum, entry.second);
    }
    RegionWriteResult result;
    result.binding = {path, targetStart, options.labelPolicy, options.targetBackground, {}, {}};
    if (previous) result.binding.labels = previous->labels;
    const auto *source = image->GetBufferPointer();
    const auto count = image->GetBufferedRegion().GetNumberOfPixels();
    std::unordered_set<Label> uniqueLabels;
    segment_puzzler::scanLabels(source, count, context->background.value_or(0), &uniqueLabels);
    std::vector<Label> labels(uniqueLabels.begin(), uniqueLabels.end());
    std::sort(labels.begin(), labels.end());
    const auto isBackground = [&](Label label) { return label == context->background.value_or(0); };
    if (options.labelPolicy == LabelPolicy::SameSegmentation) {
        for (const auto label : context->originalLabels)
            if (!isBackground(label)) maximum = std::max<std::uint64_t>(maximum, label);
    }
    std::uint64_t limit = layout->pixelBytes == 8 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t(1) << (layout->pixelBytes * 8)) - 1;
    if (layout->signedInteger) limit >>= 1;
    limit = std::min<std::uint64_t>(limit, std::numeric_limits<Label>::max());
    if (options.targetBackground > limit) fail("The target background does not fit its pixel type.");
    for (const auto label : labels) {
        if (isBackground(label)) {
            result.binding.labels[label] = options.targetBackground;
        } else if (options.labelPolicy == LabelPolicy::SameSegmentation && context->originalLabels.count(label)) {
            if (label == options.targetBackground) fail("An object ID equals the target background. Choose an independent target mapping.");
            result.binding.labels[label] = label;
            maximum = std::max<std::uint64_t>(maximum, label);
        }
    }
    for (const auto &entry : result.binding.labels) {
        if (entry.second > limit) fail("A retained label does not fit the target pixel type.");
        if (entry.second != options.targetBackground) maximum = std::max<std::uint64_t>(maximum, entry.second);
    }
    for (const auto label : labels) {
        if (result.binding.labels.count(label)) continue;
        if (maximum >= limit) fail("The target pixel type has no free segment IDs. Convert the full target to a wider integer type first.");
        ++maximum;
        if (maximum == options.targetBackground) {
            if (maximum >= limit) fail("The target pixel type has no free segment IDs.");
            ++maximum;
        }
        result.binding.labels[label] = static_cast<Label>(maximum);
    }
    QSaveFile destination(path);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)) fail(destination.errorString());
    const auto copiedFingerprint = visitPayload(path, *layout, [&](unsigned char *block, std::uint64_t first, std::size_t length) {
        intersectRows(first, length, layout->size, crop, [&](std::size_t blockIndex, std::uint64_t cropIndex, std::size_t rowLength) {
            for (std::size_t index = 0; index < rowLength; ++index)
                storeInteger(block + (blockIndex + index) * layout->pixelBytes, result.binding.labels.at(source[cropIndex + index]), *layout);
        });
    }, &destination, &result.binding.fingerprint);
    if (copiedFingerprint != fingerprint || fileFingerprint(path) != fingerprint)
        fail("The target changed while insertion was prepared. The target was not replaced.");
    if (!destination.commit()) fail(destination.errorString());
    return result;
}

void recordWrite(VolumeContext &context, const RegionWriteResult &result) {
    const auto &saved = result.binding;
    for (auto &binding : context.targets) {
        if (binding.path == saved.path) binding.fingerprint = saved.fingerprint;
    }
    auto found = std::find_if(context.targets.begin(), context.targets.end(), [&](const TargetBinding &binding) {
        return binding.path == saved.path && binding.start == saved.start && binding.policy == saved.policy && binding.background == saved.background;
    });
    if (found == context.targets.end()) context.targets.push_back(saved); else *found = saved;
    if (context.sourcePath == saved.path) context.sourceFingerprint = saved.fingerprint;
}

std::shared_ptr<VolumeContext> derivedContext(const std::shared_ptr<VolumeContext> &source) {
    auto result = source ? std::make_shared<VolumeContext>(*source) : std::make_shared<VolumeContext>();
    result->originalLabels.clear();
    result->nextLabel = 1;
    result->targets.clear();
    result->background = 0;
    return result;
}

QString contextDescription(const VolumeContext &context) {
    if (!context.crop) return {};
    const auto &crop = *context.crop;
    return QStringLiteral("Crop: start [%1, %2, %3], size [%4, %5, %6]\nSource: %7\nEdits, object sizes and neighborhoods apply only inside this crop.")
        .arg(crop.start[0]).arg(crop.start[1]).arg(crop.start[2])
        .arg(crop.size[0]).arg(crop.size[1]).arg(crop.size[2]).arg(context.sourcePath);
}

std::string serializeContext(const VolumeContext &context) {
    QJsonObject json{{"version", 1}, {"source", context.sourcePath}, {"size", coordinateJson(context.originalSize)},
                     {"fingerprint", QString::fromLatin1(context.sourceFingerprint)}, {"nextLabel", QString::number(context.nextLabel)}};
    if (context.crop) json["crop"] = QJsonObject{{"start", coordinateJson(context.crop->start)}, {"size", coordinateJson(context.crop->size)}};
    if (context.background) json["background"] = QString::number(*context.background);
    QJsonArray geometry;
    for (const auto value : context.spacing) geometry.append(value);
    for (const auto value : context.origin) geometry.append(value);
    for (const auto value : context.direction) geometry.append(value);
    json["geometry"] = geometry;
    QJsonArray labels;
    std::set<Label> ordered(context.originalLabels.begin(), context.originalLabels.end());
    for (const auto label : ordered) labels.append(QString::number(label));
    json["originalLabels"] = labels;
    QJsonArray targets;
    for (const auto &binding : context.targets) {
        QJsonArray mapping;
        for (const auto &entry : binding.labels) mapping.append(QJsonArray{QString::number(entry.first), QString::number(entry.second)});
        targets.append(QJsonObject{{"path", binding.path}, {"start", coordinateJson(binding.start)},
                                   {"same", binding.policy == LabelPolicy::SameSegmentation},
                                   {"background", QString::number(binding.background)},
                                   {"fingerprint", QString::fromLatin1(binding.fingerprint)}, {"labels", mapping}});
    }
    json["targets"] = targets;
    return QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();
}

std::shared_ptr<VolumeContext> deserializeContext(const std::string &value) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(value), &error);
    const auto json = document.object();
    if (error.error != QJsonParseError::NoError || json["version"].toInt() != 1) fail("Invalid or unsupported crop recovery metadata.");
    auto context = std::make_shared<VolumeContext>();
    context->sourcePath = json["source"].toString();
    context->originalSize = parseCoordinates(json["size"]);
    context->sourceFingerprint = json["fingerprint"].toString().toLatin1();
    context->nextLabel = unsignedJson(json["nextLabel"]);
    if (context->nextLabel == 0 || context->nextLabel > std::uint64_t(std::numeric_limits<Label>::max()) + 1)
        fail("Invalid recovery label allocator.");
    if (json.contains("background")) context->background = parseLabel(json["background"]);
    if (json.contains("crop")) {
        const auto crop = json["crop"].toObject();
        context->crop = CropRegion{parseCoordinates(crop["start"]), parseCoordinates(crop["size"])};
        validateRegion(*context->crop, context->originalSize);
    }
    const auto geometry = json["geometry"].toArray();
    if (geometry.size() != 15) fail("Invalid crop recovery geometry.");
    for (int index = 0; index < 3; ++index) {
        context->spacing[index] = geometry[index].toDouble();
        context->origin[index] = geometry[index + 3].toDouble();
    }
    for (int index = 0; index < 9; ++index) context->direction[index] = geometry[index + 6].toDouble();
    for (const auto label : json["originalLabels"].toArray()) context->originalLabels.insert(parseLabel(label));
    for (const auto value : json["targets"].toArray()) {
        const auto target = value.toObject();
        TargetBinding binding;
        binding.path = target["path"].toString();
        binding.start = parseCoordinates(target["start"]);
        binding.policy = target["same"].toBool() ? LabelPolicy::SameSegmentation : LabelPolicy::Independent;
        binding.background = parseLabel(target["background"]);
        binding.fingerprint = target["fingerprint"].toString().toLatin1();
        for (const auto entry : target["labels"].toArray()) {
            const auto pair = entry.toArray();
            if (pair.size() != 2) fail("Invalid recovery label mapping.");
            binding.labels.emplace(parseLabel(pair[0]), parseLabel(pair[1]));
        }
        context->targets.push_back(std::move(binding));
    }
    return context;
}

std::shared_ptr<VolumeContext> imageContext(const itk::MetaDataDictionary &dictionary) {
    std::string value;
    if (!itk::ExposeMetaData<std::string>(dictionary, contextKey, value)) return {};
    return deserializeContext(value);
}

void setImageContext(itk::MetaDataDictionary &dictionary, const VolumeContext &context) {
    itk::EncapsulateMetaData<std::string>(dictionary, contextKey, serializeContext(context));
}

void clearImageContext(itk::MetaDataDictionary &dictionary) {
    dictionary.Erase(contextKey);
}

} // namespace volume_io
