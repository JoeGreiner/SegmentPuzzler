#include "src/image_storage/LabelImageStorage.h"
#include "src/qtUtils/SegmentTableDialog.h"
#include <itkExtractImageFilter.h>
#include <itkLabelImageToShapeLabelMapFilter.h>
#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <random>

using Label = dataType::SegmentIdType;
using Image = dataType::SegmentsImageType;
using Storage = dataType::SegmentsStorage;
using Flags = SegmentTableDialog::FeatureFlags;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const char *name) {
    if (std::isnan(actual) && std::isnan(expected)) return;
    if (actual == expected) return;
    if (std::abs(actual - expected) > 1e-8 * std::max(1.0, std::abs(expected)) ||
        !std::isfinite(actual) || !std::isfinite(expected))
        throw std::runtime_error(std::string(name) + ": " + std::to_string(actual) + " != " + std::to_string(expected));
}

class PlaneReads final : public Storage {
public:
    using Pointer = itk::SmartPointer<PlaneReads>;
    itkNewMacro(PlaneReads);
    Storage::Pointer source;
    mutable std::size_t readCount = 0;
    const itk::ImageBase<3> *imageInformation() const override { return source->imageInformation(); }
    std::size_t storedBytes() const override { return source->storedBytes(); }
    Label GetPixel(const Index &) const override { throw std::runtime_error("Unexpected per-voxel storage read"); }
    void readRegion(const Region &region, Label *pixels, std::size_t count) const override {
        require(region.GetSize()[2] == 1, "Segment table requested multiple dense planes");
        ++readCount;
        source->readRegion(region, pixels, count);
    }
    void forEachRun(const Region &region, const RunVisitor &visit) const override { source->forEachRun(region, visit); }
    void FillBuffer(Label) override { throw std::runtime_error("Unexpected write"); }
    void fillRegion(const Region &, Label) override { throw std::runtime_error("Unexpected write"); }
    void setVoxelValues(const std::vector<Index> &, Label) override { throw std::runtime_error("Unexpected write"); }
    void fillVoxelRanges(const std::vector<VoxelRange> &) override { throw std::runtime_error("Unexpected write"); }
    void writeRegion(const Region &, const Label *, std::size_t) override { throw std::runtime_error("Unexpected write"); }
};

Flags allFeatures(int borderDistance, bool calibrated) {
    Flags flags;
    flags.physicalSize = flags.backgroundExposure = flags.foregroundExposure = true;
    flags.nearBoundary = flags.pixelsOnBorder = flags.perimeterOnBorder = true;
    flags.bbox = flags.equivSphPerimeter = flags.equivEllipsoid = true;
    flags.principalMoments = flags.perimeter = flags.surfaceToVolumeRatio = flags.orientedBBox = true;
    flags.roundness = true;
    flags.borderDistancePx = borderDistance;
    flags.overridePixelSize = calibrated;
    flags.pixelSize = 2.3;
    return flags;
}

