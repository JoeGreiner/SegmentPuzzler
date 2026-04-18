#include "src/utils/DistanceMapCliIO.h"
#include "src/utils/DistanceMapSeedExtractors.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string inputPath;
    std::string outputPath;
    distance_map_benchmark::SeedExtractorKind seedExtractor = distance_map_benchmark::SeedExtractorKind::LocalMaxima;
    double hConvexHeight = 1.0;
};

void printUsage() {
    std::cout << "cli_extract_seeds\n"
              << "  --input PATH\n"
              << "  --output PATH\n"
              << "  --seed-extractor {local-maxima,h-convex}\n"
              << "  --h-convex-height VALUE\n";
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
        } else if (argument == "--seed-extractor") {
            options.seedExtractor = distance_map_benchmark::parseSeedExtractor(requireValue("--seed-extractor"));
            if (options.seedExtractor == distance_map_benchmark::SeedExtractorKind::All) {
                throw std::runtime_error("--seed-extractor all is not supported for the step CLI.");
            }
        } else if (argument == "--h-convex-height") {
            options.hConvexHeight = std::stod(requireValue("--h-convex-height"));
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
        auto distanceMap = distance_map_benchmark::loadScalarImageAsFloat(options.inputPath);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::DistanceImageType>("input", distanceMap);

        auto seeds = distance_map_benchmark::extractSeedsFromDistanceImage(distanceMap,
                                                                           options.seedExtractor,
                                                                           options.hConvexHeight);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::SeedImageType>("seeds", seeds);
        distance_map_benchmark::writeImage<distance_map_benchmark::SeedImageType>(seeds, options.outputPath);
        std::cout << "Wrote seeds to " << options.outputPath << '\n';
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "cli_extract_seeds failed: " << exception.what() << '\n';
        return 1;
    }
}
