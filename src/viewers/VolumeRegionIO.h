#pragma once

#include "VolumeContext.h"
#include "src/utils/AppLogger.h"
#include "src/utils/MinMaxValues.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <itkImageFileReader.h>
#include <itkImageIOFactory.h>
#include <itkMetaDataObject.h>
#include <itkRegionOfInterestImageFilter.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace volume_io {

template<typename Image>
struct LoadedVolume {
    typename Image::Pointer image;
    std::shared_ptr<VolumeContext> context;
};

LoadedVolume<dataType::SegmentsStorage> readLabelStorage(const QString &path, ImageLoadOptions options);
// The caller keeps the image unchanged until writing completes.
QByteArray writeLabelVolume(const dataType::SegmentsStorage &image, const QString &path,
                           const VolumeContext *recoveryContext = nullptr, bool compressed = true);
void configureImageRegion(itk::ImageBase<3> &image, const itk::ImageIOBase &info, const CropRegion &region);

QString canonicalPath(const QString &path);
QByteArray fileFingerprint(const QString &path);
itk::ImageIOBase::Pointer imageInformation(const QString &path);
Coordinates dimensions(const itk::ImageIOBase &info);
void validateRegion(const CropRegion &region, const Coordinates &size);
std::uint64_t voxelCount(const Coordinates &size);
bool supportsBoundedRead(const QString &path, const CropRegion &region);
QString contextDescription(const VolumeContext &context);
std::string serializeContext(const VolumeContext &context);
std::shared_ptr<VolumeContext> deserializeContext(const std::string &json);
std::shared_ptr<VolumeContext> imageContext(const itk::MetaDataDictionary &dictionary);
void setImageContext(itk::MetaDataDictionary &dictionary, const VolumeContext &context);
void clearImageContext(itk::MetaDataDictionary &dictionary);

struct RegionData {
    itk::ImageIOBase::Pointer information;
    CropRegion region;
    std::vector<unsigned char> pixels;
    std::shared_ptr<VolumeContext> context;
};

// nullopt means this format needs the normal ITK reader (and its streaming check).
std::optional<RegionData> readRegion(const QString &path, const ImageLoadOptions &options);
std::shared_ptr<VolumeContext> makeContext(const QString &path, const itk::ImageIOBase &info,
                                           const ImageLoadOptions &options);
std::pair<Label, Label> sourceLabelRange(const QString &path, bool allowFullRead);

struct RegionWriteOptions {
    LabelPolicy labelPolicy = LabelPolicy::Independent;
    Label targetBackground = 0;
    std::shared_ptr<const VolumeContext> context;
};

struct RegionWriteResult {
    TargetBinding binding;
};

RegionWriteResult writeRegionToVolume(const dataType::SegmentsImageType *image,
                                      const QString &targetPath,
                                      const Coordinates &targetStart,
                                      const RegionWriteOptions &options);
RegionWriteResult writeRegionToVolume(const dataType::SegmentsStorage *image,
                                      const QString &targetPath,
                                      const Coordinates &targetStart,
                                      const RegionWriteOptions &options);
void recordWrite(VolumeContext &context, const RegionWriteResult &result);
const TargetBinding *findBinding(const VolumeContext &context, const QString &path,
                                const Coordinates &start, LabelPolicy policy, Label background);

void validateLabelSource(const QString &path, const itk::ImageIOBase &info, bool allowFullRead);

