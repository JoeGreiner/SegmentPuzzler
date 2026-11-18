#include "src/viewers/VolumeRegionIO.h"
#include "src/image_storage/LabelImageStorage.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <itkImageFileWriter.h>
#include <iostream>
#include <random>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <csignal>
#endif

using namespace volume_io;
using Image = dataType::SegmentsImageType;
void checkStoredLabels(const QString &path, const ImageLoadOptions &options);

void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<typename Action> void rejects(Action action, const char *message) {
    try { action(); } catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}

template<typename Pixel>
void save(typename itk::Image<Pixel, 3>::Pointer image, const QString &path, bool compressed = false) {
    auto writer = itk::ImageFileWriter<itk::Image<Pixel, 3>>::New();
    writer->SetFileName(path.toStdString());
    writer->SetInput(image);
    writer->SetUseCompression(compressed);
    writer->Update();
}

Image::Pointer makeImage(const Image::SizeType &size) {
    auto image = Image::New();
    image->SetRegions(size);
    const double spacing[3] = {0.5, 0.75, 2};
    const double origin[3] = {10, -20, 30};
    image->SetSpacing(spacing);
    image->SetOrigin(origin);
    Image::DirectionType direction;
    direction.SetIdentity();
    direction[0][0] = 0; direction[0][1] = -1;
    direction[1][0] = 1; direction[1][1] = 0;
    image->SetDirection(direction);
    image->Allocate();
    image->FillBuffer(0);
    return image;
}

void roundtrip(const QString &directory, const QString &suffix, bool compressed) {
    const QString path = directory + (compressed ? "/source_compressed" : "/source_raw") + suffix;
    auto full = makeImage({{13, 9, 7}});
    for (std::size_t index = 0; index < full->GetBufferedRegion().GetNumberOfPixels(); ++index)
        full->GetBufferPointer()[index] = static_cast<Label>(index % 5 + 1);
    full->SetPixel({{0, 0, 0}}, 0);
    full->SetPixel({{12, 8, 6}}, 1000000);
    save<Label>(full, path, compressed);
    ImageLoadOptions options;
    options.crop = CropRegion{{{3, 2, 1}}, {{5, 4, 3}}};
    options.labelImage = true;
    auto loaded = readImage<Label>(path, options);
    auto crop = loaded.image;
    auto context = loaded.context;
    require(context && context->background == 0, "Background must be taken from outside the crop");
    require(crop->GetBufferedRegion().GetIndex() == Image::IndexType{{0, 0, 0}}, "Crop was not rebased");
    Image::PointType expectedOrigin;
    full->TransformIndexToPhysicalPoint(Image::IndexType{{3, 2, 1}}, expectedOrigin);
    require((crop->GetOrigin() - expectedOrigin).GetNorm() < 1e-5, "Crop physical origin is incorrect");
    for (int z = 0; z < 3; ++z) for (int y = 0; y < 4; ++y) for (int x = 0; x < 5; ++x)
        require(crop->GetPixel({{x, y, z}}) == full->GetPixel({{x + 3, y + 2, z + 1}}), "Wrong crop pixels");
    crop->SetPixel({{1, 1, 1}}, 6); // New local label; insertion must allocate a global ID.
    crop->SetPixel({{0, 0, 0}}, 0);
    RegionWriteOptions write;
    write.context = context;
    write.labelPolicy = LabelPolicy::SameSegmentation;
    const auto result = writeRegionToVolume(crop, path, options.crop->start, write);
    require(result.binding.labels.at(6) == 1000001, "New label collided with an outside ID");
    recordWrite(*context, result);
    auto updated = readImage<Label>(path, {}).image;
    require(updated->GetSpacing() == full->GetSpacing() && updated->GetOrigin() == full->GetOrigin() &&
            updated->GetDirection() == full->GetDirection(), "Insertion changed target geometry");
    for (int z = 0; z < 7; ++z) for (int y = 0; y < 9; ++y) for (int x = 0; x < 13; ++x) {
        const bool inside = x >= 3 && x < 8 && y >= 2 && y < 6 && z >= 1 && z < 4;
        const Label expected = inside ? result.binding.labels.at(crop->GetPixel({{x - 3, y - 2, z - 1}})) : full->GetPixel({{x, y, z}});
        if (updated->GetPixel({{x, y, z}}) != expected) {
            std::cerr << suffix.toStdString() << " compressed=" << compressed << " voxel " << x << ',' << y << ',' << z
                      << " expected " << expected << " actual " << updated->GetPixel({{x, y, z}}) << '\n';
            throw std::runtime_error("Roundtrip changed a voxel unexpectedly");
        }
    }
    require(crop->GetPixel({{1, 1, 1}}) == 6, "Export changed the working crop IDs");
    const auto second = writeRegionToVolume(crop, path, options.crop->start, write);
    require(second.binding.labels == result.binding.labels, "Repeated export changed label IDs");
    recordWrite(*context, second);
    crop->SetPixel({{2, 1, 1}}, 7);
    const auto third = writeRegionToVolume(crop, path, options.crop->start, write);
    require(third.binding.labels.at(6) == 1000001 && third.binding.labels.at(7) == 1000002,
            "New objects between exports lost stable IDs");
    recordWrite(*context, third);
    const auto encoded = serializeContext(*context);
    const auto restored = deserializeContext(encoded);
    require(restored->originalLabels == context->originalLabels && restored->targets[0].labels == context->targets[0].labels,
            "Recovery lost ID provenance");
    const auto unchanged = fileFingerprint(path);
    rejects([&] { writeRegionToVolume(crop, path, {{12, 8, 6}}, write); }, "Out-of-bounds insertion accepted");
    require(fileFingerprint(path) == unchanged, "Rejected insertion modified the target");
    auto external = readImage<Label>(path, {}).image;
    external->SetPixel({{0, 0, 0}}, 99);
    save<Label>(external, path, compressed);
    const auto externallyChanged = fileFingerprint(path);
    rejects([&] { writeRegionToVolume(crop, path, options.crop->start, write); }, "External modification was overwritten");
    require(fileFingerprint(path) == externallyChanged, "Conflict changed the target");
}

