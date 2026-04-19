#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapCliIO.h"
#include "src/utils/WatershedRagAgglomeration.h"

#include <itkCastImageFilter.h>
#include <itkImageFileReader.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using SegmentsImageType = dataType::SegmentsImageType;

struct Options {
    std::string labelsPath;
    std::string boundaryPath;
    std::string outputPath;
    std::string thresholdMaskPath;
    std::string thresholdedBoundariesPath;
    segment_puzzler::RagLinkage linkage = segment_puzzler::RagLinkage::Average;
    segment_puzzler::BoundaryNormalizationMode boundaryMode =
        segment_puzzler::BoundaryNormalizationMode::AutoDetect;
    segment_puzzler::BoundaryEvidenceStrategy boundaryStrategy =
        segment_puzzler::BoundaryEvidenceStrategy::OpenFractionWeighted;
    segment_puzzler::AgglomerationExecutionPolicy executionPolicy =
        segment_puzzler::AgglomerationExecutionPolicy::Auto;
    segment_puzzler::SizeBiasStrategy sizeBiasStrategy =
        segment_puzzler::SizeBiasStrategy::Off;
    uint64_t sizeBiasThreshold = 5000;
    double sizeBiasStrength = 0.3;
    double sizeBiasProtection = 0.3;
    bool sizeBiasRespectMask = true;
    int threadCount = 0;
};

void printUsage() {
    std::cout << "cli_watershed_agglomertion\n"
              << "  --labels PATH\n"
              << "  --boundary PATH\n"
              << "  --output PATH\n"
              << "  --threshold-mask PATH\n"
              << "  --thresholded-boundaries PATH\n"
              << "  --linkage {average,sum}\n"
              << "  --boundary-strategy {raw-mean,open-mean,open-fraction-weighted}\n"
              << "  --boundary-scale {auto-detect,probability-0-1,probability-0-2,uint8-full-range,uint16-full-range}\n"
              << "  --size-bias-strategy {off,soft-bias,cleanup,soft-bias-and-cleanup}\n"
              << "  --size-bias-threshold N\n"
              << "  --size-bias-strength VALUE\n"
              << "  --size-bias-protection VALUE\n"
              << "  --size-bias-ignore-mask\n"
              << "  --execution {auto,serial,omp-batched}\n"
              << "  --threads N\n";
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
        } else if (argument == "--output") {
            options.outputPath = requireValue("--output");
        } else if (argument == "--threshold-mask") {
            options.thresholdMaskPath = requireValue("--threshold-mask");
        } else if (argument == "--thresholded-boundaries") {
            options.thresholdedBoundariesPath = requireValue("--thresholded-boundaries");
        } else if (argument == "--linkage") {
            const std::string value = requireValue("--linkage");
            if (value == "average") {
                options.linkage = segment_puzzler::RagLinkage::Average;
            } else if (value == "sum") {
                options.linkage = segment_puzzler::RagLinkage::Sum;
            } else {
                throw std::runtime_error("Unknown linkage: " + value);
            }
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
        } else if (argument == "--boundary-scale") {
            const std::string value = requireValue("--boundary-scale");
            if (value == "auto-detect") {
                options.boundaryMode = segment_puzzler::BoundaryNormalizationMode::AutoDetect;
            } else if (value == "probability-0-1") {
                options.boundaryMode = segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToOne;
            } else if (value == "probability-0-2") {
                options.boundaryMode = segment_puzzler::BoundaryNormalizationMode::ProbabilityZeroToTwo;
            } else if (value == "uint8-full-range") {
                options.boundaryMode = segment_puzzler::BoundaryNormalizationMode::UInt8FullRange;
            } else if (value == "uint16-full-range") {
                options.boundaryMode = segment_puzzler::BoundaryNormalizationMode::UInt16FullRange;
            } else {
                throw std::runtime_error("Unknown boundary scale: " + value);
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
        } else if (argument == "--threads") {
            options.threadCount = std::max(0, std::stoi(requireValue("--threads")));
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }

    if (options.labelsPath.empty() || options.boundaryPath.empty()) {
        throw std::runtime_error("--labels and --boundary are required.");
    }
    if (options.boundaryStrategy != segment_puzzler::BoundaryEvidenceStrategy::RawInterfaceMean &&
        options.thresholdMaskPath.empty()) {
        throw std::runtime_error("--threshold-mask is required for threshold-aware boundary strategies.");
    }
    return options;
}

