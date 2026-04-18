#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapCliIO.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string inputPath;
    std::string outputPath;
    double gaussianVariance = 0.0;
};

void printUsage() {
    std::cout << "cli_distance_map\n"
              << "  --input PATH\n"
              << "  --output PATH\n"
              << "  --gaussian-variance VALUE\n";
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
        } else if (argument == "--gaussian-variance") {
            options.gaussianVariance = std::stod(requireValue("--gaussian-variance"));
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
        auto boundaryImage = distance_map_benchmark::loadBinaryImage(options.inputPath);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::BinaryImageType>("input", boundaryImage);

        distance_map_benchmark::DistanceImageType::Pointer distanceMap;
        generateDistanceMap(boundaryImage, distanceMap, options.gaussianVariance);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::DistanceImageType>("distance_map", distanceMap);
        distance_map_benchmark::writeImage<distance_map_benchmark::DistanceImageType>(distanceMap, options.outputPath);
        std::cout << "Wrote distance map to " << options.outputPath << '\n';
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "cli_distance_map failed: " << exception.what() << '\n';
        return 1;
    }
}