void independentTarget(const QString &directory) {
    auto crop = makeImage({{2, 2, 2}});
    crop->FillBuffer(3);
    crop->SetPixel({{0, 0, 0}}, 0);
    auto target = makeImage({{8, 9, 10}});
    target->FillBuffer(3);
    target->SetPixel({{0, 0, 0}}, 90);
    const auto path = directory + "/target.nrrd";
    save<Label>(target, path);
    const auto write = writeRegionToVolume(crop, path, {{4, 5, 6}}, {});
    require(write.binding.labels.at(3) == 91, "Independent target reused an object ID");
    auto output = readImage<Label>(path, {}).image;
    require(output->GetPixel({{4, 5, 6}}) == 0 && output->GetPixel({{5, 5, 6}}) == 91 && output->GetPixel({{3, 5, 6}}) == 3,
            "Independent placement is incorrect");
    using Small = itk::Image<unsigned char, 3>;
    auto small = Small::New();
    small->SetRegions(Small::SizeType{{4, 4, 4}}); small->Allocate(); small->FillBuffer(255);
    const auto smallPath = directory + "/small.nrrd";
    save<unsigned char>(small, smallPath);
    const auto before = fileFingerprint(smallPath);
    rejects([&] { writeRegionToVolume(crop, smallPath, {{0, 0, 0}}, {}); }, "Label overflow accepted");
    require(fileFingerprint(smallPath) == before, "Label overflow changed target");
    VolumeContext context;
    context.nextLabel = 100;
    require(reserveLabels(context, 2) == 100 && context.nextLabel == 102, "Allocator recycled IDs");
    context.nextLabel = std::uint64_t(std::numeric_limits<Label>::max()) + 1;
    rejects([&] { reserveLabels(context, 1); }, "Allocator wrapped");

    const auto restoredPath = directory + "/deleted-original.nrrd";
    target->FillBuffer(5); save<Label>(target, restoredPath);
    auto original = std::make_shared<VolumeContext>();
    original->background = 0; original->originalLabels = {5, 6};
    crop->FillBuffer(7);
    RegionWriteOptions same;
    same.context = original; same.labelPolicy = LabelPolicy::SameSegmentation;
    const auto fresh = writeRegionToVolume(crop, restoredPath, {{0, 0, 0}}, same);
    require(fresh.binding.labels.at(7) == 7, "A deleted original object ID was reassigned");
    recordWrite(*original, fresh);
    crop->SetPixel({{0, 0, 0}}, 6);
    const auto restored = writeRegionToVolume(crop, restoredPath, {{0, 0, 0}}, same);
    require(restored.binding.labels.at(6) == 6 && restored.binding.labels.at(7) == 7,
            "Restoring an original object collided with a new object");
    recordWrite(*original, restored);
    same.targetBackground = 6;
    const auto consistent = fileFingerprint(restoredPath);
    rejects([&] { writeRegionToVolume(crop, restoredPath, {{0, 0, 0}}, same); }, "Object/background collision accepted");
    require(fileFingerprint(restoredPath) == consistent, "Background collision damaged the target");
}