SegmentsImageType::Pointer loadSegmentsImage(const std::string &path) {
    using ReaderType = itk::ImageFileReader<SegmentsImageType>;
    auto reader = ReaderType::New();
    reader->SetFileName(path);
    reader->Update();
    return reader->GetOutput();
}

struct SmallSegmentStats {
    std::size_t nonZeroLabelCount = 0;
    std::size_t smallLabelCount = 0;
    std::size_t nonIsolatedSmallLabelCount = 0;
    uint64_t smallestLabelSize = 0;
    uint64_t largestSmallLabelSize = 0;
};

SmallSegmentStats computeSmallSegmentStats(SegmentsImageType::Pointer image, uint64_t threshold) {
    SmallSegmentStats stats;
    if (image.IsNull()) {
        return stats;
    }

    std::unordered_map<dataType::SegmentIdType, uint64_t> countsByLabel;
    std::unordered_map<dataType::SegmentIdType, bool> touchesOtherNonZeroLabel;
    const auto size = image->GetLargestPossibleRegion().GetSize();
    const size_t dimX = size[0];
    const size_t dimY = size[1];
    const size_t dimZ = size[2];
    const size_t planeXY = dimX * dimY;
    const auto voxelCount = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *buffer = image->GetBufferPointer();
    countsByLabel.reserve(4096);
    touchesOtherNonZeroLabel.reserve(4096);
    for (std::size_t index = 0; index < voxelCount; ++index) {
        const dataType::SegmentIdType label = buffer[index];
        if (label == 0) {
            continue;
        }
        ++countsByLabel[label];
    }

    for (size_t z = 0; z < dimZ; ++z) {
        for (size_t y = 0; y < dimY; ++y) {
            for (size_t x = 0; x < dimX; ++x) {
                const size_t index = x + y * dimX + z * planeXY;
                const dataType::SegmentIdType label = buffer[index];
                if (label == 0) {
                    continue;
                }

                const auto markTouch = [&](size_t neighborIndex) {
                    const dataType::SegmentIdType neighborLabel = buffer[neighborIndex];
                    if (neighborLabel == 0 || neighborLabel == label) {
                        return;
                    }
                    touchesOtherNonZeroLabel[label] = true;
                    touchesOtherNonZeroLabel[neighborLabel] = true;
                };

                if (x + 1 < dimX) {
                    markTouch(index + 1);
                }
                if (y + 1 < dimY) {
                    markTouch(index + dimX);
                }
                if (z + 1 < dimZ) {
                    markTouch(index + planeXY);
                }
            }
        }
    }

    stats.nonZeroLabelCount = countsByLabel.size();
    uint64_t smallestSeen = std::numeric_limits<uint64_t>::max();
    for (const auto &entry : countsByLabel) {
        smallestSeen = std::min(smallestSeen, entry.second);
        if (entry.second < threshold) {
            ++stats.smallLabelCount;
            stats.largestSmallLabelSize = std::max(stats.largestSmallLabelSize, entry.second);
            if (touchesOtherNonZeroLabel[entry.first]) {
                ++stats.nonIsolatedSmallLabelCount;
            }
        }
    }
    stats.smallestLabelSize = countsByLabel.empty() ? 0 : smallestSeen;
    return stats;
}

segment_puzzler::BoundaryMaskImageType::Pointer loadDisplayMask(const Options &options,
                                                                segment_puzzler::BoundaryMaskImageType::Pointer thresholdMask) {
    if (!options.thresholdedBoundariesPath.empty()) {
        return distance_map_benchmark::loadBinaryImage(options.thresholdedBoundariesPath);
    }
    return thresholdMask;
}