template<typename ImageType>
void checkShapeRows(typename ImageType::Pointer image, const SegmentTableDialog::ComputeResult &result,
                    Label background) {
    using Filter = itk::LabelImageToShapeLabelMapFilter<ImageType>;
    auto filter = Filter::New();
    filter->SetInput(image); filter->SetBackgroundValue(background);
    filter->SetComputePerimeter(true); filter->SetComputeOrientedBoundingBox(true);
    filter->Update();
    const auto *map = filter->GetOutput();
    require(map->GetNumberOfLabelObjects() == result.rows.size(), "Label count changed");
    for (const auto &row : result.rows) {
        const auto *object = map->GetLabelObject(row.label);
        itk::ContinuousIndex<double, ImageType::ImageDimension> index;
        (void)image->TransformPhysicalPointToContinuousIndex(object->GetCentroid(), index);
        require(row.centroidX == std::round(index[0]) && row.centroidY == std::round(index[1]), "Centroid changed");
        if constexpr (ImageType::ImageDimension == 3)
            require(row.centroidZ == std::round(index[2]), "Z centroid changed");
#define CHECK(field, value) near(row.field, value, #field)
        CHECK(volume, object->GetNumberOfPixels());
        CHECK(physicalSize, object->GetPhysicalSize());
        CHECK(perimeterOnBorder, object->GetPerimeterOnBorder());
        CHECK(bboxW, object->GetBoundingBox().GetSize()[0]);
        CHECK(bboxH, object->GetBoundingBox().GetSize()[1]);
        CHECK(elongation, object->GetElongation());
        CHECK(flatness, object->GetFlatness());
        CHECK(roundness, object->GetRoundness());
        CHECK(equivSphRadius, object->GetEquivalentSphericalRadius());
        CHECK(equivSphPerimeter, object->GetEquivalentSphericalPerimeter());
        CHECK(equivEllipD0, object->GetEquivalentEllipsoidDiameter()[0]);
        CHECK(equivEllipD1, object->GetEquivalentEllipsoidDiameter()[1]);
        CHECK(principalMom0, object->GetPrincipalMoments()[0]);
        CHECK(principalMom1, object->GetPrincipalMoments()[1]);
        CHECK(perimeter, object->GetPerimeter());
        CHECK(surfaceToVolumeRatio, object->GetPerimeter() / object->GetPhysicalSize());
        const auto box = object->GetOrientedBoundingBoxSize();
        CHECK(obboxW, box[0]); CHECK(obboxH, box[1]);
        if constexpr (ImageType::ImageDimension == 3) {
            CHECK(bboxD, object->GetBoundingBox().GetSize()[2]);
            CHECK(equivEllipD2, object->GetEquivalentEllipsoidDiameter()[2]);
            CHECK(principalMom2, object->GetPrincipalMoments()[2]);
            CHECK(obboxD, box[2]); CHECK(obboxVolume, box[0] * box[1] * box[2]);
        } else {
            CHECK(bboxD, -1); CHECK(equivEllipD2, -1); CHECK(principalMom2, -1);
            CHECK(obboxD, -1); CHECK(obboxVolume, box[0] * box[1]);
        }
#undef CHECK
    }
}

void checkContacts(const Image &image, const SegmentTableDialog::ComputeResult &result, Label background) {
    struct Counts { std::size_t surface = 0, background = 0, foreground = 0, border = 0; };
    std::unordered_map<Label, Counts> counts;
    const auto region = image.GetLargestPossibleRegion();
    const auto borderDistance = std::max(0, result.flags.borderDistancePx);
    const auto size = region.GetSize();
    for (std::size_t z = 0; z < size[2]; ++z)
        for (std::size_t y = 0; y < size[1]; ++y)
            for (std::size_t x = 0; x < size[0]; ++x) {
                auto index = region.GetIndex(); index[0] += x; index[1] += y; index[2] += z;
                const auto label = image.GetPixel(index);
                if (label == background) continue;
                bool touchesBackground = false, touchesForeground = false, border = false;
                for (unsigned axis = 0; axis < (result.is2D ? 2U : 3U); ++axis) {
                    const auto offset = index[axis] - region.GetIndex()[axis];
                    border |= offset <= borderDistance ||
                        static_cast<long long>(size[axis]) - 1 - offset <= borderDistance;
                    for (int step : {-1, 1}) {
                        auto neighbor = index; neighbor[axis] += step;
                        const auto value = region.IsInside(neighbor) ? image.GetPixel(neighbor) : background;
                        touchesBackground |= value == background;
                        touchesForeground |= value != background && value != label;
                    }
                }
                auto &entry = counts[label];
                entry.surface += touchesBackground || touchesForeground;
                entry.background += touchesBackground;
                entry.foreground += touchesForeground;
                entry.border += border;
            }
    for (const auto &row : result.rows) {
        const auto &count = counts.at(row.label);
        require(row.isIsolated == (count.foreground == 0), "Isolation changed");
        if (result.flags.pixelsOnBorder) near(row.pixelsOnBorder, count.border, "pixelsOnBorder");
        if (result.flags.nearBoundary) require(row.isNearBoundary == (count.border > 0), "Boundary classification changed");
        if (result.flags.backgroundExposure) near(row.backgroundExposure, double(count.background) / count.surface, "backgroundExposure");
        if (result.flags.foregroundExposure) near(row.foregroundExposure, double(count.foreground) / count.surface, "foregroundExposure");
    }
}