void largeBlockRead(const QString &directory) {
    const auto path = directory + "/blocks.nrrd";
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create test volume");
    file.write("NRRD0005\ntype: uint\ndimension: 3\nsizes: 513 257 33\nencoding: raw\nendian: little\n\n");
    const std::uint64_t count = 513ULL * 257 * 33;
    std::vector<std::uint32_t> block(8192);
    for (std::uint64_t first = 0; first < count;) {
        const auto length = std::min<std::uint64_t>(block.size(), count - first);
        for (std::size_t index = 0; index < length; ++index) block[index] = static_cast<std::uint32_t>((first + index) % 101);
        file.write(reinterpret_cast<const char *>(block.data()), length * sizeof(std::uint32_t));
        first += length;
    }
    file.close();
    ImageLoadOptions options;
    options.crop = CropRegion{{{509, 253, 30}}, {{4, 4, 3}}};
    auto crop = readImage<Label>(path, options).image;
    require(crop->GetBufferedRegion().GetNumberOfPixels() == 48, "Full image allocated for crop");
    for (int z = 0; z < 3; ++z) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        require(crop->GetPixel({{x, y, z}}) == ((z + 30) * 513 * 257 + (y + 253) * 513 + x + 509) % 101,
                "Cross-block crop extraction failed");
}

void bigEndianRoundtrip(const QString &directory) {
    for (const bool wide : {false, true}) {
        const auto path = directory + (wide ? "/big-endian-64.nrrd" : "/big-endian-16.nrrd");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Cannot create big-endian volume");
        file.write(wide ? "NRRD0005\ntype: unsigned long long\n" : "NRRD0005\ntype: unsigned short\n");
        file.write("dimension: 3\nsizes: 3 2 1\nencoding: raw\nendian: big\n\n");
        for (const auto label : {0, 7, 0, 3, 0, 9}) {
            if (wide) {
                const auto value = qToBigEndian<std::uint64_t>(label);
                file.write(reinterpret_cast<const char *>(&value), sizeof(value));
            } else {
                const auto value = qToBigEndian<std::uint16_t>(label);
                file.write(reinterpret_cast<const char *>(&value), sizeof(value));
            }
        }
        file.close();
        ImageLoadOptions options;
        options.labelImage = true;
        options.crop = CropRegion{{{1, 0, 0}}, {{1, 2, 1}}};
        checkStoredLabels(path, options);
        checkStoredLabels(path, ImageLoadOptions{std::nullopt, true, true});
        auto loaded = readImage<Label>(path, options);
        require(loaded.image->GetPixel({{0, 0, 0}}) == 7 && loaded.image->GetPixel({{0, 1, 0}}) == 0,
                "Big-endian crop was decoded incorrectly");
        const auto result = writeRegionToVolume(loaded.image, path, options.crop->start, {});
        auto output = readImage<Label>(path, {}).image;
        require(result.binding.labels.at(7) == 10 && output->GetPixel({{1, 0, 0}}) == 10 &&
                output->GetPixel({{1, 1, 0}}) == 0 && output->GetPixel({{0, 1, 0}}) == 3 &&
                output->GetPixel({{2, 1, 0}}) == 9, "Big-endian insertion changed IDs or outside voxels");
    }
}