template<typename Image>
LoadedVolume<Image> readWithITK(const QString &path, const ImageLoadOptions &options,
                                   itk::ImageIOBase *customIO = nullptr) {
    QElapsedTimer timer;
    timer.start();
    using Reader = itk::ImageFileReader<Image>;
    auto reader = Reader::New();
    if (customIO) reader->SetImageIO(customIO);
    reader->SetFileName(path.toStdString());
    reader->UseStreamingOn();
    reader->UpdateOutputInformation();
    if (options.labelImage) validateLabelSource(path, *reader->GetImageIO(), options.allowFullRead || !options.crop);
    const auto validationMs = timer.restart();
    typename Image::Pointer result;
    if (options.crop) {
        if (!options.allowFullRead && !supportsBoundedRead(path, *options.crop))
            throw std::runtime_error("This reader requires a full-volume read. Confirm the loading fallback first.");
        validateRegion(*options.crop, dimensions(*reader->GetImageIO()));
        typename Image::RegionType region;
        for (unsigned int axis = 0; axis < 3; ++axis) {
            region.SetIndex(axis, options.crop->start[axis]);
            region.SetSize(axis, options.crop->size[axis]);
        }
        using Extract = itk::RegionOfInterestImageFilter<Image, Image>;
        auto extract = Extract::New();
        extract->SetInput(reader->GetOutput());
        extract->SetRegionOfInterest(region);
        extract->Update();
        result = extract->GetOutput();
    } else {
        reader->Update();
        result = reader->GetOutput();
    }
    result->DisconnectPipeline();
    const auto readMs = timer.restart();
    auto context = !options.crop ? imageContext(reader->GetImageIO()->GetMetaDataDictionary()) : nullptr;
    const bool recovered = context != nullptr;
    qint64 fingerprintMs = 0, rangeMs = 0, labelsMs = 0;
    if (!context) {
        context = makeContext(path, *reader->GetImageIO(), options);
        timer.restart();
        context->sourceFingerprint = fileFingerprint(path);
        fingerprintMs = timer.elapsed();
    }
    timer.restart();
    if constexpr (std::is_same_v<typename Image::PixelType, Label>) {
        if (options.labelImage) {
            if (!recovered) {
                const auto range = options.crop
                    ? sourceLabelRange(path, options.allowFullRead)
                    : segment_puzzler::minMaxValues(result->GetBufferPointer(), result->GetBufferedRegion().GetNumberOfPixels());
                context->background = options.backgroundIsHighest ? range.second : range.first;
                rangeMs = timer.restart();
            }
            initializeLabels(*context, result->GetBufferPointer(), result->GetBufferedRegion().GetNumberOfPixels(), !recovered);
            labelsMs = timer.elapsed();
        }
    }
    clearImageContext(result->GetMetaDataDictionary());
    SP_LOG_INFO("io", QStringLiteral("volume_load path=\"%1\" metadata_validation_ms=%2 itk_read_ms=%3 "
                                    "fingerprint_ms=%4 label_range_ms=%5 label_init_ms=%6 recovered=%7")
        .arg(path).arg(validationMs).arg(readMs).arg(fingerprintMs).arg(rangeMs).arg(labelsMs).arg(recovered));
    return {result, context};
}

template<typename Pixel, typename Source>
void copyPixels(Pixel *destination, const std::vector<unsigned char> &bytes, bool labels) {
    const auto count = bytes.size() / sizeof(Source);
    for (std::size_t index = 0; index < count; ++index) {
        Source value;
        std::memcpy(&value, bytes.data() + index * sizeof(Source), sizeof(Source));
        if (labels && (!std::isfinite(static_cast<long double>(value)) || static_cast<long double>(value) < 0 ||
                       static_cast<long double>(value) > std::numeric_limits<Label>::max()))
            throw std::runtime_error("Label values exceed the supported non-negative 32-bit range.");
        destination[index] = static_cast<Pixel>(value);
    }
}

std::shared_ptr<VolumeContext> readGzipIntensityPixels(const QString &path, itk::ImageIOBase &info,
                                                      const std::function<unsigned char *()> &allocate);

