#include <itkImage.h>
#include <itkImageFileReader.h>
#include <itkImageIOBase.h>
#include <itkImageIOFactory.h>
#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIterator.h>
#include <itkMultiThreaderBase.h>
#include <itkSignedMaurerDistanceMapImageFilter.h>

#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapSeedExtractors.h"
#include "src/utils/DistanceMapFH3D.h"
#include "examples/benchmarks/BenchmarkPerfettoTracing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

using distance_map_benchmark::BinaryVoxelType;
using distance_map_benchmark::BinaryImageType;
using distance_map_benchmark::DistanceVoxelType;
using distance_map_benchmark::DistanceImageType;
using distance_map_benchmark::SeedExtractorKind;
using distance_map_benchmark::SeedImageType;
using distance_map_benchmark::SeedLabelType;

constexpr double kErrorTolerance = 1.0e-3;

double wallTimeSeconds() {
#ifdef USE_OMP
    return omp_get_wtime();
#else
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
#endif
}

struct MemorySnapshot {
    double currentMB = 0.0;
    double peakMB = 0.0;
};

MemorySnapshot readProcessMemory() {
    MemorySnapshot snapshot;
#if defined(__APPLE__)
    task_basic_info_data_t taskInfo{};
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&taskInfo),
                  &count) == KERN_SUCCESS) {
        snapshot.currentMB = static_cast<double>(taskInfo.resident_size) / (1024.0 * 1024.0);
    }

    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snapshot.peakMB = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
    } else {
        snapshot.peakMB = snapshot.currentMB;
    }
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    long totalPages = 0;
    long residentPages = 0;
    if (statm >> totalPages >> residentPages) {
        const double pageMB = static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
        snapshot.currentMB = residentPages * pageMB;
    }

    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            long peakKB = 0;
            status >> peakKB;
            snapshot.peakMB = static_cast<double>(peakKB) / 1024.0;
            break;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (snapshot.peakMB == 0.0) {
        snapshot.peakMB = snapshot.currentMB;
    }
#endif
    return snapshot;
}

enum class AlgorithmSelection {
    All,
    Maurer,
    FH
};

enum class BenchmarkMode {
    Edt,
    Seeds,
    Watershed
};

enum class WatershedAlgorithmSelection {
    All,
    Itk,
    FastMarker
};

enum class GeneratorKind {
    Shells,
    PackedSpheres,
    SparseSites
};

std::string toString(GeneratorKind kind) {
    switch (kind) {
        case GeneratorKind::Shells:
            return "shells";
        case GeneratorKind::PackedSpheres:
            return "packed-spheres";
        case GeneratorKind::SparseSites:
            return "sparse-sites";
    }
    return "unknown";
}

struct BenchmarkCase {
    std::string name;
    std::array<int, 3> dims{};
    std::array<double, 3> spacing{};
    std::vector<BinaryVoxelType> mask;
    BinaryImageType::Pointer itkMask;
    double siteFraction = 0.0;
};

struct AccuracyMetrics {
    double maxAbsError = 0.0;
    double meanAbsError = 0.0;
    double rmse = 0.0;
    std::size_t mismatchCount = 0;
};

struct SeedComparisonMetrics {
    std::size_t referenceSeedCount = 0;
    std::size_t candidateSeedCount = 0;
    std::size_t referenceSeedVoxels = 0;
    std::size_t candidateSeedVoxels = 0;
    std::size_t overlappingSeedVoxels = 0;
    double voxelIoU = 0.0;
    std::size_t matchedSeedCount = 0;
    std::size_t missedReferenceSeeds = 0;
    std::size_t extraCandidateSeeds = 0;
    double meanMatchedCentroidDistance = 0.0;
};

struct RunMetrics {
    double elapsedMs = 0.0;
    double distanceMapMs = 0.0;
    double seedExtractionMs = 0.0;
    double invertMs = 0.0;
    double watershedMs = 0.0;
    double quantizeMs = 0.0;
    double watershedInitMs = 0.0;
    double watershedFloodMs = 0.0;
    double xPassMs = 0.0;
    double yPassMs = 0.0;
    double zPassMs = 0.0;
    double megaVoxelsPerSecond = 0.0;
    double rssBeforeMB = 0.0;
    double rssAfterMB = 0.0;
    double peakRssMB = 0.0;
    double profileMinSeconds = 0.0;
    std::size_t loopCount = 1;
    std::size_t inputBytes = 0;
    std::size_t outputBytes = 0;
    std::size_t scratchBytes = 0;
};

struct BackendRunResult {
    std::vector<DistanceVoxelType> distances;
    std::vector<SeedLabelType> seeds;
    std::vector<SeedLabelType> watershedLabels;
    RunMetrics metrics;
};

struct WatershedComparisonMetrics {
    std::size_t referenceSegmentCount = 0;
    std::size_t candidateSegmentCount = 0;
    std::size_t referenceZeroLabelCount = 0;
    std::size_t candidateZeroLabelCount = 0;
    std::size_t differingVoxelCount = 0;
    double matchingVoxelFraction = 0.0;
};

struct CsvRow {
    std::string caseName;
    std::string sourceKind;
    std::string algorithm;
    std::string seedExtractor = "none";
    std::array<int, 3> dims{};
    std::array<double, 3> spacing{};
    double siteFraction = 0.0;
    int threads = 1;
    int repetition = 0;
    RunMetrics metrics;
    AccuracyMetrics accuracy;
    SeedComparisonMetrics seedMetrics;
    WatershedComparisonMetrics watershedMetrics;
};

struct BenchmarkOptions {
    AlgorithmSelection algorithmSelection = AlgorithmSelection::All;
    BenchmarkMode mode = BenchmarkMode::Edt;
    WatershedAlgorithmSelection watershedAlgorithmSelection = WatershedAlgorithmSelection::All;
    SeedExtractorKind seedExtractor = SeedExtractorKind::LocalMaxima;
    std::optional<GeneratorKind> generator;
    std::optional<std::array<int, 3>> requestedSize;
    std::optional<std::array<double, 3>> spacingOverride;
    std::vector<int> threadCounts;
    int repetitions = 3;
    int warmups = 1;
    double profileMinSeconds = 0.0;
    std::uint32_t seed = 1337;
    std::optional<std::string> inputPath;
    double threshold = 0.0;
    std::string csvPath = "distance_map_benchmark_results.csv";
    std::optional<std::string> perfettoOutputPath;
    std::size_t perfettoBufferKb = 32768;
};

const char *benchmarkModeName(BenchmarkMode mode) {
    switch (mode) {
        case BenchmarkMode::Edt:
            return "edt";
        case BenchmarkMode::Seeds:
            return "seeds";
        case BenchmarkMode::Watershed:
            return "watershed";
    }
    return "unknown";
}

const char *watershedAlgorithmName(WatershedAlgorithmSelection selection) {
    switch (selection) {
        case WatershedAlgorithmSelection::All:
            return "all";
        case WatershedAlgorithmSelection::Itk:
            return "itk";
        case WatershedAlgorithmSelection::FastMarker:
            return "fast-marker";
    }
    return "unknown";
}

std::size_t flatIndex(int x, int y, int z, const std::array<int, 3> &dims) {
    return static_cast<std::size_t>((z * dims[1] + y) * dims[0] + x);
}

std::array<int, 3> parseDims(const std::string &value) {
    std::array<int, 3> dims{};
    char separatorA = 0;
    char separatorB = 0;
    std::istringstream stream(value);
    stream >> dims[0] >> separatorA >> dims[1] >> separatorB >> dims[2];
    if (!stream || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 ||
        (separatorA != 'x' && separatorA != 'X') ||
        (separatorB != 'x' && separatorB != 'X')) {
        throw std::runtime_error("Invalid size, expected XxYxZ.");
    }
    return dims;
}

std::array<double, 3> parseSpacing(const std::string &value) {
    std::array<double, 3> spacing{};
    char separatorA = 0;
    char separatorB = 0;
    std::istringstream stream(value);
    stream >> spacing[0] >> separatorA >> spacing[1] >> separatorB >> spacing[2];
    if (!stream || spacing[0] <= 0.0 || spacing[1] <= 0.0 || spacing[2] <= 0.0 ||
        separatorA != ',' || separatorB != ',') {
        throw std::runtime_error("Invalid spacing, expected sx,sy,sz.");
    }
    return spacing;
}

std::vector<int> parseThreadList(const std::string &value) {
    if (value == "auto") {
        unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
#ifdef USE_OMP
        hardwareThreads = std::max(hardwareThreads, static_cast<unsigned int>(omp_get_max_threads()));
#endif
        std::vector<int> threadCounts;
        for (unsigned int threads = 1; threads < hardwareThreads; threads *= 2) {
            threadCounts.push_back(static_cast<int>(threads));
        }
        if (threadCounts.empty() || threadCounts.back() != static_cast<int>(hardwareThreads)) {
            threadCounts.push_back(static_cast<int>(hardwareThreads));
        }
        return threadCounts;
    }

    std::vector<int> threadCounts;
    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) {
            continue;
        }
        const int threadCount = std::stoi(token);
        if (threadCount <= 0) {
            throw std::runtime_error("Thread counts must be positive.");
        }
        threadCounts.push_back(threadCount);
    }
    if (threadCounts.empty()) {
        throw std::runtime_error("Thread list is empty.");
    }
    return threadCounts;
}

GeneratorKind parseGenerator(const std::string &value) {
    if (value == "shells") {
        return GeneratorKind::Shells;
    }
    if (value == "packed-spheres") {
        return GeneratorKind::PackedSpheres;
    }
    if (value == "sparse-sites") {
        return GeneratorKind::SparseSites;
    }
    throw std::runtime_error("Unknown generator: " + value);
}