template<typename Pixel>
void gzipIntensity(const QString &directory, bool nonFinite = false) {
    using Intensity = itk::Image<Pixel, 3>;
    const auto geometry = makeImage({{13, 9, 7}});
    auto image = Intensity::New();
    image->CopyInformation(geometry);
    image->SetRegions(geometry->GetLargestPossibleRegion());
    image->Allocate(false);
    const auto count = image->GetBufferedRegion().GetNumberOfPixels();
    for (std::size_t i = 0; i < count; ++i)
        image->GetBufferPointer()[i] = static_cast<Pixel>(int(i % 101) - 48);

    image->GetBufferPointer()[0] = std::numeric_limits<Pixel>::lowest();
    image->GetBufferPointer()[1] = std::numeric_limits<Pixel>::max();
    if constexpr (std::is_floating_point_v<Pixel>) {
        image->GetBufferPointer()[2] = Pixel(-0.125);
        if (nonFinite) {
            image->GetBufferPointer()[3] = std::numeric_limits<Pixel>::quiet_NaN();
            image->GetBufferPointer()[4] = std::numeric_limits<Pixel>::infinity();
        }
    }

    for (const QString &suffix : {QStringLiteral(".nrrd"), QStringLiteral(".nii.gz")}) {
        const auto path = directory + "/intensity" + suffix;
        save<Pixel>(image, path, true);
        const auto direct = tryReadGzipIntensity<Pixel>(path);
        require(direct.image.IsNotNull() == !(nonFinite && suffix == ".nii.gz"), "Unexpected intensity reader");
        if constexpr (std::is_same_v<Pixel, unsigned int>) {
            require(tryReadGzipIntensity<float>(path).image.IsNull(), "Integer-to-float conversion bypassed ITK");
            require(tryReadGzipIntensity<int>(path).image.IsNull(), "Unsigned-to-signed conversion bypassed ITK");
        }
        const auto expected = readWithITK<Intensity>(path, {});
        const auto actual = readImage<Pixel>(path, {});
        require(actual.image->GetRequestedRegion() == expected.image->GetRequestedRegion() &&
                actual.image->GetBufferedRegion() == expected.image->GetBufferedRegion() &&
                actual.image->GetSpacing() == expected.image->GetSpacing() &&
                actual.image->GetOrigin() == expected.image->GetOrigin() &&
                actual.image->GetDirection() == expected.image->GetDirection(), "Direct intensity loading changed geometry");
        require(std::memcmp(expected.image->GetBufferPointer(), actual.image->GetBufferPointer(), count * sizeof(Pixel)) == 0, "Direct intensity loading changed voxels");
        require(actual.context->sourceFingerprint == expected.context->sourceFingerprint, "Direct intensity fingerprint differs");
        require(actual.image->GetMetaDataDictionary().GetKeys() == expected.image->GetMetaDataDictionary().GetKeys(),
                "Direct intensity loading lost metadata");
    }

    if constexpr (!std::is_same_v<Pixel, unsigned short>) return;
    const auto path = directory + "/intensity.nii.gz";
    QFile file(path);
    const auto emptyGzipMember = QByteArray::fromHex("1f8b080000000000000303000000000000000000");
    require(file.open(QIODevice::Append) && file.write(emptyGzipMember) == emptyGzipMember.size(), "Cannot append gzip member");
    file.close();
    require(tryReadGzipIntensity<Pixel>(path).image.IsNull(), "Multi-member gzip should fall back to ITK");
    const auto fallback = readImage<Pixel>(path, {});
    require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + count,
                       fallback.image->GetBufferPointer()), "ITK fallback changed voxels");
}

void gzipValidation(const QString &directory) {
    auto image = makeImage({{131, 137, 35}});
    for (const auto &suffix : {".nrrd", ".nii.gz"}) {
        const auto path = directory + "/checksum" + suffix;
        save<Label>(image, path, true);
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "Cannot read gzip fixture");
        const auto original = file.readAll();
        file.close();
        for (int footerOffset : {8, 4, 0}) {
            auto damaged = original;
            if (footerOffset) {
                const auto offset = damaged.size() - footerOffset;
                damaged[offset] = damaged.at(offset) ^ 1;
            } else damaged.chop(4);
            require(file.open(QIODevice::WriteOnly), "Cannot write gzip fixture");
            require(file.write(damaged) == damaged.size(), "Incomplete gzip fixture");
            file.close();
            rejects([&] { readLabelStorage(path, {}); }, "Streaming accepted damaged gzip");
        }
    }
}

