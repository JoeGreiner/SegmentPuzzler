#include "examples/benchmarks/BenchmarkPerfettoTracing.h"
#include "src/utils/DistanceMapCliIO.h"
#include "src/utils/WatershedRagAgglomeration.h"

#include <itkImageRegionIterator.h>
#include <itkImageFileReader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace {

using SegmentsImageType = dataType::SegmentsImageType;
using SegmentIdType = dataType::SegmentIdType;

double wallTimeSeconds() {
#ifdef USE_OMP
    return omp_get_wtime();
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

struct Options {
    std::optional<std::string> labelsPath;
    std::optional<std::string> boundaryPath;
    std::optional<std::string> thresholdMaskPath;
    std::string generator = "unique-grid";
    std::string boundaryGenerator = "constant-zero";
    std::string thresholdMaskGenerator = "all-open";
    std::array<int, 3> size{64, 64, 64};
    int repetitions = 3;
    int warmup = 1;
    std::vector<int> threadCounts{1};
    double profileMinSeconds = 0.0;
    std::optional<std::string> perfettoOutputPath;
    std::size_t perfettoBufferKb = 32768;
    std::string csvPath;
    segment_puzzler::BoundaryEvidenceStrategy boundaryStrategy =
        segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted;
    segment_puzzler::AgglomerationExecutionPolicy executionPolicy =
        segment_puzzler::AgglomerationExecutionPolicy::Auto;
    // Size-bias options
    segment_puzzler::SizeBiasStrategy sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::Off;
    uint64_t sizeBiasThreshold = 5000;
    double sizeBiasStrength = 0.3;
    double sizeBiasProtection = 0.3;
    bool sizeBiasRespectMask = true;
};

struct CsvRow {
    int repetition = 0;
    int threads = 1;
    std::size_t fragments = 0;
    std::size_t edges = 0;
    std::size_t merges = 0;
    std::size_t outputClusters = 0;
    std::size_t batchCount = 0;
    std::size_t maxBatchPairs = 0;
    double compactMs = 0.0;
    double ragMs = 0.0;
    double heapMs = 0.0;
    double agglomerationMs = 0.0;
    double batchSelectionMs = 0.0;
    double batchReduceMs = 0.0;
    double batchApplyMs = 0.0;
    double projectionMs = 0.0;
    double elapsedMs = 0.0;
    std::string policy;
};

std::array<int, 3> parseDims(const std::string &value) {
    std::array<int, 3> dims{};
    char sepA = 0;
    char sepB = 0;
    std::istringstream stream(value);
    stream >> dims[0] >> sepA >> dims[1] >> sepB >> dims[2];
    if (!stream || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 ||
        (sepA != 'x' && sepA != 'X') || (sepB != 'x' && sepB != 'X')) {
        throw std::runtime_error("Invalid size, expected XxYxZ.");
    }
    return dims;
}

std::vector<int> parseThreadList(const std::string &value) {
    std::vector<int> threadCounts;
    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) {
            continue;
        }
        threadCounts.push_back(std::max(1, std::stoi(token)));
    }
    if (threadCounts.empty()) {
        throw std::runtime_error("Empty thread list.");
    }
    return threadCounts;
}

void printUsage() {
    std::cout << "benchmark_watershed_agglomertion\n"
              << "  --labels PATH\n"
              << "  --boundary PATH\n"
              << "  --threshold-mask PATH\n"
              << "  --generator {unique-grid}\n"
              << "  --boundary-generator {constant-zero,checkerboard}\n"
              << "  --threshold-mask-generator {all-open,wide-slab}\n"
              << "  --size XxYxZ\n"
              << "  --threads list\n"
              << "  --boundary-strategy {raw-mean,open-mean,open-fraction-weighted}\n"
              << "  --execution {auto,serial,omp-batched}\n"
              << "  --repetitions N\n"
              << "  --warmup N\n"
              << "  --profile-min-seconds S\n"
              << "  --perfetto-output PATH\n"
              << "  --perfetto-buffer-kb N\n"
              << "  --csv PATH\n";
}