AlgorithmSelection parseAlgorithmSelection(const std::string &value) {
    if (value == "all") {
        return AlgorithmSelection::All;
    }
    if (value == "maurer") {
        return AlgorithmSelection::Maurer;
    }
    if (value == "fh") {
        return AlgorithmSelection::FH;
    }
    throw std::runtime_error("Unknown algorithm selection: " + value);
}

WatershedAlgorithmSelection parseWatershedAlgorithmSelection(const std::string &value) {
    if (value == "all") {
        return WatershedAlgorithmSelection::All;
    }
    if (value == "itk") {
        return WatershedAlgorithmSelection::Itk;
    }
    if (value == "fast-marker") {
        return WatershedAlgorithmSelection::FastMarker;
    }
    throw std::runtime_error("Unknown watershed algorithm selection: " + value);
}

BenchmarkMode parseBenchmarkMode(const std::string &value) {
    if (value == "edt") {
        return BenchmarkMode::Edt;
    }
    if (value == "seeds") {
        return BenchmarkMode::Seeds;
    }
    if (value == "watershed") {
        return BenchmarkMode::Watershed;
    }
    throw std::runtime_error("Unknown benchmark mode: " + value);
}

void printUsage() {
    std::cout << "benchmark_distance_map\n"
              << "  --mode {edt,seeds,watershed}\n"
              << "  --algorithm {all,maurer,fh}\n"
              << "  --watershed-algorithm {all,itk,fast-marker}\n"
              << "  --seed-extractor {local-maxima,h-convex,all}\n"
              << "  --generator {shells,packed-spheres,sparse-sites}\n"
              << "  --size XxYxZ\n"
              << "  --spacing sx,sy,sz\n"
              << "  --threads list|auto\n"
              << "  --repetitions N\n"
              << "  --warmup N\n"
              << "  --profile-min-seconds S\n"
              << "  --seed N\n"
              << "  --input path\n"
              << "  --threshold value\n"
              << "  --perfetto-output path\n"
              << "  --perfetto-buffer-kb N\n"
              << "  --csv path\n";
}

BenchmarkOptions parseArguments(int argc, char **argv) {
    BenchmarkOptions options;
    options.threadCounts = parseThreadList("auto");

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto requireValue = [&](const char *flagName) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + flagName);
            }
            return argv[++i];
        };

        if (argument == "--help" || argument == "-h") {
            printUsage();
            std::exit(0);
        } else if (argument == "--mode") {
            options.mode = parseBenchmarkMode(requireValue("--mode"));
        } else if (argument == "--algorithm") {
            options.algorithmSelection = parseAlgorithmSelection(requireValue("--algorithm"));
        } else if (argument == "--watershed-algorithm") {
            options.watershedAlgorithmSelection = parseWatershedAlgorithmSelection(requireValue("--watershed-algorithm"));
        } else if (argument == "--seed-extractor") {
            options.seedExtractor = distance_map_benchmark::parseSeedExtractor(requireValue("--seed-extractor"));
        } else if (argument == "--generator") {
            options.generator = parseGenerator(requireValue("--generator"));
        } else if (argument == "--size") {
            options.requestedSize = parseDims(requireValue("--size"));
        } else if (argument == "--spacing") {
            options.spacingOverride = parseSpacing(requireValue("--spacing"));
        } else if (argument == "--threads") {
            options.threadCounts = parseThreadList(requireValue("--threads"));
        } else if (argument == "--repetitions") {
            options.repetitions = std::max(1, std::stoi(requireValue("--repetitions")));
        } else if (argument == "--warmup") {
            options.warmups = std::max(0, std::stoi(requireValue("--warmup")));
        } else if (argument == "--profile-min-seconds") {
            options.profileMinSeconds = std::max(0.0, std::stod(requireValue("--profile-min-seconds")));
        } else if (argument == "--seed") {
            options.seed = static_cast<std::uint32_t>(std::stoul(requireValue("--seed")));
        } else if (argument == "--input") {
            options.inputPath = requireValue("--input");
        } else if (argument == "--threshold") {
            options.threshold = std::stod(requireValue("--threshold"));
        } else if (argument == "--perfetto-output") {
            options.perfettoOutputPath = requireValue("--perfetto-output");
        } else if (argument == "--perfetto-buffer-kb") {
            options.perfettoBufferKb = static_cast<std::size_t>(std::stoull(requireValue("--perfetto-buffer-kb")));
        } else if (argument == "--csv") {
            options.csvPath = requireValue("--csv");
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }

    return options;
}

DistanceImageType::Pointer buildItkDistanceImage(const std::vector<DistanceVoxelType> &distances,
                                                 const std::array<int, 3> &dims,
                                                 const std::array<double, 3> &spacing) {
    auto image = DistanceImageType::New();
    DistanceImageType::SizeType size{};
    DistanceImageType::IndexType start{};
    size[0] = static_cast<DistanceImageType::SizeType::SizeValueType>(dims[0]);
    size[1] = static_cast<DistanceImageType::SizeType::SizeValueType>(dims[1]);
    size[2] = static_cast<DistanceImageType::SizeType::SizeValueType>(dims[2]);
    start.Fill(0);
    image->SetRegions(DistanceImageType::RegionType(start, size));
    DistanceImageType::SpacingType itkSpacing;
    itkSpacing[0] = spacing[0];
    itkSpacing[1] = spacing[1];
    itkSpacing[2] = spacing[2];
    image->SetSpacing(itkSpacing);
    image->Allocate();

    itk::ImageRegionIterator<DistanceImageType> iterator(image, image->GetLargestPossibleRegion());
    std::size_t index = 0;
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator, ++index) {
        iterator.Set(distances[index]);
    }
    return image;
}

BinaryImageType::Pointer buildItkMaskImage(const std::vector<BinaryVoxelType> &mask,
                                           const std::array<int, 3> &dims,
                                           const std::array<double, 3> &spacing) {
    auto image = BinaryImageType::New();
    BinaryImageType::SizeType size{};
    BinaryImageType::IndexType start{};
    size[0] = static_cast<BinaryImageType::SizeType::SizeValueType>(dims[0]);
    size[1] = static_cast<BinaryImageType::SizeType::SizeValueType>(dims[1]);
    size[2] = static_cast<BinaryImageType::SizeType::SizeValueType>(dims[2]);
    start.Fill(0);
    image->SetRegions(BinaryImageType::RegionType(start, size));
    BinaryImageType::SpacingType itkSpacing;
    itkSpacing[0] = spacing[0];
    itkSpacing[1] = spacing[1];
    itkSpacing[2] = spacing[2];
    image->SetSpacing(itkSpacing);
    image->Allocate();

    itk::ImageRegionIterator<BinaryImageType> iterator(image, image->GetLargestPossibleRegion());
    std::size_t index = 0;
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator, ++index) {
        iterator.Set(mask[index]);
    }
    return image;
}

template <typename TImage>
std::vector<BinaryVoxelType> binarizeImage(typename TImage::Pointer image, double threshold) {
    std::vector<BinaryVoxelType> mask;
    mask.reserve(image->GetLargestPossibleRegion().GetNumberOfPixels());
    itk::ImageRegionConstIterator<TImage> iterator(image, image->GetLargestPossibleRegion());
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        mask.push_back(static_cast<BinaryVoxelType>(iterator.Get() > threshold ? 1 : 0));
    }
    return mask;
}

template <typename PixelType>
BenchmarkCase loadCaseFromFileTyped(const std::string &path,
                                    double threshold,
                                    const std::optional<std::array<double, 3>> &spacingOverride) {
    using ImageType = itk::Image<PixelType, 3>;
    using ReaderType = itk::ImageFileReader<ImageType>;

    auto reader = ReaderType::New();
    reader->SetFileName(path);
    reader->Update();
    auto image = reader->GetOutput();

    BenchmarkCase benchmarkCase;
    const auto region = image->GetLargestPossibleRegion();
    const auto size = region.GetSize();
    benchmarkCase.name = "file:" + path;
    benchmarkCase.dims = {static_cast<int>(size[0]), static_cast<int>(size[1]), static_cast<int>(size[2])};
    const auto spacing = image->GetSpacing();
    benchmarkCase.spacing = spacingOverride.value_or(std::array<double, 3>{spacing[0], spacing[1], spacing[2]});
    benchmarkCase.mask = binarizeImage<ImageType>(image, threshold);

    const std::size_t siteCount = static_cast<std::size_t>(
        std::count(benchmarkCase.mask.begin(), benchmarkCase.mask.end(), static_cast<BinaryVoxelType>(1)));
    benchmarkCase.siteFraction =
        static_cast<double>(siteCount) / static_cast<double>(benchmarkCase.mask.size());
    benchmarkCase.itkMask = buildItkMaskImage(benchmarkCase.mask, benchmarkCase.dims, benchmarkCase.spacing);
    return benchmarkCase;
}