void rejectedWrites(const QString &directory) {
    auto crop = makeImage({{2, 2, 2}});
    crop->FillBuffer(1);
    const auto path = directory + "/truncated.nrrd";
    save<Label>(makeImage({{8, 8, 8}}), path, true);
    QFile file(path);
    require(file.open(QIODevice::ReadWrite), "Cannot truncate test file");
    file.resize(file.size() - 4); file.close();
    const auto before = fileFingerprint(path);
    rejects([&] { writeRegionToVolume(crop, path, {{0, 0, 0}}, {}); }, "Corrupt gzip was accepted");
    rejects([&] { readLabelStorage(path, {}); }, "Streaming accepted corrupt gzip");
    require(fileFingerprint(path) == before, "Failed gzip read modified the target");

    const auto target = directory + "/locked.nrrd";
    save<Label>(makeImage({{8, 8, 8}}), target);
    QFile lock(target + ".segmentpuzzler.lock");
    require(lock.open(QIODevice::WriteOnly), "Cannot block target lock");
    lock.write("not a valid lock owner\n"); lock.close();
    const auto locked = fileFingerprint(target);
    rejects([&] { writeRegionToVolume(crop, target, {{0, 0, 0}}, {}); }, "Concurrent target writer was accepted");
    require(fileFingerprint(target) == locked, "Locked target was modified");

#if defined(__unix__) || defined(__APPLE__)
    const auto limited = directory + "/write-failure.nrrd";
    save<Label>(makeImage({{32, 32, 32}}), limited);
    const auto intact = fileFingerprint(limited);
    struct rlimit originalLimit{};
    require(getrlimit(RLIMIT_FSIZE, &originalLimit) == 0, "Cannot inspect file-size limit");
    auto limit = originalLimit; limit.rlim_cur = 4096;
    const auto originalHandler = std::signal(SIGXFSZ, SIG_IGN);
    require(setrlimit(RLIMIT_FSIZE, &limit) == 0, "Cannot simulate a short write");
    bool failed = false;
    try { writeRegionToVolume(crop, limited, {{0, 0, 0}}, {}); }
    catch (const std::exception &) { failed = true; }
    setrlimit(RLIMIT_FSIZE, &originalLimit);
    std::signal(SIGXFSZ, originalHandler);
    require(failed && fileFingerprint(limited) == intact, "A failed temporary write damaged the target");
#endif
}

void nonzeroBackground(const QString &directory) {
    const auto path = directory + "/nonzero-background.nrrd";
    auto full = makeImage({{4, 4, 4}}); full->FillBuffer(65535);
    full->SetPixel({{1, 1, 1}}, 0);
    full->SetPixel({{2, 1, 1}}, 4);
    save<Label>(full, path);
    ImageLoadOptions options;
    options.labelImage = true; options.backgroundIsHighest = true;
    options.crop = CropRegion{{{1, 1, 1}}, {{2, 1, 1}}};
    auto loaded = readImage<Label>(path, options);
    auto crop = loaded.image;
    auto context = loaded.context;
    require(context->background == 65535, "Highest source background was lost in a background-free crop");
    RegionWriteOptions write;
    write.context = context; write.targetBackground = 65535; write.labelPolicy = LabelPolicy::SameSegmentation;
    const auto result = writeRegionToVolume(crop, path, options.crop->start, write);
    require(result.binding.labels.at(0) == 0 && result.binding.labels.at(4) == 4,
            "Object zero was mistaken for the known nonzero background");
}

void labelInitialization() {
    const auto maximum = std::numeric_limits<Label>::max();
    std::vector<Label> runs(10003, 7);
    std::fill(runs.begin() + 123, runs.begin() + 7654, 0);
    runs.back() = maximum;
    std::vector<Label> alternating(10003);
    for (std::size_t i = 0; i < alternating.size(); ++i) alternating[i] = i % 5;
    const std::vector<std::vector<Label>> cases{{}, {0}, {7}, {maximum},
        {0, 7, 1000000, maximum, 7, 0}, runs, alternating};
    for (const auto &pixels : cases) {
        for (const auto background : {std::optional<Label>{}, std::optional<Label>{0},
                                      std::optional<Label>{7}, std::optional<Label>{maximum}}) {
            for (const bool remember : {false, true}) {
                for (const std::uint64_t next : {std::uint64_t{1}, std::uint64_t{1000001}, std::uint64_t(maximum) + 1}) {
                    VolumeContext expected;
                    expected.background = background;
                    expected.originalLabels = {42};
                    expected.nextLabel = next;
                    auto actual = expected;
                    if (!expected.background) expected.background = 0;
                    for (const auto label : pixels) {
                        if (remember) expected.originalLabels.insert(label);
                        useLabel(expected, label);
                    }
                    initializeLabels(actual, pixels.data(), pixels.size(), remember);
                    require(actual.background == expected.background && actual.nextLabel == expected.nextLabel &&
                            actual.originalLabels == expected.originalLabels,
                            "Parallel label initialization differs from sequential semantics");
                }
            }
        }
    }
}

