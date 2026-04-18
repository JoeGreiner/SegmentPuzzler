#include "src/utils/DistanceMapCliIO.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string inputPath;
    std::string outputPath;
    float lowerThreshold = 0.5f;
};

void printUsage() {
    std::cout << "cli_threshold_boundaries\n"
              << "  --input PATH\n"
              << "  --output PATH\n"
              << "  --lower-threshold VALUE\n";
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
        } else if (argument == "--input") {
            options.inputPath = requireValue("--input");
        } else if (argument == "--output") {
            options.outputPath = requireValue("--output");
        } else if (argument == "--lower-threshold") {
            options.lowerThreshold = std::stof(requireValue("--lower-threshold"));
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }

    if (options.inputPath.empty() || options.outputPath.empty()) {
        throw std::runtime_error("Both --input and --output are required.");
    }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseArguments(argc, argv);
        auto inputImage = distance_map_benchmark::loadScalarImageAsFloat(options.inputPath);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::DistanceImageType>("input", inputImage);

        auto thresholdedImage = distance_map_benchmark::thresholdScalarImage(inputImage, options.lowerThreshold);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::BinaryImageType>("thresholded", thresholdedImage);
        distance_map_benchmark::writeImage<distance_map_benchmark::BinaryImageType>(thresholdedImage, options.outputPath);
        std::cout << "Wrote thresholded image to " << options.outputPath << '\n';
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "cli_threshold_boundaries failed: " << exception.what() << '\n';
        return 1;
    }
}