BenchmarkCase loadCaseFromFile(const std::string &path,
                               double threshold,
                               const std::optional<std::array<double, 3>> &spacingOverride) {
    auto imageIO = itk::ImageIOFactory::CreateImageIO(path.c_str(), itk::IOFileModeEnum::ReadMode);
    if (!imageIO) {
        throw std::runtime_error("Unable to inspect input file: " + path);
    }

    imageIO->SetFileName(path);
    imageIO->ReadImageInformation();
    if (imageIO->GetNumberOfDimensions() != 3) {
        throw std::runtime_error("Only 3D inputs are supported.");
    }

    switch (imageIO->GetComponentType()) {
        case itk::ImageIOBase::IOComponentEnum::UCHAR:
            return loadCaseFromFileTyped<unsigned char>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::CHAR:
            return loadCaseFromFileTyped<char>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::USHORT:
            return loadCaseFromFileTyped<unsigned short>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::SHORT:
            return loadCaseFromFileTyped<short>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::UINT:
            return loadCaseFromFileTyped<unsigned int>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::INT:
            return loadCaseFromFileTyped<int>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::ULONG:
            return loadCaseFromFileTyped<unsigned long>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::LONG:
            return loadCaseFromFileTyped<long>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::FLOAT:
            return loadCaseFromFileTyped<float>(path, threshold, spacingOverride);
        case itk::ImageIOBase::IOComponentEnum::DOUBLE:
            return loadCaseFromFileTyped<double>(path, threshold, spacingOverride);
        default:
            throw std::runtime_error("Unsupported input pixel type.");
    }
}

BenchmarkCase makeSparseSitesCase(const std::array<int, 3> &dims,
                                  const std::array<double, 3> &spacing,
                                  std::uint32_t seed) {
    BenchmarkCase benchmarkCase;
    benchmarkCase.name = "sparse-sites-" + std::to_string(dims[0]) + "x" +
                         std::to_string(dims[1]) + "x" + std::to_string(dims[2]);
    benchmarkCase.dims = dims;
    benchmarkCase.spacing = spacing;
    benchmarkCase.mask.assign(static_cast<std::size_t>(dims[0]) * dims[1] * dims[2], 0);

    std::mt19937 generator(seed);
    const std::size_t voxelCount = benchmarkCase.mask.size();
    const std::size_t siteCount = std::max<std::size_t>(1, voxelCount / 1500);
    std::uniform_int_distribution<int> distX(0, dims[0] - 1);
    std::uniform_int_distribution<int> distY(0, dims[1] - 1);
    std::uniform_int_distribution<int> distZ(0, dims[2] - 1);
    for (std::size_t i = 0; i < siteCount; ++i) {
        const int x = distX(generator);
        const int y = distY(generator);
        const int z = distZ(generator);
        benchmarkCase.mask[flatIndex(x, y, z, dims)] = 1;
    }

    benchmarkCase.siteFraction = static_cast<double>(std::count(
        benchmarkCase.mask.begin(), benchmarkCase.mask.end(), static_cast<BinaryVoxelType>(1))) /
        static_cast<double>(voxelCount);
    benchmarkCase.itkMask = buildItkMaskImage(benchmarkCase.mask, dims, spacing);
    return benchmarkCase;
}

BenchmarkCase makeShellsCase(const std::array<int, 3> &dims,
                             const std::array<double, 3> &spacing) {
    BenchmarkCase benchmarkCase;
    benchmarkCase.name = "shells-" + std::to_string(dims[0]) + "x" +
                         std::to_string(dims[1]) + "x" + std::to_string(dims[2]);
    benchmarkCase.dims = dims;
    benchmarkCase.spacing = spacing;
    benchmarkCase.mask.assign(static_cast<std::size_t>(dims[0]) * dims[1] * dims[2], 0);

    const double centerX = 0.5 * (dims[0] - 1);
    const double centerY = 0.5 * (dims[1] - 1);
    const double centerZ = 0.5 * (dims[2] - 1);
    const double baseRadius = 0.18 * static_cast<double>(std::min({dims[0], dims[1], dims[2]}));
    const std::array<double, 3> radii{baseRadius, baseRadius * 1.8, baseRadius * 2.6};

    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                const double dx = (x - centerX) * spacing[0];
                const double dy = (y - centerY) * spacing[1];
                const double dz = (z - centerZ) * spacing[2];
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                for (double radius : radii) {
                    if (std::abs(distance - radius) <= 0.75 * std::min({spacing[0], spacing[1], spacing[2]})) {
                        benchmarkCase.mask[flatIndex(x, y, z, dims)] = 1;
                        break;
                    }
                }
            }
        }
    }

    benchmarkCase.siteFraction = static_cast<double>(std::count(
        benchmarkCase.mask.begin(), benchmarkCase.mask.end(), static_cast<BinaryVoxelType>(1))) /
        static_cast<double>(benchmarkCase.mask.size());
    benchmarkCase.itkMask = buildItkMaskImage(benchmarkCase.mask, dims, spacing);
    return benchmarkCase;
}

BenchmarkCase makePackedSpheresCase(const std::array<int, 3> &dims,
                                    const std::array<double, 3> &spacing) {
    BenchmarkCase benchmarkCase;
    benchmarkCase.name = "packed-spheres-" + std::to_string(dims[0]) + "x" +
                         std::to_string(dims[1]) + "x" + std::to_string(dims[2]);
    benchmarkCase.dims = dims;
    benchmarkCase.spacing = spacing;
    benchmarkCase.mask.assign(static_cast<std::size_t>(dims[0]) * dims[1] * dims[2], 0);

    const double minDim = static_cast<double>(std::min({dims[0], dims[1], dims[2]}));
    const double radius = std::max(4.0, 0.09 * minDim);
    const double spacingBetweenCenters = 2.15 * radius;
    std::vector<std::array<double, 3>> centers;
    for (double cz = radius * 1.5; cz < dims[2] - radius; cz += spacingBetweenCenters) {
        for (double cy = radius * 1.5; cy < dims[1] - radius; cy += spacingBetweenCenters) {
            for (double cx = radius * 1.5; cx < dims[0] - radius; cx += spacingBetweenCenters) {
                centers.push_back({cx, cy, cz});
            }
        }
    }

    const double shellThickness = 0.75 * std::min({spacing[0], spacing[1], spacing[2]});
    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                for (const auto &center : centers) {
                    const double dx = (x - center[0]) * spacing[0];
                    const double dy = (y - center[1]) * spacing[1];
                    const double dz = (z - center[2]) * spacing[2];
                    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (std::abs(distance - radius) <= shellThickness) {
                        benchmarkCase.mask[flatIndex(x, y, z, dims)] = 1;
                        break;
                    }
                }
            }
        }
    }

    benchmarkCase.siteFraction = static_cast<double>(std::count(
        benchmarkCase.mask.begin(), benchmarkCase.mask.end(), static_cast<BinaryVoxelType>(1))) /
        static_cast<double>(benchmarkCase.mask.size());
    benchmarkCase.itkMask = buildItkMaskImage(benchmarkCase.mask, dims, spacing);
    return benchmarkCase;
}

BenchmarkCase makeSyntheticCase(GeneratorKind generator,
                                const std::array<int, 3> &dims,
                                const std::array<double, 3> &spacing,
                                std::uint32_t seed) {
    switch (generator) {
        case GeneratorKind::Shells:
            return makeShellsCase(dims, spacing);
        case GeneratorKind::PackedSpheres:
            return makePackedSpheresCase(dims, spacing);
        case GeneratorKind::SparseSites:
            return makeSparseSitesCase(dims, spacing, seed);
    }
    throw std::runtime_error("Unknown synthetic generator.");
}

std::vector<BenchmarkCase> buildCases(const BenchmarkOptions &options) {
    if (options.inputPath.has_value()) {
        return {loadCaseFromFile(*options.inputPath, options.threshold, options.spacingOverride)};
    }

    std::vector<BenchmarkCase> cases;
    if (options.generator.has_value() && options.requestedSize.has_value()) {
        const auto spacing = options.spacingOverride.value_or(std::array<double, 3>{1.0, 1.0, 1.0});
        cases.push_back(makeSyntheticCase(*options.generator, *options.requestedSize, spacing, options.seed));
        return cases;
    }

    const std::vector<std::pair<std::array<int, 3>, std::array<double, 3>>> defaultSuites = {
        {{{128, 128, 128}}, {{1.0, 1.0, 1.0}}},
        {{{256, 256, 256}}, {{1.0, 1.0, 1.0}}},
        {{{384, 384, 384}}, {{1.0, 1.0, 1.0}}},
        {{{256, 256, 128}}, {{1.0, 1.0, 3.0}}},
    };
    const std::vector<GeneratorKind> generators = options.generator.has_value()
        ? std::vector<GeneratorKind>{*options.generator}
        : std::vector<GeneratorKind>{GeneratorKind::Shells, GeneratorKind::PackedSpheres, GeneratorKind::SparseSites};

    std::uint32_t generatorSeed = options.seed;
    for (const auto generator : generators) {
        for (const auto &suite : defaultSuites) {
            cases.push_back(makeSyntheticCase(generator,
                                              options.requestedSize.value_or(suite.first),
                                              options.spacingOverride.value_or(suite.second),
                                              generatorSeed++));
            if (options.requestedSize.has_value()) {
                break;
            }
        }
    }
    return cases;
}

AccuracyMetrics computeAccuracy(const std::vector<DistanceVoxelType> &reference,
                                const std::vector<DistanceVoxelType> &candidate) {
    TRACE_EVENT("compare", "compute_accuracy", "voxels", static_cast<uint64_t>(reference.size()));
    if (reference.size() != candidate.size()) {
        throw std::runtime_error("Accuracy comparison requires equal-sized outputs.");
    }

    AccuracyMetrics accuracy;
    double sumAbsError = 0.0;
    double sumSquaredError = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const double error = std::abs(static_cast<double>(reference[i]) - static_cast<double>(candidate[i]));
        accuracy.maxAbsError = std::max(accuracy.maxAbsError, error);
        sumAbsError += error;
        sumSquaredError += error * error;
        if (error > kErrorTolerance) {
            ++accuracy.mismatchCount;
        }
    }
    const double denominator = reference.empty() ? 1.0 : static_cast<double>(reference.size());
    accuracy.meanAbsError = sumAbsError / denominator;
    accuracy.rmse = std::sqrt(sumSquaredError / denominator);
    return accuracy;
}