void checkStoredLabels(const QString &path, const ImageLoadOptions &options) {
    const auto dense = readImage<Label>(path, options);
    const auto stored = readLabelStorage(path, options);
    const auto decoded = stored.image->denseRegion(stored.image->GetLargestPossibleRegion());
    require(decoded->GetBufferedRegion() == dense.image->GetBufferedRegion() &&
            decoded->GetOrigin() == dense.image->GetOrigin() && decoded->GetSpacing() == dense.image->GetSpacing() &&
            decoded->GetDirection() == dense.image->GetDirection(), "Streaming load changed geometry");
    require(std::equal(dense.image->GetBufferPointer(), dense.image->GetBufferPointer() +
            dense.image->GetBufferedRegion().GetNumberOfPixels(), decoded->GetBufferPointer()), "Streaming load changed pixels");
    require(serializeContext(*dense.context) == serializeContext(*stored.context), "Streaming load changed provenance or labels");
    require(!imageContext(stored.image->GetMetaDataDictionary()), "Streaming storage retained duplicate context");
}

void streamingWrites(const QString &directory) {
    auto image = makeImage({{131, 137, 35}});
    const auto count = image->GetBufferedRegion().GetNumberOfPixels();
    for (std::size_t i = 0; i < count; ++i) image->GetBufferPointer()[i] = (i / 97) % 19;
    image->GetBufferPointer()[count - 1] = 4000000000U;
    const std::string description = "Backslash \\ and\nnewline, \xc3\xa4";
    itk::EncapsulateMetaData<std::string>(image->GetMetaDataDictionary(), "description", description);
    itk::EncapsulateMetaData<std::string>(image->GetMetaDataDictionary(), "sizes", "custom metadata");
    itk::EncapsulateMetaData<std::string>(image->GetMetaDataDictionary(), "NRRD_content", "Label volume");
    itk::EncapsulateMetaData<std::string>(image->GetMetaDataDictionary(), "NRRD_labels[0]", "X axis");
    itk::EncapsulateMetaData<double>(image->GetMetaDataDictionary(), "NRRD_thicknesses[1]", 2.5);
    VolumeContext context;
    context.background = 0; context.originalLabels = {0, 7}; context.nextLabel = 21;
    setImageContext(image->GetMetaDataDictionary(), context);
    auto region = image->GetLargestPossibleRegion();
    region.SetIndex({{-3, 5, 9}}); image->SetRegions(region);
    Image::PointType expectedOrigin;
    image->TransformIndexToPhysicalPoint(region.GetIndex(), expectedOrigin);
    for (bool rle : {false, true}) for (bool compressed : {false, true}) for (bool recovery : {false, true}) {
        dataType::SegmentsStorage::Pointer storage = rle ? segment_puzzler::storeLabels(*image)
            : segment_puzzler::DenseImageStorage<Label>::fromImage(image);
        const auto path = directory + "/streaming-write.nrrd";
        const auto before = storage->GetMTime();
        const auto hash = writeLabelVolume(*storage, path, recovery ? &context : nullptr, compressed);
        require(hash == fileFingerprint(path), "Written fingerprint differs");
        require(storage->GetMTime() == before, "Writing modified source storage");
        auto reader = itk::ImageFileReader<Image>::New();
        reader->SetFileName(path.toStdString()); reader->Update();
        const auto *decoded = reader->GetOutput();
        require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + count, decoded->GetBufferPointer()),
                "Streaming write changed pixels");
        require(decoded->GetSpacing() == image->GetSpacing() && decoded->GetDirection() == image->GetDirection() &&
                (decoded->GetOrigin() - expectedOrigin).GetNorm() < 1e-10, "Streaming write changed geometry");
        std::string actual;
        require(itk::ExposeMetaData<std::string>(decoded->GetMetaDataDictionary(), "description", actual) && actual == description,
                "Streaming write changed metadata");
        require(itk::ExposeMetaData<std::string>(decoded->GetMetaDataDictionary(), "sizes", actual) && actual == "custom metadata",
                "Streaming write changed a metadata key matching a field name");
        require(itk::ExposeMetaData<std::string>(decoded->GetMetaDataDictionary(), "NRRD_content", actual) && actual == "Label volume",
                "Streaming write changed NRRD content");
        require(itk::ExposeMetaData<std::string>(decoded->GetMetaDataDictionary(), "NRRD_labels[0]", actual) && actual == "X axis",
                "Streaming write changed axis labels");
        double thickness = 0;
        require(itk::ExposeMetaData<double>(decoded->GetMetaDataDictionary(), "NRRD_thicknesses[1]", thickness) && thickness == 2.5,
                "Streaming write changed axis thickness");
        const auto savedContext = imageContext(decoded->GetMetaDataDictionary());
        require(bool(savedContext) == recovery, "Export/recovery context differs");
        if (recovery) require(serializeContext(*savedContext) == serializeContext(context), "Recovery context changed");
    }
    auto large = makeImage({{2048, 2049, 2}});
    large->FillBuffer(7);
    std::mt19937 random(7);
    for (std::size_t i = 0; i < 512 * 1024; ++i) large->GetBufferPointer()[i] = random() % 256;
    large->SetPixel({{2047, 2048, 1}}, 19);
    auto storage = segment_puzzler::storeLabels(*large);
    const auto path = directory + "/streaming-write-rows.nrrd";
    writeLabelVolume(*storage, path);
    const auto loaded = readImage<Label>(path, {}).image;
    require(std::equal(large->GetBufferPointer(), large->GetBufferPointer() + large->GetBufferedRegion().GetNumberOfPixels(),
                       loaded->GetBufferPointer()), "Row batches changed pixels");
    const auto fingerprint = fileFingerprint(path);
    itk::EncapsulateMetaData<std::string>(storage->GetMetaDataDictionary(), "bad:=key", "value");
    rejects([&] { writeLabelVolume(*storage, path); }, "Invalid metadata was accepted");
    require(fileFingerprint(path) == fingerprint, "Rejected write replaced target");
}