void checkImage(Image::Pointer image, Label background, const Flags &flags) {
    for (bool compressed : {false, true}) {
        auto storage = PlaneReads::New();
        storage->source = compressed ? segment_puzzler::storeLabels(*image)
            : Storage::Pointer(segment_puzzler::DenseImageStorage<Label>::fromImage(image));
        const auto modified = storage->source->GetMTime();
        const auto result = SegmentTableDialog::computeFeatures(storage, flags, background);
        require(storage->source->GetMTime() == modified, "Feature computation modified source storage");
        if (result.rows.empty()) require(storage->readCount == 0, "Empty table read dense planes");
        checkContacts(*image, result, background);
        if (result.is2D) {
            using Image2D = itk::Image<Label, 2>;
            auto extract = itk::ExtractImageFilter<Image, Image2D>::New();
            auto region = image->GetLargestPossibleRegion(); region.SetSize(2, 0);
            extract->SetInput(image); extract->SetExtractionRegion(region); extract->SetDirectionCollapseToSubmatrix();
            extract->Update();
            Image2D::Pointer plane = extract->GetOutput();
            plane->DisconnectPipeline();
            if (flags.overridePixelSize) {
                Image2D::SpacingType spacing; spacing.Fill(flags.pixelSize); plane->SetSpacing(spacing);
            }
            checkShapeRows<Image2D>(plane, result, background);
            for (const auto &row : result.rows)
                require(row.centroidZ == image->GetLargestPossibleRegion().GetIndex()[2], "Singleton Z changed");
        } else checkShapeRows<Image>(image, result, background);
        Flags quick;
        quick.elongation = quick.flatness = quick.equivSphRadius = false;
        storage->readCount = 0;
        const auto quickResult = SegmentTableDialog::computeFeatures(storage, quick, background);
        require(storage->readCount == 0, "Isolation requested dense slices");
        checkContacts(*image, quickResult, background);
        require(quickResult.rows.size() == result.rows.size(), "Quick mode lost labels");
        for (std::size_t i = 0; i < result.rows.size(); ++i) {
            near(quickResult.rows[i].volume, result.rows[i].volume, "Quick mode volume");
            require(quickResult.rows[i].centroidX == result.rows[i].centroidX &&
                    quickResult.rows[i].centroidY == result.rows[i].centroidY &&
                    quickResult.rows[i].centroidZ == result.rows[i].centroidZ, "Quick mode centroid changed");
        }
    }
}

void checkIntensity() {
    using IntensityImage = itk::Image<float, 3>;
    auto image = IntensityImage::New();
    const Image::RegionType region({-2, 7, 11}, {101, 2, 1});
    image->SetRegions(region);
    image->Allocate();
    image->FillBuffer(std::numeric_limits<float>::quiet_NaN());
    auto labels = Image::New();
    labels->SetRegions(region);
    labels->Allocate();
    labels->FillBuffer(17);
    for (int i = 0; i < 100; ++i) image->GetBufferPointer()[i] = i - 50.5f;
    labels->GetBufferPointer()[101] = 23;
    image->GetBufferPointer()[102] = std::numeric_limits<float>::infinity();
    labels->GetBufferPointer()[103] = 0;
    image->GetBufferPointer()[103] = 10000;
    Flags flags;
    flags.meanIntensity = flags.minIntensity = flags.maxIntensity = flags.intensityQuantiles = true;
    const auto storage = segment_puzzler::storeLabels(*labels);
    image->SetOrigin(IntensityImage::PointType{{1, 0, 0}});
    const auto table = SegmentTableDialog::computeFeatures(storage, flags, 0, image);
    require(table.rows.size() == 2, "Intensity label count");
    const std::array<double, 7> expected{-1, -50.5, 48.5, -50.5, -46.5, 43.5, 47.5};
    for (const auto &row : table.rows)
        for (std::size_t i = 0; i < expected.size(); ++i)
            near(row.intensity[i], row.label == 17 ? expected[i] : std::numeric_limits<double>::quiet_NaN(), "Intensity");
    auto largeValues = itk::Image<double, 3>::New();
    largeValues->SetRegions(region);
    largeValues->Allocate();
    largeValues->FillBuffer(std::numeric_limits<double>::max());
    const auto large = SegmentTableDialog::computeFeatures(storage, flags, 0, largeValues);
    for (const auto &row : large.rows)
        require(row.intensity[0] == std::numeric_limits<double>::max(), "Finite mean overflowed");
    image->SetRegions(Image::RegionType({-1, 7, 11}, region.GetSize()));
    try { SegmentTableDialog::computeFeatures(storage, flags, 0, image); }
    catch (const std::invalid_argument &) { return; }
    throw std::runtime_error("Mismatched intensity grid accepted");
}