Options parseArguments(int argc, char **argv) {
    Options options;
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
        } else if (argument == "--labels") {
            options.labelsPath = requireValue("--labels");
        } else if (argument == "--boundary") {
            options.boundaryPath = requireValue("--boundary");
        } else if (argument == "--threshold-mask") {
            options.thresholdMaskPath = requireValue("--threshold-mask");
        } else if (argument == "--generator") {
            options.generator = requireValue("--generator");
        } else if (argument == "--boundary-generator") {
            options.boundaryGenerator = requireValue("--boundary-generator");
        } else if (argument == "--threshold-mask-generator") {
            options.thresholdMaskGenerator = requireValue("--threshold-mask-generator");
        } else if (argument == "--size") {
            options.size = parseDims(requireValue("--size"));
        } else if (argument == "--threads") {
            options.threadCounts = parseThreadList(requireValue("--threads"));
        } else if (argument == "--boundary-strategy") {
            const std::string value = requireValue("--boundary-strategy");
            if (value == "raw-mean") {
                options.boundaryStrategy = segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean;
            } else if (value == "open-mean") {
                options.boundaryStrategy = segment_puzzler::BoundaryEvidenceStrategy::OpenInterfaceMean;
            } else if (value == "open-fraction-weighted") {
                options.boundaryStrategy = segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted;
            } else {
                throw std::runtime_error("Unknown boundary strategy: " + value);
            }
        } else if (argument == "--execution") {
            const std::string value = requireValue("--execution");
            if (value == "auto") {
                options.executionPolicy = segment_puzzler::AgglomerationExecutionPolicy::Auto;
            } else if (value == "serial") {
                options.executionPolicy = segment_puzzler::AgglomerationExecutionPolicy::Serial;
            } else if (value == "omp-batched") {
                options.executionPolicy = segment_puzzler::AgglomerationExecutionPolicy::OmpBatched;
            } else {
                throw std::runtime_error("Unknown execution policy: " + value);
            }
        } else if (argument == "--repetitions") {
            options.repetitions = std::max(1, std::stoi(requireValue("--repetitions")));
        } else if (argument == "--warmup") {
            options.warmup = std::max(0, std::stoi(requireValue("--warmup")));
        } else if (argument == "--profile-min-seconds") {
            options.profileMinSeconds = std::max(0.0, std::stod(requireValue("--profile-min-seconds")));
        } else if (argument == "--perfetto-output") {
            options.perfettoOutputPath = requireValue("--perfetto-output");
        } else if (argument == "--perfetto-buffer-kb") {
            options.perfettoBufferKb = static_cast<std::size_t>(std::stoull(requireValue("--perfetto-buffer-kb")));
        } else if (argument == "--csv") {
            options.csvPath = requireValue("--csv");
        } else if (argument == "--size-bias-strategy") {
            const std::string value = requireValue("--size-bias-strategy");
            if (value == "off") {
                options.sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::Off;
            } else if (value == "soft-bias") {
                options.sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::SoftBias;
            } else if (value == "cleanup") {
                options.sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::Cleanup;
            } else if (value == "soft-bias-and-cleanup") {
                options.sizeBiasStrategy = segment_puzzler::SizeBiasStrategy::SoftBiasAndCleanup;
            } else {
                throw std::runtime_error("Unknown size-bias-strategy: " + value);
            }
        } else if (argument == "--size-bias-threshold") {
            options.sizeBiasThreshold = static_cast<uint64_t>(std::stoull(requireValue("--size-bias-threshold")));
        } else if (argument == "--size-bias-strength") {
            options.sizeBiasStrength = std::stod(requireValue("--size-bias-strength"));
        } else if (argument == "--size-bias-protection") {
            options.sizeBiasProtection = std::stod(requireValue("--size-bias-protection"));
        } else if (argument == "--size-bias-ignore-mask") {
            options.sizeBiasRespectMask = false;
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }
    if (options.boundaryStrategy != segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean &&
        options.labelsPath.has_value() && !options.thresholdMaskPath.has_value()) {
        throw std::runtime_error("--threshold-mask is required for threshold-aware boundary strategies on file inputs.");
    }
    return options;
}