void streamingLabels(const QString &directory) {
    auto image = makeImage({{131, 137, 35}});
    const auto count = image->GetBufferedRegion().GetNumberOfPixels();
    for (std::size_t i = 0; i < count; ++i) image->GetBufferPointer()[i] = 1 + (i / 97) % 19;
    image->GetBufferPointer()[0] = 0;
    image->GetBufferPointer()[count - 1] = 4000000000U;
    for (const auto &suffix : {".nrrd", ".nii", ".nii.gz", ".mha"}) {
        for (bool compressed : {false, true}) {
            const auto path = directory + "/streaming" + suffix;
            save<Label>(image, path, compressed);
            ImageLoadOptions options; options.labelImage = true; options.allowFullRead = true;
            checkStoredLabels(path, options);
            options.crop = CropRegion{{{3, 5, 1}}, {{127, 129, 33}}};
            checkStoredLabels(path, options);
            options.backgroundIsHighest = true;
            checkStoredLabels(path, options);
        }
    }
}

void fullLoadContextAndValidation(const QString &directory) {
    const auto path = directory + "/full-labels.nrrd";
    auto image = makeImage({{4, 2, 1}});
    image->SetPixel({{1, 0, 0}}, 9);
    save<Label>(image, path);
    ImageLoadOptions options; options.labelImage = true;
    auto loaded = readImage<Label>(path, options);
    require(loaded.context->background == 0 && loaded.context->nextLabel == 10 &&
            loaded.context->originalLabels == std::unordered_set<Label>({0, 9}), "Full loading lost label state");
    require(!imageContext(loaded.image->GetMetaDataDictionary()), "Live image retains a second context");
    auto highestOptions = options;
    highestOptions.backgroundIsHighest = true;
    checkStoredLabels(path, options);
    checkStoredLabels(path, highestOptions);
    auto highest = readImage<Label>(path, highestOptions);
    require(highest.context->background == 9 && highest.context->nextLabel == 1 &&
            highest.context->originalLabels == loaded.context->originalLabels,
            "Highest background incorrectly advanced the label allocator");
    require(reserveLabels(*loaded.context, 1) == 10, "Loaded allocator was not initialized");
    const auto recoveryPath = directory + "/context-recovery.nrrd";
    setImageContext(image->GetMetaDataDictionary(), *loaded.context);
    save<Label>(image, recoveryPath);
    auto recovered = readImage<Label>(recoveryPath, options);
    require(recovered.context->nextLabel == 11 && recovered.context->originalLabels == loaded.context->originalLabels &&
            !imageContext(recovered.image->GetMetaDataDictionary()), "Recovery did not extract the authoritative context");
    image->SetPixel({{2, 0, 0}}, 20);
    save<Label>(image, recoveryPath);
    checkStoredLabels(recoveryPath, highestOptions);
    recovered = readImage<Label>(recoveryPath, highestOptions);
    require(recovered.context->background == 0 && recovered.context->nextLabel == 21 &&
            recovered.context->originalLabels == loaded.context->originalLabels,
            "Recovery changed original labels or failed to account for newly used IDs");

    using Signed = itk::Image<long long, 3>;
    auto invalid = Signed::New(); invalid->SetRegions(Signed::SizeType{{2, 1, 1}}); invalid->Allocate();
    const auto invalidPath = directory + "/invalid-label.nrrd";
    for (const auto value : {-1LL, static_cast<long long>(std::numeric_limits<Label>::max()) + 1}) {
        invalid->FillBuffer(value); save<long long>(invalid, invalidPath);
        rejects([&] { readImage<Label>(invalidPath, options); }, "Full loading silently narrowed invalid labels");
        rejects([&] { readLabelStorage(invalidPath, options); }, "Streaming silently narrowed invalid labels");
    }
    using Float = itk::Image<float, 3>;
    auto fractional = Float::New(); fractional->SetRegions(Float::SizeType{{2, 1, 1}}); fractional->Allocate(); fractional->FillBuffer(1.5f);
    save<float>(fractional, invalidPath);
    rejects([&] { readImage<Label>(invalidPath, options); }, "Full loading truncated fractional labels");
    rejects([&] { readLabelStorage(invalidPath, options); }, "Streaming truncated fractional labels");

    VolumeContext labels; labels.background = 11; labels.nextLabel = 10;
    require(reserveLabels(labels, 2) == 12 && labels.nextLabel == 14, "Allocator did not skip the background");
    useLabel(labels, 20); useLabel(labels, 3);
    require(reserveLabels(labels, 1) == 21, "Explicit IDs moved the allocator backwards");
    labels.nextLabel = std::numeric_limits<Label>::max();
    require(reserveLabels(labels, 1) == std::numeric_limits<Label>::max(), "Last representable ID was lost");
    rejects([&] { reserveLabels(labels, 1); }, "Exhausted allocator wrapped around");
}