struct SeedComponent {
    SeedLabelType label = 0;
    std::size_t voxelCount = 0;
    std::array<double, 3> centroid{0.0, 0.0, 0.0};
};

template <typename TImage>
std::vector<typename TImage::PixelType> flattenLabelImage(typename TImage::Pointer image) {
    using PixelType = typename TImage::PixelType;
    std::vector<PixelType> labels;
    labels.reserve(image->GetLargestPossibleRegion().GetNumberOfPixels());
    itk::ImageRegionConstIterator<TImage> iterator(image, image->GetLargestPossibleRegion());
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        labels.push_back(iterator.Get());
    }
    return labels;
}

std::vector<SeedLabelType> flattenSeedImage(SeedImageType::Pointer image) {
    return flattenLabelImage<SeedImageType>(image);
}

std::size_t countPositiveLabels(const std::vector<SeedLabelType> &labels) {
    std::unordered_set<SeedLabelType> uniqueLabels;
    for (SeedLabelType label : labels) {
        if (label != 0) {
            uniqueLabels.insert(label);
        }
    }
    return uniqueLabels.size();
}

std::vector<SeedLabelType> flattenWatershedImage(SeedImageType::Pointer image) {
    std::vector<SeedLabelType> labels;
    labels.reserve(image->GetLargestPossibleRegion().GetNumberOfPixels());
    itk::ImageRegionConstIterator<SeedImageType> iterator(image, image->GetLargestPossibleRegion());
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        labels.push_back(iterator.Get());
    }
    return labels;
}

std::vector<SeedComponent> summarizeSeedComponents(const std::vector<SeedLabelType> &labels,
                                                   const std::array<int, 3> &dims) {
    const SeedLabelType maxLabel = labels.empty() ? 0 : *std::max_element(labels.begin(), labels.end());
    struct Accumulator {
        std::size_t count = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };
    std::vector<Accumulator> accumulators(static_cast<std::size_t>(maxLabel) + 1);
    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                const SeedLabelType label = labels[flatIndex(x, y, z, dims)];
                if (label == 0) {
                    continue;
                }
                auto &acc = accumulators[static_cast<std::size_t>(label)];
                acc.count += 1;
                acc.x += static_cast<double>(x);
                acc.y += static_cast<double>(y);
                acc.z += static_cast<double>(z);
            }
        }
    }

    std::vector<SeedComponent> components;
    for (SeedLabelType label = 1; label <= maxLabel; ++label) {
        const auto &acc = accumulators[static_cast<std::size_t>(label)];
        if (acc.count == 0) {
            continue;
        }
        SeedComponent component;
        component.label = label;
        component.voxelCount = acc.count;
        component.centroid = {
            acc.x / static_cast<double>(acc.count),
            acc.y / static_cast<double>(acc.count),
            acc.z / static_cast<double>(acc.count)
        };
        components.push_back(component);
    }
    return components;
}

SeedComparisonMetrics compareSeeds(const std::vector<SeedLabelType> &referenceSeeds,
                                   const std::vector<SeedLabelType> &candidateSeeds,
                                   const std::array<int, 3> &dims) {
    TRACE_EVENT("compare", "compare_seeds", "voxels", static_cast<uint64_t>(referenceSeeds.size()));
    if (referenceSeeds.size() != candidateSeeds.size()) {
        throw std::runtime_error("Seed comparison requires equal-sized outputs.");
    }

    SeedComparisonMetrics metrics;
    for (std::size_t i = 0; i < referenceSeeds.size(); ++i) {
        const bool referenceNonZero = referenceSeeds[i] != 0;
        const bool candidateNonZero = candidateSeeds[i] != 0;
        metrics.referenceSeedVoxels += referenceNonZero ? 1 : 0;
        metrics.candidateSeedVoxels += candidateNonZero ? 1 : 0;
        metrics.overlappingSeedVoxels += (referenceNonZero && candidateNonZero) ? 1 : 0;
    }
    const std::size_t unionCount = metrics.referenceSeedVoxels + metrics.candidateSeedVoxels - metrics.overlappingSeedVoxels;
    metrics.voxelIoU = unionCount == 0 ? 1.0 : static_cast<double>(metrics.overlappingSeedVoxels) / static_cast<double>(unionCount);

    const auto referenceComponents = summarizeSeedComponents(referenceSeeds, dims);
    const auto candidateComponents = summarizeSeedComponents(candidateSeeds, dims);
    metrics.referenceSeedCount = referenceComponents.size();
    metrics.candidateSeedCount = candidateComponents.size();

    std::vector<bool> candidateUsed(candidateComponents.size(), false);
    double matchedDistanceSum = 0.0;
    const double maxMatchDistance = 3.0;
    for (const auto &referenceComponent : referenceComponents) {
        double bestDistance = std::numeric_limits<double>::infinity();
        std::size_t bestIndex = candidateComponents.size();
        for (std::size_t i = 0; i < candidateComponents.size(); ++i) {
            if (candidateUsed[i]) {
                continue;
            }
            const auto &candidateComponent = candidateComponents[i];
            const double dx = referenceComponent.centroid[0] - candidateComponent.centroid[0];
            const double dy = referenceComponent.centroid[1] - candidateComponent.centroid[1];
            const double dz = referenceComponent.centroid[2] - candidateComponent.centroid[2];
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
            }
        }

        if (bestIndex != candidateComponents.size() && bestDistance <= maxMatchDistance) {
            candidateUsed[bestIndex] = true;
            metrics.matchedSeedCount += 1;
            matchedDistanceSum += bestDistance;
        }
    }

    metrics.missedReferenceSeeds = metrics.referenceSeedCount - metrics.matchedSeedCount;
    metrics.extraCandidateSeeds = metrics.candidateSeedCount - metrics.matchedSeedCount;
    metrics.meanMatchedCentroidDistance =
        metrics.matchedSeedCount == 0 ? 0.0 : matchedDistanceSum / static_cast<double>(metrics.matchedSeedCount);
    return metrics;
}

WatershedComparisonMetrics compareWatershedLabels(const std::vector<SeedLabelType> &referenceLabels,
                                                  const std::vector<SeedLabelType> &candidateLabels) {
    TRACE_EVENT("compare", "compare_watershed_labels", "voxels", static_cast<uint64_t>(referenceLabels.size()));
    if (referenceLabels.size() != candidateLabels.size()) {
        throw std::runtime_error("Watershed comparison requires equal-sized outputs.");
    }

    WatershedComparisonMetrics metrics;
    metrics.referenceSegmentCount = countPositiveLabels(referenceLabels);
    metrics.candidateSegmentCount = countPositiveLabels(candidateLabels);
    metrics.referenceZeroLabelCount = static_cast<std::size_t>(
        std::count(referenceLabels.begin(), referenceLabels.end(), static_cast<SeedLabelType>(0)));
    metrics.candidateZeroLabelCount = static_cast<std::size_t>(
        std::count(candidateLabels.begin(), candidateLabels.end(), static_cast<SeedLabelType>(0)));

    for (std::size_t i = 0; i < referenceLabels.size(); ++i) {
        if (referenceLabels[i] != candidateLabels[i]) {
            ++metrics.differingVoxelCount;
        }
    }

    const double denominator = referenceLabels.empty() ? 1.0 : static_cast<double>(referenceLabels.size());
    metrics.matchingVoxelFraction =
        static_cast<double>(referenceLabels.size() - metrics.differingVoxelCount) / denominator;
    return metrics;
}

void scaleRunMetrics(RunMetrics &metrics, double scale) {
    metrics.elapsedMs *= scale;
    metrics.distanceMapMs *= scale;
    metrics.seedExtractionMs *= scale;
    metrics.invertMs *= scale;
    metrics.watershedMs *= scale;
    metrics.quantizeMs *= scale;
    metrics.watershedInitMs *= scale;
    metrics.watershedFloodMs *= scale;
    metrics.xPassMs *= scale;
    metrics.yPassMs *= scale;
    metrics.zPassMs *= scale;
    metrics.megaVoxelsPerSecond *= scale;
    metrics.rssBeforeMB *= scale;
    metrics.rssAfterMB *= scale;
    metrics.peakRssMB *= scale;
}

void accumulateRunMetrics(RunMetrics &target, const RunMetrics &source) {
    target.elapsedMs += source.elapsedMs;
    target.distanceMapMs += source.distanceMapMs;
    target.seedExtractionMs += source.seedExtractionMs;
    target.invertMs += source.invertMs;
    target.watershedMs += source.watershedMs;
    target.quantizeMs += source.quantizeMs;
    target.watershedInitMs += source.watershedInitMs;
    target.watershedFloodMs += source.watershedFloodMs;
    target.xPassMs += source.xPassMs;
    target.yPassMs += source.yPassMs;
    target.zPassMs += source.zPassMs;
    target.megaVoxelsPerSecond += source.megaVoxelsPerSecond;
    target.rssBeforeMB += source.rssBeforeMB;
    target.rssAfterMB += source.rssAfterMB;
    target.peakRssMB += source.peakRssMB;
    target.inputBytes = source.inputBytes;
    target.outputBytes = source.outputBytes;
    target.scratchBytes = source.scratchBytes;
}