SegmentsImageType::Pointer allocateSegmentsImage(const std::array<int, 3> &size) {
    auto image = SegmentsImageType::New();
    SegmentsImageType::IndexType start{};
    start.Fill(0);
    SegmentsImageType::SizeType itkSize{};
    itkSize[0] = size[0];
    itkSize[1] = size[1];
    itkSize[2] = size[2];
    image->SetRegions(SegmentsImageType::RegionType(start, itkSize));
    SegmentsImageType::SpacingType spacing;
    spacing[0] = 1.0;
    spacing[1] = 1.0;
    spacing[2] = 1.0;
    image->SetSpacing(spacing);
    image->Allocate();
    return image;
}

SegmentsImageType::Pointer loadSegmentsImage(const std::string &path) {
    using ReaderType = itk::ImageFileReader<SegmentsImageType>;
    auto reader = ReaderType::New();
    reader->SetFileName(path);
    reader->Update();
    return reader->GetOutput();
}

SegmentsImageType::Pointer makeUniqueGridLabels(const std::array<int, 3> &size) {
    auto image = allocateSegmentsImage(size);
    SegmentIdType nextLabel = 1;
    itk::ImageRegionIterator<SegmentsImageType> iterator(image, image->GetLargestPossibleRegion());
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        iterator.Set(nextLabel++);
    }
    return image;
}

segment_puzzler::BoundaryFloatImageType::Pointer makeBoundaryImage(const std::array<int, 3> &size,
                                                                   const std::string &generator) {
    auto image = segment_puzzler::BoundaryFloatImageType::New();
    segment_puzzler::BoundaryFloatImageType::IndexType start{};
    start.Fill(0);
    segment_puzzler::BoundaryFloatImageType::SizeType itkSize{};
    itkSize[0] = size[0];
    itkSize[1] = size[1];
    itkSize[2] = size[2];
    image->SetRegions(segment_puzzler::BoundaryFloatImageType::RegionType(start, itkSize));
    segment_puzzler::BoundaryFloatImageType::SpacingType spacing;
    spacing[0] = 1.0;
    spacing[1] = 1.0;
    spacing[2] = 1.0;
    image->SetSpacing(spacing);
    image->Allocate();

    itk::ImageRegionIterator<segment_puzzler::BoundaryFloatImageType> iterator(image, image->GetLargestPossibleRegion());
    std::size_t index = 0;
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator, ++index) {
        if (generator == "constant-zero") {
            iterator.Set(0.0f);
        } else if (generator == "checkerboard") {
            iterator.Set((index % 2) == 0 ? 0.0f : 0.25f);
        } else {
            throw std::runtime_error("Unknown boundary generator: " + generator);
        }
    }
    return image;
}

segment_puzzler::BoundaryMaskImageType::Pointer makeThresholdMaskImage(const std::array<int, 3> &size,
                                                                       const std::string &generator) {
    auto image = segment_puzzler::BoundaryMaskImageType::New();
    segment_puzzler::BoundaryMaskImageType::IndexType start{};
    start.Fill(0);
    segment_puzzler::BoundaryMaskImageType::SizeType itkSize{};
    itkSize[0] = size[0];
    itkSize[1] = size[1];
    itkSize[2] = size[2];
    image->SetRegions(segment_puzzler::BoundaryMaskImageType::RegionType(start, itkSize));
    segment_puzzler::BoundaryMaskImageType::SpacingType spacing;
    spacing[0] = 1.0;
    spacing[1] = 1.0;
    spacing[2] = 1.0;
    image->SetSpacing(spacing);
    image->Allocate();

    const int slabMin = std::max(0, size[0] / 3);
    const int slabMax = std::min(size[0], (2 * size[0]) / 3);
    itk::ImageRegionIterator<segment_puzzler::BoundaryMaskImageType> iterator(image, image->GetLargestPossibleRegion());
    for (iterator.GoToBegin(); !iterator.IsAtEnd(); ++iterator) {
        const auto index = iterator.GetIndex();
        if (generator == "all-open") {
            iterator.Set(0);
        } else if (generator == "wide-slab") {
            iterator.Set(index[0] >= slabMin && index[0] < slabMax ? 1 : 0);
        } else {
            throw std::runtime_error("Unknown threshold-mask generator: " + generator);
        }
    }
    return image;
}