void checkIntegerQuantiles() {
    using IntensityImage = itk::Image<signed char, 3>;
    const Image::SizeType size{16, 8, 2};
    auto image = IntensityImage::New();
    image->SetRegions(size);
    image->Allocate();
    auto labels = Image::New();
    labels->SetRegions(size);
    labels->Allocate();
    labels->FillBuffer(0);
    for (int i = 0; i < 256; ++i) image->GetBufferPointer()[i] = static_cast<signed char>(i - 128);
    Flags flags;
    flags.intensityQuantiles = true;
    const auto table = SegmentTableDialog::computeFeatures(segment_puzzler::storeLabels(*labels), flags, 255, image);
    const auto &values = table.rows.at(0).intensity;
    require(values[3] == -126 && values[4] == -116 && values[5] == 115 && values[6] == 125,
            "Signed histogram percentiles");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        checkIntensity();
        checkIntegerQuantiles();
        auto image = Image::New();
        image->SetRegions(Image::SizeType{3, 3, 4});
        image->Allocate();
        image->FillBuffer(255);
        image->SetPixel({0, 0, 1}, 0);
        image->SetPixel({0, 0, 2}, 7);
        image->SetPixel({1, 1, 2}, 9);
        const auto contacts = SegmentTableDialog::computeFeatures(segment_puzzler::storeLabels(*image), {}, 255);
        require(contacts.rows.size() == 3, "Expected three foreground labels");
        checkContacts(*image, contacts, 255);
        for (const auto &row : contacts.rows) require(row.roundness == -1, "Roundness enabled by default");

        std::mt19937 random(17);
        for (auto size : {Image::SizeType{13, 11, 1}, Image::SizeType{13, 11, 7}, Image::SizeType{1, 7, 4}})
            for (Label background : {Label(0), std::numeric_limits<Label>::max()}) {
                auto image = Image::New();
                image->SetRegions(Image::RegionType({-7, -3, -4}, size));
                image->SetSpacing(Image::SpacingType{{0.4, 1.3, 2.1}});
                image->SetOrigin(Image::PointType{{12.0, -4.0, 8.0}});
                Image::DirectionType direction; direction.SetIdentity();
                direction[0][0] = std::cos(0.6); direction[1][1] = std::cos(0.6);
                direction[0][1] = -std::sin(0.6); direction[1][0] = std::sin(0.6);
                image->SetDirection(direction); image->Allocate();
                const std::array<Label, 5> labels{background, 0, 3, 17, 4000000000U};
                for (std::size_t i = 0; i < size[0] * size[1] * size[2]; ++i)
                    image->GetBufferPointer()[i] = labels[random() % labels.size()];
                for (int border : {-1, 0, 2, 100, std::numeric_limits<int>::max()}) checkImage(image, background, allFeatures(border, border == 2));
                image->FillBuffer(background);
                checkImage(image, background, allFeatures(0, false));
                image->FillBuffer(17);
                checkImage(image, background, allFeatures(0, false));
            }
        std::cout << "Segment table: dense/RLE, ITK reference, contacts, geometry, borders and plane-only reads passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