void memoryProbe(const QString &directory, int planes, bool fullVolume = false) {
    require(planes > 0 && planes <= 2048, "Invalid probe size");
    const auto path = directory + "/memory-probe.nrrd";
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create memory probe");
    file.write(QString("NRRD0005\ntype: uint\ndimension: 3\nsizes: 512 512 %1\nencoding: raw\nendian: little\n\n").arg(planes).toUtf8());
    require(file.resize(file.pos() + qint64(planes) * 512 * 512 * 4), "Cannot size memory probe");
    file.close();
    ImageLoadOptions options;
    if (!fullVolume) options.crop = CropRegion{{{508, 508, std::uint64_t(planes - 1)}}, {{4, 4, 1}}};
    options.labelImage = true;
    auto crop = readImage<Label>(path, options).image;
    require(crop->GetBufferedRegion().GetNumberOfPixels() == (fullVolume ? std::size_t(planes) * 512 * 512 : 16), "Memory probe loaded wrong region");
    std::cout << (fullVolume ? "Read full " : "Read 16-voxel crop from ") << planes << " MiB volume\n";
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        std::cout << "Peak RSS: " << usage.ru_maxrss / (1024.0 * 1024.0) << " MiB\n";
#else
        std::cout << "Peak RSS: " << usage.ru_maxrss / 1024.0 << " MiB\n";
#endif
    }
#endif
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    try {
        require(directory.isValid(), "No temporary directory");
        if (argc == 3 && QString::fromLocal8Bit(argv[1]).startsWith("--memory-probe")) {
            memoryProbe(directory.path(), QString::fromLocal8Bit(argv[2]).toInt(), QString::fromLocal8Bit(argv[1]) == "--memory-probe-full");
            return 0;
        }
        labelInitialization();
        roundtrip(directory.path(), ".nrrd", false);
        roundtrip(directory.path(), ".nrrd", true);
        roundtrip(directory.path(), ".nii", false);
        roundtrip(directory.path(), ".nii.gz", true);
        streamingLabels(directory.path());
        streamingWrites(directory.path());
        fullLoadContextAndValidation(directory.path());
        independentTarget(directory.path());
        largeBlockRead(directory.path());
        bigEndianRoundtrip(directory.path());
        gzipIntensity<signed char>(directory.path());
        gzipIntensity<unsigned char>(directory.path());
        gzipIntensity<short>(directory.path());
        gzipIntensity<unsigned short>(directory.path());
        gzipIntensity<int>(directory.path());
        gzipIntensity<unsigned int>(directory.path());
        gzipIntensity<long long>(directory.path());
        gzipIntensity<unsigned long long>(directory.path());
        gzipIntensity<float>(directory.path());
        gzipIntensity<double>(directory.path());
        gzipIntensity<float>(directory.path(), true);
        gzipIntensity<double>(directory.path(), true);
        gzipValidation(directory.path());
        rejectedWrites(directory.path());
        nonzeroBackground(directory.path());
        std::cout << "Volume crop, label mapping, atomic insertion and recovery tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