template <typename Runner>
BackendRunResult runWithProfileLoop(Runner &&runner, double profileMinSeconds) {
    TRACE_EVENT("benchmark", "profile_loop", "profile_min_seconds", profileMinSeconds);
    TRACE_EVENT("benchmark", "profile_loop_iteration", "iteration", 1u);
    BackendRunResult lastResult = runner();
    if (profileMinSeconds <= 0.0) {
        lastResult.metrics.profileMinSeconds = 0.0;
        lastResult.metrics.loopCount = 1;
        return lastResult;
    }

    RunMetrics accumulatedMetrics = lastResult.metrics;
    std::size_t loopCount = 1;
    while (accumulatedMetrics.elapsedMs < profileMinSeconds * 1000.0) {
        TRACE_EVENT("benchmark", "profile_loop_iteration", "iteration", static_cast<uint64_t>(loopCount + 1));
        lastResult = runner();
        accumulateRunMetrics(accumulatedMetrics, lastResult.metrics);
        ++loopCount;
    }

    const double scale = 1.0 / static_cast<double>(loopCount);
    scaleRunMetrics(accumulatedMetrics, scale);
    accumulatedMetrics.profileMinSeconds = profileMinSeconds;
    accumulatedMetrics.loopCount = loopCount;
    lastResult.metrics = accumulatedMetrics;
    return lastResult;
}

const char *algorithmName(AlgorithmSelection selection) {
    switch (selection) {
        case AlgorithmSelection::Maurer:
            return "maurer";
        case AlgorithmSelection::FH:
            return "fh";
        case AlgorithmSelection::All:
            return "all";
    }
    return "unknown";
}

const char *seedExtractorNameLiteral(SeedExtractorKind kind) {
    switch (kind) {
        case SeedExtractorKind::LocalMaxima:
            return "local-maxima";
        case SeedExtractorKind::HConvex:
            return "h-convex";
        case SeedExtractorKind::All:
            return "all";
    }
    return "unknown";
}

struct WatershedInputs {
    DistanceImageType::Pointer distanceMap;
    DistanceImageType::Pointer invertedDistanceMap;
    SeedImageType::Pointer seeds;
    RunMetrics metrics;
};

WatershedInputs buildWatershedInputs(const BenchmarkCase &benchmarkCase,
                                     int threadCount,
                                     SeedExtractorKind extractorKind) {
    TRACE_EVENT("watershed", "build_watershed_inputs",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount,
                "seed_extractor", seedExtractorNameLiteral(extractorKind));
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);

    WatershedInputs inputs;
    const double distanceStart = wallTimeSeconds();
    BinaryImageType::Pointer maskImage = benchmarkCase.itkMask;
    generateDistanceMap(maskImage, inputs.distanceMap, 0.0, DistanceMapAlgorithm::Maurer, threadCount);
    const double distanceEnd = wallTimeSeconds();

    const double seedsStart = wallTimeSeconds();
    inputs.seeds = extractSeedsFromDistanceImage(inputs.distanceMap, extractorKind);
    const double seedsEnd = wallTimeSeconds();

    const double invertStart = wallTimeSeconds();
    invertDistanceMap(inputs.distanceMap, inputs.invertedDistanceMap);
    const double invertEnd = wallTimeSeconds();

    inputs.metrics.distanceMapMs = (distanceEnd - distanceStart) * 1000.0;
    inputs.metrics.seedExtractionMs = (seedsEnd - seedsStart) * 1000.0;
    inputs.metrics.invertMs = (invertEnd - invertStart) * 1000.0;
    return inputs;
}

BackendRunResult runMaurerOnce(const BenchmarkCase &benchmarkCase, int threadCount) {
    TRACE_EVENT("distance_map", "run_maurer_distance_map",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount);
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);

    MemorySnapshot before = readProcessMemory();
    const double startTime = wallTimeSeconds();

    using MaurerFilterType = itk::SignedMaurerDistanceMapImageFilter<BinaryImageType, DistanceImageType>;
    auto distanceFilter = MaurerFilterType::New();
    distanceFilter->SetInput(benchmarkCase.itkMask);
    distanceFilter->SetBackgroundValue(0);
    distanceFilter->SquaredDistanceOn();
    distanceFilter->UseImageSpacingOn();
    distanceFilter->InsideIsPositiveOff();
    distanceFilter->Update();

    auto outputImage = distanceFilter->GetOutput();
    std::vector<DistanceVoxelType> output;
    output.reserve(benchmarkCase.mask.size());
    itk::ImageRegionConstIterator<DistanceImageType> iterator(outputImage, outputImage->GetLargestPossibleRegion());
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        output.push_back(static_cast<DistanceVoxelType>(std::abs(iterator.Get())));
    }

    const double endTime = wallTimeSeconds();
    MemorySnapshot after = readProcessMemory();

    BackendRunResult result;
    result.distances = std::move(output);
    result.metrics.elapsedMs = (endTime - startTime) * 1000.0;
    result.metrics.megaVoxelsPerSecond =
        static_cast<double>(benchmarkCase.mask.size()) / std::max(1e-9, endTime - startTime) / 1.0e6;
    result.metrics.rssBeforeMB = before.currentMB;
    result.metrics.rssAfterMB = after.currentMB;
    result.metrics.peakRssMB = after.peakMB;
    result.metrics.inputBytes = benchmarkCase.mask.size() * sizeof(BinaryVoxelType);
    result.metrics.outputBytes = result.distances.size() * sizeof(DistanceVoxelType);
    result.metrics.scratchBytes = 0;
    result.metrics.distanceMapMs = result.metrics.elapsedMs;
    return result;
}

BackendRunResult runMaurerSeedsOnce(const BenchmarkCase &benchmarkCase,
                                    int threadCount,
                                    SeedExtractorKind extractorKind) {
    TRACE_EVENT("seeds", "run_maurer_seeds",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount,
                "seed_extractor", seedExtractorNameLiteral(extractorKind));
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(threadCount);

    MemorySnapshot before = readProcessMemory();
    const double startTime = wallTimeSeconds();

    using MaurerFilterType = itk::SignedMaurerDistanceMapImageFilter<BinaryImageType, DistanceImageType>;
    auto distanceFilter = MaurerFilterType::New();
    distanceFilter->SetInput(benchmarkCase.itkMask);
    distanceFilter->SetBackgroundValue(0);
    distanceFilter->SquaredDistanceOff();
    distanceFilter->UseImageSpacingOff();
    distanceFilter->InsideIsPositiveOff();

    const double distanceStart = wallTimeSeconds();
    distanceFilter->Update();
    auto distanceImage = distanceFilter->GetOutput();
    const double distanceEnd = wallTimeSeconds();

    const double seedsStart = wallTimeSeconds();
    auto seedImage = extractSeedsFromDistanceImage(distanceImage, extractorKind);
    const double seedsEnd = wallTimeSeconds();

    std::vector<DistanceVoxelType> distances;
    distances.reserve(benchmarkCase.mask.size());
    itk::ImageRegionConstIterator<DistanceImageType> distanceIterator(distanceImage, distanceImage->GetLargestPossibleRegion());
    for (distanceIterator.GoToBegin(); !distanceIterator.IsAtEnd(); ++distanceIterator) {
        distances.push_back(distanceIterator.Get());
    }

    const double endTime = wallTimeSeconds();
    MemorySnapshot after = readProcessMemory();

    BackendRunResult result;
    result.distances = std::move(distances);
    result.seeds = flattenSeedImage(seedImage);
    result.metrics.elapsedMs = (endTime - startTime) * 1000.0;
    result.metrics.distanceMapMs = (distanceEnd - distanceStart) * 1000.0;
    result.metrics.seedExtractionMs = (seedsEnd - seedsStart) * 1000.0;
    result.metrics.megaVoxelsPerSecond =
        static_cast<double>(benchmarkCase.mask.size()) / std::max(1e-9, endTime - startTime) / 1.0e6;
    result.metrics.rssBeforeMB = before.currentMB;
    result.metrics.rssAfterMB = after.currentMB;
    result.metrics.peakRssMB = after.peakMB;
    result.metrics.inputBytes = benchmarkCase.mask.size() * sizeof(BinaryVoxelType);
    result.metrics.outputBytes =
        result.distances.size() * sizeof(DistanceVoxelType) + result.seeds.size() * sizeof(SeedLabelType);
    result.metrics.scratchBytes = 0;
    return result;
}

BackendRunResult runFHOnce(const BenchmarkCase &benchmarkCase, int threadCount) {
    TRACE_EVENT("distance_map", "run_fh_distance_map",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount);
    MemorySnapshot before = readProcessMemory();
    const auto fhResult = distance_map_benchmark::runBoundaryAwareSquaredEdt(
        benchmarkCase.mask, benchmarkCase.dims, benchmarkCase.spacing, threadCount);
    MemorySnapshot after = readProcessMemory();

    BackendRunResult result;
    result.distances = fhResult.distances;
    result.metrics.elapsedMs = fhResult.metrics.elapsedMs;
    result.metrics.xPassMs = fhResult.metrics.xPassMs;
    result.metrics.yPassMs = fhResult.metrics.yPassMs;
    result.metrics.zPassMs = fhResult.metrics.zPassMs;
    result.metrics.megaVoxelsPerSecond =
        static_cast<double>(benchmarkCase.mask.size()) / std::max(1e-9, fhResult.metrics.elapsedMs / 1000.0) / 1.0e6;
    result.metrics.rssBeforeMB = before.currentMB;
    result.metrics.rssAfterMB = after.currentMB;
    result.metrics.peakRssMB = after.peakMB;
    result.metrics.inputBytes = benchmarkCase.mask.size() * sizeof(BinaryVoxelType);
    result.metrics.outputBytes = result.distances.size() * sizeof(DistanceVoxelType);
    result.metrics.scratchBytes = fhResult.metrics.scratchBytes;
    result.metrics.distanceMapMs = result.metrics.elapsedMs;
    return result;
}

