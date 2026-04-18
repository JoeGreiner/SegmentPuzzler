#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapCliIO.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string distanceMapPath;
    std::string seedsPath;
    std::string outputPath;
    std::string thresholdedBoundariesPath;
    int filterSmallSeeds = 0;
    bool showWatershedLines = false;
    bool fullyConnected = false;
    WatershedAlgorithm algorithm = WatershedAlgorithm::MorphologicalWatershedFromMarkers;
};

void printUsage() {
    std::cout << "cli_watershed_from_seeds\n"
              << "  --distance-map PATH\n"
              << "  --seeds PATH\n"
              << "  --output PATH\n"
              << "  --thresholded-boundaries PATH\n"
              << "  --filter-small-seeds VALUE\n"
              << "  --show-watershed-lines {0,1}\n"
              << "  --fully-connected {0,1}\n"
              << "  --algorithm {itk,fast-marker}\n";
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
        } else if (argument == "--distance-map") {
            options.distanceMapPath = requireValue("--distance-map");
        } else if (argument == "--seeds") {
            options.seedsPath = requireValue("--seeds");
        } else if (argument == "--output") {
            options.outputPath = requireValue("--output");
        } else if (argument == "--thresholded-boundaries") {
            options.thresholdedBoundariesPath = requireValue("--thresholded-boundaries");
        } else if (argument == "--filter-small-seeds") {
            options.filterSmallSeeds = std::stoi(requireValue("--filter-small-seeds"));
        } else if (argument == "--show-watershed-lines") {
            options.showWatershedLines = std::stoi(requireValue("--show-watershed-lines")) != 0;
        } else if (argument == "--fully-connected") {
            options.fullyConnected = std::stoi(requireValue("--fully-connected")) != 0;
        } else if (argument == "--algorithm") {
            const std::string value = requireValue("--algorithm");
            if (value == "itk") {
                options.algorithm = WatershedAlgorithm::MorphologicalWatershedFromMarkers;
            } else if (value == "fast-marker") {
                options.algorithm = WatershedAlgorithm::FastMarkerWatershed;
            } else {
                throw std::runtime_error("Unknown watershed algorithm: " + value);
            }
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }

    if (options.distanceMapPath.empty() || options.seedsPath.empty() || options.outputPath.empty()) {
        throw std::runtime_error("--distance-map, --seeds, and --output are required.");
    }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseArguments(argc, argv);
        auto distanceMap = distance_map_benchmark::loadScalarImageAsFloat(options.distanceMapPath);
        auto seeds = distance_map_benchmark::loadSeedImage(options.seedsPath);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::DistanceImageType>("distance_map", distanceMap);
        distance_map_benchmark::printImageSummary<distance_map_benchmark::SeedImageType>("seeds", seeds);

        distance_map_benchmark::DistanceImageType::Pointer invertedDistanceMap;
        invertDistanceMap(distanceMap, invertedDistanceMap);
        WatershedRunOptions watershedOptions;
        watershedOptions.algorithm = options.algorithm;
        watershedOptions.showWatershedLines = options.showWatershedLines;
        watershedOptions.fullyConnected = options.fullyConnected;

        distance_map_benchmark::SeedImageType::Pointer watershedImage;
        runWatershed(invertedDistanceMap, seeds, watershedImage, watershedOptions);

        if (options.filterSmallSeeds > 0) {
            distance_map_benchmark::SeedImageType::Pointer filteredSeeds;
            filterSmallSegmentSeeds(watershedImage, filteredSeeds, static_cast<float>(options.filterSmallSeeds));
            seeds = filteredSeeds;
            runWatershed(invertedDistanceMap, seeds, watershedImage, watershedOptions);
        }

        if (!options.thresholdedBoundariesPath.empty()) {
            auto thresholdedBoundaries = distance_map_benchmark::loadBinaryImage(options.thresholdedBoundariesPath);
            insertBoundariesIntoWatershed(watershedImage, thresholdedBoundaries);
        }

        distance_map_benchmark::printImageSummary<distance_map_benchmark::SeedImageType>("watershed", watershedImage);
        distance_map_benchmark::writeImage<distance_map_benchmark::SeedImageType>(watershedImage, options.outputPath);
        std::cout << "Wrote watershed labels to " << options.outputPath << '\n';
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "cli_watershed_from_seeds failed: " << exception.what() << '\n';
        return 1;
    }
}