template<typename Pixel>
LoadedVolume<itk::Image<Pixel, 3>> tryReadGzipIntensity(const QString &path) {
    if (!path.endsWith(".nrrd", Qt::CaseInsensitive) && !path.endsWith(".nii.gz", Qt::CaseInsensitive)) return {};

    QElapsedTimer timer;
    timer.start();
    using Image = itk::Image<Pixel, 3>;
    auto reader = itk::ImageFileReader<Image>::New();
    reader->SetFileName(path.toStdString());
    reader->UpdateOutputInformation();
    auto *info = reader->GetImageIO();
    const auto type = info->GetComponentType();
    if (info->GetComponentSize() != sizeof(Pixel) ||
        itk::ImageIOBase::IsComponentTypeFloatingPoint(type) != std::is_floating_point_v<Pixel> ||
        itk::ImageIOBase::IsComponentTypeUnsigned(type) != std::is_unsigned_v<Pixel>)
        return {};

    typename Image::Pointer image = reader->GetOutput();
    auto context = readGzipIntensityPixels(path, *info, [&] {
        image->SetRegions(image->GetLargestPossibleRegion());
        image->Allocate(false);
        return reinterpret_cast<unsigned char *>(image->GetBufferPointer());
    });
    if (!context) return {};
    if constexpr (std::is_floating_point_v<Pixel>) {
        // NIfTI readers may normalize non-finite voxels.
        const auto *pixels = image->GetBufferPointer();
        if (path.endsWith(".nii.gz", Qt::CaseInsensitive) &&
            !std::all_of(pixels, pixels + image->GetBufferedRegion().GetNumberOfPixels(),
                         [](Pixel value) { return std::isfinite(value); }))
            return {};
    }
    image->DisconnectPipeline();
    clearImageContext(image->GetMetaDataDictionary());
    SP_LOG_INFO("io", QStringLiteral("volume_load path=\"%1\" gzip_intensity_read_ms=%2")
        .arg(path).arg(timer.elapsed()));
    return {image, context};
}

template<typename Pixel>
LoadedVolume<itk::Image<Pixel, 3>> readImage(const QString &path, const ImageLoadOptions &options) {
    using Image = itk::Image<Pixel, 3>;
    if (!options.crop && !options.labelImage) {
        auto loaded = tryReadGzipIntensity<Pixel>(path);
        if (loaded.image) return loaded;
    }
    QElapsedTimer timer;
    timer.start();
    auto data = options.crop ? readRegion(path, options) : std::nullopt;
    if (!data) return readWithITK<Image>(path, options);
    const auto readMs = timer.restart();
    auto image = Image::New();
    {
        configureImageRegion(*image, *data->information, data->region);
        image->Allocate();
        auto *destination = image->GetBufferPointer();
        using Type = itk::IOComponentEnum;
        switch (data->information->GetComponentType()) {
            case Type::UCHAR: copyPixels<Pixel, unsigned char>(destination, data->pixels, options.labelImage); break;
            case Type::CHAR: copyPixels<Pixel, signed char>(destination, data->pixels, options.labelImage); break;
            case Type::USHORT: copyPixels<Pixel, unsigned short>(destination, data->pixels, options.labelImage); break;
            case Type::SHORT: copyPixels<Pixel, short>(destination, data->pixels, options.labelImage); break;
            case Type::UINT: copyPixels<Pixel, unsigned int>(destination, data->pixels, options.labelImage); break;
            case Type::INT: copyPixels<Pixel, int>(destination, data->pixels, options.labelImage); break;
            case Type::ULONG: copyPixels<Pixel, unsigned long>(destination, data->pixels, options.labelImage); break;
            case Type::LONG: copyPixels<Pixel, long>(destination, data->pixels, options.labelImage); break;
            case Type::ULONGLONG: copyPixels<Pixel, unsigned long long>(destination, data->pixels, options.labelImage); break;
            case Type::LONGLONG: copyPixels<Pixel, long long>(destination, data->pixels, options.labelImage); break;
            case Type::FLOAT: copyPixels<Pixel, float>(destination, data->pixels, options.labelImage); break;
            case Type::DOUBLE: copyPixels<Pixel, double>(destination, data->pixels, options.labelImage); break;
            default: throw std::runtime_error("Unsupported crop component type.");
        }
    }
    const auto copyMs = timer.restart();
    if constexpr (std::is_same_v<Pixel, Label>) {
        if (options.labelImage)
            initializeLabels(*data->context, image->GetBufferPointer(), image->GetBufferedRegion().GetNumberOfPixels(), true);
    }
    clearImageContext(image->GetMetaDataDictionary());
    SP_LOG_INFO("io", QStringLiteral("volume_load path=\"%1\" region_read_ms=%2 copy_ms=%3 label_init_ms=%4 "
                                    "(region read includes fingerprint and source label range)")
        .arg(path).arg(readMs).arg(copyMs).arg(timer.elapsed()));
    return {image, data->context};
}

} // namespace volume_io