std::tuple<SegmentsImageType::Pointer, segment_puzzler::BoundaryFloatImageType::Pointer, segment_puzzler::BoundaryMaskImageType::Pointer>
buildInputs(const Options &options) {
    if (options.labelsPath.has_value() != options.boundaryPath.has_value()) {
        throw std::runtime_error("--labels and --boundary must be provided together.");
    }
    if (options.labelsPath.has_value()) {
        auto thresholdMask = options.thresholdMaskPath.has_value()
            ? distance_map_benchmark::loadBinaryImage(*options.thresholdMaskPath)
            : segment_puzzler::BoundaryMaskImageType::Pointer{};
        return {loadSegmentsImage(*options.labelsPath),
                distance_map_benchmark::loadScalarImageAsFloat(*options.boundaryPath),
                thresholdMask};
    }
    if (options.generator != "unique-grid") {
        throw std::runtime_error("Only --generator unique-grid is supported.");
    }
    auto thresholdMask = options.boundaryStrategy == segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean
        ? segment_puzzler::BoundaryMaskImageType::Pointer{}
        : makeThresholdMaskImage(options.size, options.thresholdMaskGenerator);
    return {makeUniqueGridLabels(options.size),
            makeBoundaryImage(options.size, options.boundaryGenerator),
            thresholdMask};
}

CsvRow runOne(const Options &options,
              SegmentsImageType::Pointer labels,
              segment_puzzler::BoundaryFloatImageType::Pointer boundary,
              segment_puzzler::BoundaryMaskImageType::Pointer thresholdMask,
              int threadCount,
              int repetition) {
    TRACE_EVENT("agglomeration", "run_one", "threads", threadCount, "repetition", repetition);
    segment_puzzler::WatershedRagAgglomerationOptions agglomerationOptions;
    agglomerationOptions.linkage = segment_puzzler::RagLinkage::Average;
    agglomerationOptions.boundaryNormalization = segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne;
    agglomerationOptions.boundaryEvidenceStrategy = options.boundaryStrategy;
    agglomerationOptions.executionPolicy = options.executionPolicy;
    agglomerationOptions.threadCount = threadCount;
    agglomerationOptions.sizeBiasStrategy = options.sizeBiasStrategy;
    agglomerationOptions.sizeBiasThreshold = options.sizeBiasThreshold;
    agglomerationOptions.sizeBiasStrength = options.sizeBiasStrength;
    agglomerationOptions.sizeBiasProtection = options.sizeBiasProtection;
    agglomerationOptions.sizeBiasRespectMask = options.sizeBiasRespectMask;

    double start = wallTimeSeconds();
    auto result = segment_puzzler::runWatershedRagAgglomeration(labels, boundary, thresholdMask, agglomerationOptions);
    double end = wallTimeSeconds();

    CsvRow row;
    row.repetition = repetition;
    row.threads = threadCount;
    row.fragments = result.stats.inputFragmentCount;
    row.edges = result.stats.ragEdgeCount;
    row.merges = result.stats.mergeCount;
    row.outputClusters = result.stats.outputClusterCount;
    row.batchCount = result.stats.batchCount;
    row.maxBatchPairs = result.stats.maxBatchPairs;
    row.compactMs = result.stats.compactLabelsMs;
    row.ragMs = result.stats.ragBuildMs;
    row.heapMs = result.stats.heapInitMs;
    row.agglomerationMs = result.stats.agglomerationMs;
    row.batchSelectionMs = result.stats.batchSelectionMs;
    row.batchReduceMs = result.stats.batchReduceMs;
    row.batchApplyMs = result.stats.batchApplyMs;
    row.projectionMs = result.stats.projectionMs;
    row.elapsedMs = (end - start) * 1000.0;
    row.policy = segment_puzzler::agglomerationExecutionPolicyName(result.stats.executionPolicyUsed);
    return row;
}

