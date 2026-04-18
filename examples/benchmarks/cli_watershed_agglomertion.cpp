#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapCliIO.h"
#include "src/utils/WatershedRagAgglomeration.h"

#include <itkCastImageFilter.h>
#include <itkImageFileReader.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseArguments(argc, argv);
        auto labels = loadSegmentsImage(options.labelsPath);
        auto boundary = distance_map_benchmark::loadScalarImageAsFloat(options.boundaryPath);
        auto thresholdMask = options.thresholdMaskPath.empty()
            ? segment_puzzler::BoundaryMaskImageType::Pointer{}
            : distance_map_benchmark::loadBinaryImage(options.thresholdMaskPath);

        distance_map_benchmark::printImageSummary<SegmentsImageType>("labels", labels);
        distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryFloatImageType>("boundary", boundary);
        if (thresholdMask.IsNotNull()) {
            distance_map_benchmark::printImageSummary<segment_puzzler::BoundaryMaskImageType>("threshold_mask", thresholdMask);
        }

        segment_puzzler::WatershedRagAgglomerationOptions agglomerationOptions;
        agglomerationOptions.linkage = options.linkage;
        agglomerationOptions.boundaryNormalization = options.boundaryMode;
        agglomerationOptions.boundaryEvidenceStrategy = options.boundaryStrategy;
        agglomerationOptions.executionPolicy = options.executionPolicy;
        agglomerationOptions.threadCount = options.threadCount;

        auto result = segment_puzzler::runWatershedRagAgglomeration(labels, boundary, thresholdMask, agglomerationOptions);
        auto output = result.agglomeratedLabels;

        if (!options.thresholdedBoundariesPath.empty()) {
            auto thresholdedBoundaries = distance_map_benchmark::loadBinaryImage(options.thresholdedBoundariesPath);
            insertBoundariesIntoWatershed(output, thresholdedBoundaries);
        }

        distance_map_benchmark::printImageSummary<SegmentsImageType>("agglomertion", output);
        std::cout << "linkage=" << segment_puzzler::ragLinkageName(options.linkage)
                  << " boundary_strategy=" << segment_puzzler::boundaryEvidenceStrategyName(options.boundaryStrategy)
                  << " boundary_scale=" << segment_puzzler::boundaryNormalizationModeName(options.boundaryMode)
                  << " execution=" << segment_puzzler::agglomerationExecutionPolicyName(options.executionPolicy)
                  << " threads=" << options.threadCount
                  << '\n';
        if (!options.outputPath.empty()) {
            distance_map_benchmark::writeImage<SegmentsImageType>(output, options.outputPath);
            std::cout << "Wrote agglomertion labels to " << options.outputPath << '\n';
        }
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "cli_watershed_agglomertion failed: " << exception.what() << '\n';
        return 1;
    }
}