BackendRunResult runFHSeedsOnce(const BenchmarkCase &benchmarkCase,
                                int threadCount,
                                SeedExtractorKind extractorKind) {
    TRACE_EVENT("seeds", "run_fh_seeds",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount,
                "seed_extractor", seedExtractorNameLiteral(extractorKind));
    MemorySnapshot before = readProcessMemory();
    const auto unitSpacing = std::array<double, 3>{1.0, 1.0, 1.0};
    const auto fhResult = distance_map_benchmark::runBoundaryAwareSquaredEdt(
        benchmarkCase.mask, benchmarkCase.dims, unitSpacing, threadCount);

    std::vector<DistanceVoxelType> metricDistances = fhResult.distances;
    for (auto &value : metricDistances) {
        value = value <= 0.0f ? 0.0f : static_cast<DistanceVoxelType>(std::sqrt(static_cast<double>(value)));
    }

    const double seedsStart = wallTimeSeconds();
    auto distanceImage = buildItkDistanceImage(metricDistances, benchmarkCase.dims, benchmarkCase.spacing);
    auto seedImage = extractSeedsFromDistanceImage(distanceImage, extractorKind);
    const double seedsEnd = wallTimeSeconds();
    MemorySnapshot after = readProcessMemory();

    BackendRunResult result;
    result.distances = std::move(metricDistances);
    result.seeds = flattenSeedImage(seedImage);
    result.metrics.elapsedMs = fhResult.metrics.elapsedMs + (seedsEnd - seedsStart) * 1000.0;
    result.metrics.distanceMapMs = fhResult.metrics.elapsedMs;
    result.metrics.seedExtractionMs = (seedsEnd - seedsStart) * 1000.0;
    result.metrics.xPassMs = fhResult.metrics.xPassMs;
    result.metrics.yPassMs = fhResult.metrics.yPassMs;
    result.metrics.zPassMs = fhResult.metrics.zPassMs;
    result.metrics.megaVoxelsPerSecond =
        static_cast<double>(benchmarkCase.mask.size()) / std::max(1e-9, result.metrics.elapsedMs / 1000.0) / 1.0e6;
    result.metrics.rssBeforeMB = before.currentMB;
    result.metrics.rssAfterMB = after.currentMB;
    result.metrics.peakRssMB = after.peakMB;
    result.metrics.inputBytes = benchmarkCase.mask.size() * sizeof(BinaryVoxelType);
    result.metrics.outputBytes =
        result.distances.size() * sizeof(DistanceVoxelType) + result.seeds.size() * sizeof(SeedLabelType);
    result.metrics.scratchBytes = fhResult.metrics.scratchBytes;
    return result;
}

BackendRunResult runItkWatershedOnce(const BenchmarkCase &benchmarkCase,
                                     int threadCount,
                                     SeedExtractorKind extractorKind) {
    TRACE_EVENT("watershed", "run_itk_watershed",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount,
                "seed_extractor", seedExtractorNameLiteral(extractorKind));
    MemorySnapshot before = readProcessMemory();
    WatershedInputs inputs = buildWatershedInputs(benchmarkCase, threadCount, extractorKind);

    WatershedRunOptions options;
    options.algorithm = WatershedAlgorithm::MorphologicalWatershedFromMarkers;

    const double watershedStart = wallTimeSeconds();
    SeedImageType::Pointer watershedImage;
    runWatershed(inputs.invertedDistanceMap, inputs.seeds, watershedImage, options);
    const double watershedEnd = wallTimeSeconds();
    MemorySnapshot after = readProcessMemory();

    BackendRunResult result;
    result.distances = flattenLabelImage<DistanceImageType>(inputs.distanceMap);
    result.seeds = flattenSeedImage(inputs.seeds);
    result.watershedLabels = flattenWatershedImage(watershedImage);
    result.metrics = inputs.metrics;
    result.metrics.watershedMs = (watershedEnd - watershedStart) * 1000.0;
    result.metrics.elapsedMs =
        result.metrics.distanceMapMs + result.metrics.seedExtractionMs + result.metrics.invertMs + result.metrics.watershedMs;
    result.metrics.megaVoxelsPerSecond =
        static_cast<double>(benchmarkCase.mask.size()) / std::max(1e-9, result.metrics.elapsedMs / 1000.0) / 1.0e6;
    result.metrics.rssBeforeMB = before.currentMB;
    result.metrics.rssAfterMB = after.currentMB;
    result.metrics.peakRssMB = after.peakMB;
    result.metrics.inputBytes = benchmarkCase.mask.size() * sizeof(BinaryVoxelType);
    result.metrics.outputBytes =
        result.distances.size() * sizeof(DistanceVoxelType) +
        result.seeds.size() * sizeof(SeedLabelType) +
        result.watershedLabels.size() * sizeof(SeedLabelType);
    result.metrics.scratchBytes = 0;
    return result;
}

BackendRunResult runFastMarkerWatershedOnce(const BenchmarkCase &benchmarkCase,
                                            int threadCount,
                                            SeedExtractorKind extractorKind) {
    TRACE_EVENT("watershed", "run_fast_marker_watershed",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount,
                "seed_extractor", seedExtractorNameLiteral(extractorKind));
    MemorySnapshot before = readProcessMemory();
    WatershedInputs inputs = buildWatershedInputs(benchmarkCase, threadCount, extractorKind);

    WatershedRunOptions options;
    options.algorithm = WatershedAlgorithm::FastMarkerWatershed;

    segment_puzzler::FastMarkerWatershedMetrics fastMetrics;
    const double watershedStart = wallTimeSeconds();
    SeedImageType::Pointer watershedImage;
    runWatershed(inputs.invertedDistanceMap, inputs.seeds, watershedImage, options, &fastMetrics);
    const double watershedEnd = wallTimeSeconds();
    MemorySnapshot after = readProcessMemory();

    BackendRunResult result;
    result.distances = flattenLabelImage<DistanceImageType>(inputs.distanceMap);
    result.seeds = flattenSeedImage(inputs.seeds);
    result.watershedLabels = flattenWatershedImage(watershedImage);
    result.metrics = inputs.metrics;
    result.metrics.watershedMs = (watershedEnd - watershedStart) * 1000.0;
    result.metrics.quantizeMs = fastMetrics.quantizeMs;
    result.metrics.watershedInitMs = fastMetrics.initMs;
    result.metrics.watershedFloodMs = fastMetrics.floodMs;
    result.metrics.elapsedMs =
        result.metrics.distanceMapMs + result.metrics.seedExtractionMs + result.metrics.invertMs + result.metrics.watershedMs;
    result.metrics.megaVoxelsPerSecond =
        static_cast<double>(benchmarkCase.mask.size()) / std::max(1e-9, result.metrics.elapsedMs / 1000.0) / 1.0e6;
    result.metrics.rssBeforeMB = before.currentMB;
    result.metrics.rssAfterMB = after.currentMB;
    result.metrics.peakRssMB = after.peakMB;
    result.metrics.inputBytes = benchmarkCase.mask.size() * sizeof(BinaryVoxelType);
    result.metrics.outputBytes =
        result.distances.size() * sizeof(DistanceVoxelType) +
        result.seeds.size() * sizeof(SeedLabelType) +
        result.watershedLabels.size() * sizeof(SeedLabelType);
    result.metrics.scratchBytes = fastMetrics.scratchBytes + fastMetrics.bucketBytes;
    return result;
}

BackendRunResult runMaurer(const BenchmarkCase &benchmarkCase, int threadCount, double profileMinSeconds) {
    return runWithProfileLoop(
        [&]() { return runMaurerOnce(benchmarkCase, threadCount); },
        profileMinSeconds);
}

BackendRunResult runMaurerSeeds(const BenchmarkCase &benchmarkCase,
                                int threadCount,
                                SeedExtractorKind extractorKind,
                                double profileMinSeconds) {
    return runWithProfileLoop(
        [&]() { return runMaurerSeedsOnce(benchmarkCase, threadCount, extractorKind); },
        profileMinSeconds);
}

BackendRunResult runFH(const BenchmarkCase &benchmarkCase, int threadCount, double profileMinSeconds) {
    return runWithProfileLoop(
        [&]() { return runFHOnce(benchmarkCase, threadCount); },
        profileMinSeconds);
}

BackendRunResult runFHSeeds(const BenchmarkCase &benchmarkCase,
                            int threadCount,
                            SeedExtractorKind extractorKind,
                            double profileMinSeconds) {
    return runWithProfileLoop(
        [&]() { return runFHSeedsOnce(benchmarkCase, threadCount, extractorKind); },
        profileMinSeconds);
}

BackendRunResult runItkWatershed(const BenchmarkCase &benchmarkCase,
                                 int threadCount,
                                 SeedExtractorKind extractorKind,
                                 double profileMinSeconds) {
    return runWithProfileLoop(
        [&]() { return runItkWatershedOnce(benchmarkCase, threadCount, extractorKind); },
        profileMinSeconds);
}

BackendRunResult runFastMarkerWatershed(const BenchmarkCase &benchmarkCase,
                                        int threadCount,
                                        SeedExtractorKind extractorKind,
                                        double profileMinSeconds) {
    return runWithProfileLoop(
        [&]() { return runFastMarkerWatershedOnce(benchmarkCase, threadCount, extractorKind); },
        profileMinSeconds);
}