void writeCsv(const std::string &path, const std::vector<CsvRow> &rows) {
    std::ofstream csv(path);
    csv << "repetition,threads,policy,fragments,edges,merges,output_clusters,batch_count,max_batch_pairs,"
           "compact_ms,rag_ms,heap_ms,agglomeration_ms,batch_selection_ms,batch_reduce_ms,batch_apply_ms,projection_ms,elapsed_ms\n";
    for (const CsvRow &row : rows) {
        csv << row.repetition << ','
            << row.threads << ','
            << row.policy << ','
            << row.fragments << ','
            << row.edges << ','
            << row.merges << ','
            << row.outputClusters << ','
            << row.batchCount << ','
            << row.maxBatchPairs << ','
            << std::fixed << std::setprecision(3)
            << row.compactMs << ','
            << row.ragMs << ','
            << row.heapMs << ','
            << row.agglomerationMs << ','
            << row.batchSelectionMs << ','
            << row.batchReduceMs << ','
            << row.batchApplyMs << ','
            << row.projectionMs << ','
            << row.elapsedMs << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseArguments(argc, argv);
        benchmark_tracing::PerfettoTraceSession::initializeOnce();
        auto perfettoSession = options.perfettoOutputPath.has_value()
            ? std::optional<benchmark_tracing::PerfettoTraceSession>(
                benchmark_tracing::PerfettoTraceSession::start(*options.perfettoOutputPath, options.perfettoBufferKb))
            : std::nullopt;
        auto inputs = buildInputs(options);
        distance_map_benchmark::printImageSummary<SegmentsImageType>("labels", std::get<0>(inputs));
        distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryFloatImageType>("boundary", std::get<1>(inputs));
        if (std::get<2>(inputs).IsNotNull()) {
            distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryMaskImageType>("threshold_mask", std::get<2>(inputs));
        }

        std::vector<CsvRow> rows;
        for (int threadCount : options.threadCounts) {
            for (int warmup = 0; warmup < options.warmup; ++warmup) {
                (void)runOne(options, std::get<0>(inputs), std::get<1>(inputs), std::get<2>(inputs), threadCount, 0);
            }
            for (int repetition = 1; repetition <= options.repetitions; ++repetition) {
                CsvRow row = runOne(options, std::get<0>(inputs), std::get<1>(inputs), std::get<2>(inputs), threadCount, repetition);
                rows.push_back(row);
                std::cout << "threads=" << row.threads
                          << " repetition=" << row.repetition
                          << " policy=" << row.policy
                          << " boundary_strategy=" << segment_puzzler::boundaryEvidenceStrategyName(options.boundaryStrategy)
                          << " fragments=" << row.fragments
                          << " edges=" << row.edges
                          << " merges=" << row.merges
                          << " agglomeration_ms=" << row.agglomerationMs
                          << " elapsed_ms=" << row.elapsedMs
                          << '\n';
            }
        }
        if (!options.csvPath.empty()) {
            writeCsv(options.csvPath, rows);
            std::cout << "Wrote csv to " << options.csvPath << '\n';
        }
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "benchmark_watershed_agglomertion failed: " << exception.what() << '\n';
        return 1;
    }
}