SegmentsImageType::Pointer deriveDisplayView(SegmentsImageType::Pointer labels,
                                             segment_puzzler::BoundaryMaskImageType::Pointer displayMask) {
    if (labels.IsNull() || displayMask.IsNull()) {
        return labels;
    }
    auto partition = deriveBoundaryConsistentPartition(labels, displayMask, WatershedRunOptions{}, false);
    return partition.displayLabels;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseArguments(argc, argv);
        auto labels = loadSegmentsImage(options.labelsPath);
        auto boundary = distance_map_benchmark::loadScalarImageAsFloat(options.boundaryPath);
        auto thresholdMask = options.thresholdMaskPath.empty()
            ? segment_puzzler::BoundaryMaskImageType::Pointer{}
            : distance_map_benchmark::loadBinaryImage(options.thresholdMaskPath);
        auto displayMask = loadDisplayMask(options, thresholdMask);

        distance_map_benchmark::printImageSummary<SegmentsImageType>("labels", labels);
        distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryFloatImageType>("boundary", boundary);
        if (thresholdMask.IsNotNull()) {
            distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryMaskImageType>("threshold_mask", thresholdMask);
        }
        if (displayMask.IsNotNull() && displayMask != thresholdMask) {
            distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryMaskImageType>("display_mask", displayMask);
        }

        segment_puzzler::WatershedRagAgglomerationOptions agglomerationOptions;
        agglomerationOptions.linkage = options.linkage;
        agglomerationOptions.boundaryNormalization = options.boundaryMode;
        agglomerationOptions.boundaryEvidenceStrategy = options.boundaryStrategy;
        agglomerationOptions.executionPolicy = options.executionPolicy;
        agglomerationOptions.threadCount = options.threadCount;
        agglomerationOptions.sizeBiasStrategy = options.sizeBiasStrategy;
        agglomerationOptions.sizeBiasThreshold = options.sizeBiasThreshold;
        agglomerationOptions.sizeBiasStrength = options.sizeBiasStrength;
        agglomerationOptions.sizeBiasProtection = options.sizeBiasProtection;
        agglomerationOptions.sizeBiasRespectMask = options.sizeBiasRespectMask;

        auto result = segment_puzzler::runWatershedRagAgglomeration(labels, boundary, thresholdMask, agglomerationOptions);
        auto output = result.agglomeratedLabels;
        auto displayInput = deriveDisplayView(labels, displayMask);
        auto displayOutput = deriveDisplayView(output, displayMask);

        const auto initialSmallSegmentStats = computeSmallSegmentStats(displayInput, options.sizeBiasThreshold);
        const auto finalSmallSegmentStats = computeSmallSegmentStats(displayOutput, options.sizeBiasThreshold);

        distance_map_benchmark::printImageSummary<SegmentsImageType>("agglomertion", displayOutput);
        std::cout << "linkage=" << segment_puzzler::ragLinkageName(options.linkage)
                  << " boundary_strategy=" << segment_puzzler::boundaryEvidenceStrategyName(options.boundaryStrategy)
                  << " boundary_scale=" << segment_puzzler::boundaryNormalizationModeName(options.boundaryMode)
                  << " size_bias_strategy=" << segment_puzzler::sizeBiasStrategyName(options.sizeBiasStrategy)
                  << " size_bias_threshold=" << options.sizeBiasThreshold
                  << " size_bias_strength=" << options.sizeBiasStrength
                  << " size_bias_protection=" << options.sizeBiasProtection
                  << " size_bias_respect_mask=" << (options.sizeBiasRespectMask ? "true" : "false")
                  << " execution=" << segment_puzzler::agglomerationExecutionPolicyName(options.executionPolicy)
                  << " threads=" << options.threadCount
                  << '\n';
        std::cout << "initial_non_zero_labels=" << initialSmallSegmentStats.nonZeroLabelCount
                  << " initial_small_labels_below_threshold=" << initialSmallSegmentStats.smallLabelCount
                  << " final_non_zero_labels=" << finalSmallSegmentStats.nonZeroLabelCount
                  << " final_small_labels_below_threshold=" << finalSmallSegmentStats.smallLabelCount
                  << " reduced_small_labels="
                  << static_cast<long long>(initialSmallSegmentStats.smallLabelCount) -
                         static_cast<long long>(finalSmallSegmentStats.smallLabelCount)
                  << " remaining_non_isolated_small_labels=" << finalSmallSegmentStats.nonIsolatedSmallLabelCount
                  << " smallest_label_size=" << finalSmallSegmentStats.smallestLabelSize
                  << " largest_small_label_size=" << finalSmallSegmentStats.largestSmallLabelSize
                  << " core_initial_small_clusters=" << result.stats.initialSmallClusterCount
                  << " core_final_small_clusters=" << result.stats.finalSmallClusterCount
                  << " core_cleanup_merges=" << result.stats.sizeBiasCleanupMergeCount
                  << " core_final_cleanup_eligible_small_clusters=" << result.stats.finalCleanupEligibleSmallClusterCount
                  << '\n';
        if (!options.outputPath.empty()) {
            distance_map_benchmark::writeImage<SegmentsImageType>(displayOutput, options.outputPath);
            std::cout << "Wrote agglomertion labels to " << options.outputPath << '\n';
        }
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "cli_watershed_agglomertion failed: " << exception.what() << '\n';
        return 1;
    }
}