void warmupCase(const BenchmarkCase &benchmarkCase,
                int threadCount,
                AlgorithmSelection algorithm,
                BenchmarkMode mode,
                SeedExtractorKind seedExtractor,
                WatershedAlgorithmSelection watershedAlgorithm,
                int warmups) {
    TRACE_EVENT("benchmark", "warmup_case",
                "case", benchmarkCase.name.c_str(),
                "threads", threadCount,
                "warmups", warmups,
                "mode", benchmarkModeName(mode));
    for (int i = 0; i < warmups; ++i) {
        TRACE_EVENT("benchmark", "warmup_iteration", "iteration", i + 1);
        if (mode == BenchmarkMode::Watershed) {
            if (watershedAlgorithm == WatershedAlgorithmSelection::Itk) {
                (void)runItkWatershedOnce(benchmarkCase, threadCount, seedExtractor);
            } else if (watershedAlgorithm == WatershedAlgorithmSelection::FastMarker) {
                (void)runFastMarkerWatershedOnce(benchmarkCase, threadCount, seedExtractor);
            }
        } else if (algorithm == AlgorithmSelection::Maurer) {
            (void)(mode == BenchmarkMode::Seeds
                ? runMaurerSeedsOnce(benchmarkCase, threadCount, seedExtractor)
                : runMaurerOnce(benchmarkCase, threadCount));
        } else if (algorithm == AlgorithmSelection::FH) {
            (void)(mode == BenchmarkMode::Seeds
                ? runFHSeedsOnce(benchmarkCase, threadCount, seedExtractor)
                : runFHOnce(benchmarkCase, threadCount));
        }
    }
}

void writeCsv(const std::string &path, const std::vector<CsvRow> &rows) {
    std::ofstream csv(path);
    csv << "case_name,source_kind,algorithm,seed_extractor,dim_x,dim_y,dim_z,spacing_x,spacing_y,spacing_z,"
           "foreground_fraction,threads,repetition,loop_count,profile_min_seconds,elapsed_ms,distance_map_ms,seed_extraction_ms,invert_ms,watershed_ms,quantize_ms,watershed_init_ms,watershed_flood_ms,x_pass_ms,y_pass_ms,z_pass_ms,"
           "mvox_per_s,input_bytes,output_bytes,scratch_bytes,rss_before_mb,rss_after_mb,"
           "peak_rss_mb,max_abs_error,mean_abs_error,rmse,mismatch_count,"
           "reference_seed_count,candidate_seed_count,reference_seed_voxels,candidate_seed_voxels,"
           "overlapping_seed_voxels,seed_voxel_iou,matched_seed_count,missed_reference_seeds,"
           "extra_candidate_seeds,mean_matched_centroid_distance,"
           "reference_segment_count,candidate_segment_count,reference_zero_label_count,candidate_zero_label_count,differing_voxel_count,matching_voxel_fraction\n";
    csv << std::fixed << std::setprecision(6);
    for (const auto &row : rows) {
        csv << row.caseName << ','
            << row.sourceKind << ','
            << row.algorithm << ','
            << row.seedExtractor << ','
            << row.dims[0] << ','
            << row.dims[1] << ','
            << row.dims[2] << ','
            << row.spacing[0] << ','
            << row.spacing[1] << ','
            << row.spacing[2] << ','
            << row.siteFraction << ','
            << row.threads << ','
            << row.repetition << ','
            << row.metrics.loopCount << ','
            << row.metrics.profileMinSeconds << ','
            << row.metrics.elapsedMs << ','
            << row.metrics.distanceMapMs << ','
            << row.metrics.seedExtractionMs << ','
            << row.metrics.invertMs << ','
            << row.metrics.watershedMs << ','
            << row.metrics.quantizeMs << ','
            << row.metrics.watershedInitMs << ','
            << row.metrics.watershedFloodMs << ','
            << row.metrics.xPassMs << ','
            << row.metrics.yPassMs << ','
            << row.metrics.zPassMs << ','
            << row.metrics.megaVoxelsPerSecond << ','
            << row.metrics.inputBytes << ','
            << row.metrics.outputBytes << ','
            << row.metrics.scratchBytes << ','
            << row.metrics.rssBeforeMB << ','
            << row.metrics.rssAfterMB << ','
            << row.metrics.peakRssMB << ','
            << row.accuracy.maxAbsError << ','
            << row.accuracy.meanAbsError << ','
            << row.accuracy.rmse << ','
            << row.accuracy.mismatchCount << ','
            << row.seedMetrics.referenceSeedCount << ','
            << row.seedMetrics.candidateSeedCount << ','
            << row.seedMetrics.referenceSeedVoxels << ','
            << row.seedMetrics.candidateSeedVoxels << ','
            << row.seedMetrics.overlappingSeedVoxels << ','
            << row.seedMetrics.voxelIoU << ','
            << row.seedMetrics.matchedSeedCount << ','
            << row.seedMetrics.missedReferenceSeeds << ','
            << row.seedMetrics.extraCandidateSeeds << ','
            << row.seedMetrics.meanMatchedCentroidDistance << ','
            << row.watershedMetrics.referenceSegmentCount << ','
            << row.watershedMetrics.candidateSegmentCount << ','
            << row.watershedMetrics.referenceZeroLabelCount << ','
            << row.watershedMetrics.candidateZeroLabelCount << ','
            << row.watershedMetrics.differingVoxelCount << ','
            << row.watershedMetrics.matchingVoxelFraction << '\n';
    }
}

void printRowSummary(const CsvRow &row) {
    std::cout << std::fixed << std::setprecision(3)
              << row.caseName << " | "
              << row.algorithm << " | seed_extractor=" << row.seedExtractor
              << " | threads=" << row.threads
              << " | rep=" << row.repetition
              << " | loops=" << row.metrics.loopCount
              << " | elapsed_ms=" << row.metrics.elapsedMs
              << " | mvox/s=" << row.metrics.megaVoxelsPerSecond;
    if (row.metrics.profileMinSeconds > 0.0) {
        std::cout << " | profile_min_s=" << row.metrics.profileMinSeconds;
    }
    if (row.metrics.distanceMapMs > 0.0 || row.metrics.seedExtractionMs > 0.0) {
        std::cout << " | distance/seed_ms=" << row.metrics.distanceMapMs << "/" << row.metrics.seedExtractionMs;
    }
    if (row.metrics.invertMs > 0.0 || row.metrics.watershedMs > 0.0) {
        std::cout << " | invert/ws_ms=" << row.metrics.invertMs << "/" << row.metrics.watershedMs;
    }
    if (row.metrics.quantizeMs > 0.0 || row.metrics.watershedInitMs > 0.0 || row.metrics.watershedFloodMs > 0.0) {
        std::cout << " | quant/init/flood_ms=" << row.metrics.quantizeMs << "/"
                  << row.metrics.watershedInitMs << "/" << row.metrics.watershedFloodMs;
    }
    if (row.algorithm == "fh") {
        std::cout << " | x/y/z_ms=" << row.metrics.xPassMs << "/"
                  << row.metrics.yPassMs << "/" << row.metrics.zPassMs;
    }
    std::cout << " | rss_mb=" << row.metrics.rssAfterMB
              << " | max_err=" << row.accuracy.maxAbsError
              << " | mismatches=" << row.accuracy.mismatchCount;
    if (row.seedMetrics.referenceSeedCount > 0 || row.seedMetrics.candidateSeedCount > 0) {
        std::cout << " | seeds(ref/cand/matched)="
                  << row.seedMetrics.referenceSeedCount << "/"
                  << row.seedMetrics.candidateSeedCount << "/"
                  << row.seedMetrics.matchedSeedCount
                  << " | seed_iou=" << row.seedMetrics.voxelIoU;
    }
    if (row.watershedMetrics.referenceSegmentCount > 0 || row.watershedMetrics.candidateSegmentCount > 0) {
        std::cout << " | segments(ref/cand)="
                  << row.watershedMetrics.referenceSegmentCount << "/"
                  << row.watershedMetrics.candidateSegmentCount
                  << " | voxel_match=" << row.watershedMetrics.matchingVoxelFraction;
    }
    std::cout
              << '\n';
}

} // namespace

int main(int argc, char **argv) {
    try {
        const BenchmarkOptions options = parseArguments(argc, argv);
        auto perfettoSession = options.perfettoOutputPath.has_value()
            ? std::optional<benchmark_tracing::PerfettoTraceSession>(
                benchmark_tracing::PerfettoTraceSession::start(*options.perfettoOutputPath, options.perfettoBufferKb))
            : std::nullopt;
        TRACE_EVENT("benchmark", "benchmark_distance_map",
                    "mode", benchmarkModeName(options.mode),
                    "algorithm", algorithmName(options.algorithmSelection),
                    "watershed_algorithm", watershedAlgorithmName(options.watershedAlgorithmSelection),
                    "seed_extractor", seedExtractorNameLiteral(options.seedExtractor),
                    "perfetto_enabled", perfettoSession.has_value());
        const std::vector<BenchmarkCase> cases = buildCases(options);
        std::vector<CsvRow> rows;
        const std::vector<SeedExtractorKind> seedExtractors =
            options.mode == BenchmarkMode::Edt
                ? std::vector<SeedExtractorKind>{SeedExtractorKind::LocalMaxima}
                : (options.seedExtractor == SeedExtractorKind::All
                    ? std::vector<SeedExtractorKind>{SeedExtractorKind::LocalMaxima, SeedExtractorKind::HConvex}
                    : std::vector<SeedExtractorKind>{options.seedExtractor});

        for (const auto &benchmarkCase : cases) {
            TRACE_EVENT("benchmark.case", "benchmark_case",
                        "case", benchmarkCase.name.c_str(),
                        "mode", benchmarkModeName(options.mode),
                        "site_fraction", benchmarkCase.siteFraction);
            std::cout << "Case: " << benchmarkCase.name
                      << " dims=" << benchmarkCase.dims[0] << "x" << benchmarkCase.dims[1] << "x" << benchmarkCase.dims[2]
                      << " spacing=" << benchmarkCase.spacing[0] << "," << benchmarkCase.spacing[1] << "," << benchmarkCase.spacing[2]
                      << " mode=" << benchmarkModeName(options.mode)
                      << " seed_extractor=" << (options.mode == BenchmarkMode::Edt ? "none" : distance_map_benchmark::seedExtractorName(options.seedExtractor))
                      << " foreground_fraction=" << benchmarkCase.siteFraction
                      << '\n';

            for (const int threadCount : options.threadCounts) {
                for (const auto seedExtractor : seedExtractors) {
                    TRACE_EVENT("benchmark.case", "case_thread_config",
                                "case", benchmarkCase.name.c_str(),
                                "threads", threadCount,
                                "seed_extractor", options.mode == BenchmarkMode::Edt ? "none"
                                    : seedExtractorNameLiteral(seedExtractor));
                    const std::string seedExtractorName =
                        options.mode == BenchmarkMode::Edt ? "none" : distance_map_benchmark::seedExtractorName(seedExtractor);

                    if (options.mode == BenchmarkMode::Watershed) {
                        warmupCase(benchmarkCase, threadCount, AlgorithmSelection::Maurer, options.mode,
                                   seedExtractor, WatershedAlgorithmSelection::Itk, options.warmups);
                        BackendRunResult itkReferenceRun =
                            runItkWatershed(benchmarkCase, threadCount, seedExtractor, options.profileMinSeconds);

                        if (options.watershedAlgorithmSelection == WatershedAlgorithmSelection::All ||
                            options.watershedAlgorithmSelection == WatershedAlgorithmSelection::Itk) {
                            CsvRow row;
                            row.caseName = benchmarkCase.name;
                            row.sourceKind = options.inputPath.has_value() ? "file" : "synthetic";
                            row.algorithm = "itk";
                            row.seedExtractor = seedExtractorName;
                            row.dims = benchmarkCase.dims;
                            row.spacing = benchmarkCase.spacing;
                            row.siteFraction = benchmarkCase.siteFraction;
                            row.threads = threadCount;
                            row.repetition = 1;
                            row.metrics = itkReferenceRun.metrics;
                            row.watershedMetrics = compareWatershedLabels(
                                itkReferenceRun.watershedLabels, itkReferenceRun.watershedLabels);
                            rows.push_back(row);
                            printRowSummary(rows.back());

                            for (int repetition = 2; repetition <= options.repetitions; ++repetition) {
                                BackendRunResult run =
                                    runItkWatershed(benchmarkCase, threadCount, seedExtractor, options.profileMinSeconds);
                                CsvRow repeatRow = row;
                                repeatRow.repetition = repetition;
                                repeatRow.metrics = run.metrics;
                                repeatRow.watershedMetrics = compareWatershedLabels(
                                    itkReferenceRun.watershedLabels, run.watershedLabels);
                                rows.push_back(repeatRow);
                                printRowSummary(rows.back());
                            }
                        }

                        if (options.watershedAlgorithmSelection == WatershedAlgorithmSelection::All ||
                            options.watershedAlgorithmSelection == WatershedAlgorithmSelection::FastMarker) {
                            warmupCase(benchmarkCase, threadCount, AlgorithmSelection::Maurer, options.mode,
                                       seedExtractor, WatershedAlgorithmSelection::FastMarker, options.warmups);
                            for (int repetition = 1; repetition <= options.repetitions; ++repetition) {
                                BackendRunResult run =
                                    runFastMarkerWatershed(benchmarkCase, threadCount, seedExtractor, options.profileMinSeconds);
                                CsvRow row;
                                row.caseName = benchmarkCase.name;
                                row.sourceKind = options.inputPath.has_value() ? "file" : "synthetic";
                                row.algorithm = "fast-marker";
                                row.seedExtractor = seedExtractorName;
                                row.dims = benchmarkCase.dims;
                                row.spacing = benchmarkCase.spacing;
                                row.siteFraction = benchmarkCase.siteFraction;
                                row.threads = threadCount;
                                row.repetition = repetition;
                                row.metrics = run.metrics;
                                row.watershedMetrics = compareWatershedLabels(
                                    itkReferenceRun.watershedLabels, run.watershedLabels);
                                rows.push_back(row);
                                printRowSummary(rows.back());
                            }
                        }
                        continue;
                    }

                    std::vector<DistanceVoxelType> maurerReferenceDistances;
                    std::vector<SeedLabelType> maurerReferenceSeeds;
                    warmupCase(benchmarkCase, threadCount, AlgorithmSelection::Maurer, options.mode,
                               seedExtractor, WatershedAlgorithmSelection::All, options.warmups);
                    BackendRunResult maurerReferenceRun =
                        options.mode == BenchmarkMode::Seeds
                            ? runMaurerSeeds(benchmarkCase, threadCount, seedExtractor, options.profileMinSeconds)
                            : runMaurer(benchmarkCase, threadCount, options.profileMinSeconds);
                    maurerReferenceDistances = maurerReferenceRun.distances;
                    maurerReferenceSeeds = maurerReferenceRun.seeds;

                    if (options.algorithmSelection == AlgorithmSelection::All ||
                        options.algorithmSelection == AlgorithmSelection::Maurer) {
                        CsvRow row;
                        row.caseName = benchmarkCase.name;
                        row.sourceKind = options.inputPath.has_value() ? "file" : "synthetic";
                        row.algorithm = "maurer";
                        row.seedExtractor = seedExtractorName;
                        row.dims = benchmarkCase.dims;
                        row.spacing = benchmarkCase.spacing;
                        row.siteFraction = benchmarkCase.siteFraction;
                        row.threads = threadCount;
                        row.repetition = 1;
                        row.metrics = maurerReferenceRun.metrics;
                        row.accuracy = {};
                        row.seedMetrics.referenceSeedCount = maurerReferenceSeeds.empty()
                            ? 0
                            : summarizeSeedComponents(maurerReferenceSeeds, benchmarkCase.dims).size();
                        row.seedMetrics.candidateSeedCount = row.seedMetrics.referenceSeedCount;
                        row.seedMetrics.referenceSeedVoxels = static_cast<std::size_t>(
                            std::count_if(maurerReferenceSeeds.begin(), maurerReferenceSeeds.end(), [](SeedLabelType v) { return v != 0; }));
                        row.seedMetrics.candidateSeedVoxels = row.seedMetrics.referenceSeedVoxels;
                        row.seedMetrics.overlappingSeedVoxels = row.seedMetrics.referenceSeedVoxels;
                        row.seedMetrics.voxelIoU = maurerReferenceSeeds.empty() ? 0.0 : 1.0;
                        row.seedMetrics.matchedSeedCount = row.seedMetrics.referenceSeedCount;
                        rows.push_back(row);
                        printRowSummary(rows.back());

                        for (int repetition = 2; repetition <= options.repetitions; ++repetition) {
                            BackendRunResult run =
                                options.mode == BenchmarkMode::Seeds
                                    ? runMaurerSeeds(benchmarkCase, threadCount, seedExtractor, options.profileMinSeconds)
                                    : runMaurer(benchmarkCase, threadCount, options.profileMinSeconds);
                            CsvRow repeatRow = row;
                            repeatRow.repetition = repetition;
                            repeatRow.metrics = run.metrics;
                            rows.push_back(repeatRow);
                            printRowSummary(rows.back());
                        }
                    }

                    if (options.algorithmSelection == AlgorithmSelection::All ||
                        options.algorithmSelection == AlgorithmSelection::FH) {
                        warmupCase(benchmarkCase, threadCount, AlgorithmSelection::FH, options.mode,
                                   seedExtractor, WatershedAlgorithmSelection::All, options.warmups);
                        for (int repetition = 1; repetition <= options.repetitions; ++repetition) {
                            BackendRunResult run =
                                options.mode == BenchmarkMode::Seeds
                                    ? runFHSeeds(benchmarkCase, threadCount, seedExtractor, options.profileMinSeconds)
                                    : runFH(benchmarkCase, threadCount, options.profileMinSeconds);
                            CsvRow row;
                            row.caseName = benchmarkCase.name;
                            row.sourceKind = options.inputPath.has_value() ? "file" : "synthetic";
                            row.algorithm = "fh";
                            row.seedExtractor = seedExtractorName;
                            row.dims = benchmarkCase.dims;
                            row.spacing = benchmarkCase.spacing;
                            row.siteFraction = benchmarkCase.siteFraction;
                            row.threads = threadCount;
                            row.repetition = repetition;
                            row.metrics = run.metrics;
                            row.accuracy = computeAccuracy(maurerReferenceDistances, run.distances);
                            if (options.mode == BenchmarkMode::Seeds) {
                                row.seedMetrics = compareSeeds(maurerReferenceSeeds, run.seeds, benchmarkCase.dims);
                            }
                            rows.push_back(row);
                            printRowSummary(rows.back());
                        }
                    }
                }
            }
        }

        writeCsv(options.csvPath, rows);
        std::cout << "Wrote benchmark results to " << options.csvPath << '\n';
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "benchmark_distance_map failed: " << exception.what() << '\n';
        return 1;
    }
}
