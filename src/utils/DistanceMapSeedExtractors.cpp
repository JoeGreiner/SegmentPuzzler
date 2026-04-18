#include "DistanceMapSeedExtractors.h"

#include <itkBinaryThresholdImageFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkHConvexImageFilter.h>
#include <itkImageRegionIterator.h>
#include <itkImageRegionIteratorWithIndex.h>

#include <stdexcept>

namespace distance_map_benchmark {

SeedExtractorKind parseSeedExtractor(const std::string &value) {
    if (value == "local-maxima") {
        return SeedExtractorKind::LocalMaxima;
    }
    if (value == "h-convex") {
        return SeedExtractorKind::HConvex;
    }
    if (value == "all") {
        return SeedExtractorKind::All;
    }
    throw std::runtime_error("Unknown seed extractor: " + value);
}

std::string seedExtractorName(SeedExtractorKind kind) {
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

SeedImageType::Pointer extractHConvexSeedsFromDistanceImage(DistanceImageType::Pointer distanceMap,
                                                            double minimalHeight) {
    using HConvexImageFilterType = itk::HConvexImageFilter<DistanceImageType, DistanceImageType>;
    using ThresholdFilterType = itk::BinaryThresholdImageFilter<DistanceImageType, BinaryImageType>;
    using ConnectedComponentFilterType = itk::ConnectedComponentImageFilter<BinaryImageType, SeedImageType>;

    auto hConvexImageFilter = HConvexImageFilterType::New();
    hConvexImageFilter->SetInput(distanceMap);
    hConvexImageFilter->SetHeight(minimalHeight);
    hConvexImageFilter->SetFullyConnected(true);

    auto thresholdFilter = ThresholdFilterType::New();
    thresholdFilter->SetInput(hConvexImageFilter->GetOutput());
    thresholdFilter->SetLowerThreshold(0.1f);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);

    auto connectedComponentFilter = ConnectedComponentFilterType::New();
    connectedComponentFilter->SetInput(thresholdFilter->GetOutput());
    connectedComponentFilter->Update();
    return connectedComponentFilter->GetOutput();
}

SeedImageType::Pointer extractLocalMaximaSeedsFromDistanceImage(DistanceImageType::Pointer distanceMap) {
    using ConnectedComponentFilterType = itk::ConnectedComponentImageFilter<BinaryImageType, SeedImageType>;

    auto maximaMask = BinaryImageType::New();
    maximaMask->SetRegions(distanceMap->GetLargestPossibleRegion());
    maximaMask->SetSpacing(distanceMap->GetSpacing());
    maximaMask->SetOrigin(distanceMap->GetOrigin());
    maximaMask->SetDirection(distanceMap->GetDirection());
    maximaMask->Allocate();
    maximaMask->FillBuffer(0);

    const auto region = distanceMap->GetLargestPossibleRegion();
    const auto size = region.GetSize();
    itk::ImageRegionConstIteratorWithIndex<DistanceImageType> distanceIt(distanceMap, region);
    itk::ImageRegionIterator<BinaryImageType> maximaIt(maximaMask, region);
    for (distanceIt.GoToBegin(), maximaIt.GoToBegin(); !distanceIt.IsAtEnd(); ++distanceIt, ++maximaIt) {
        const DistanceVoxelType centerValue = distanceIt.Get();
        if (centerValue <= 0.0f) {
            maximaIt.Set(0);
            continue;
        }

        const auto index = distanceIt.GetIndex();
        bool hasGreaterNeighbor = false;
        for (int dz = -1; dz <= 1 && !hasGreaterNeighbor; ++dz) {
            for (int dy = -1; dy <= 1 && !hasGreaterNeighbor; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;
                    }
                    const long nx = static_cast<long>(index[0]) + dx;
                    const long ny = static_cast<long>(index[1]) + dy;
                    const long nz = static_cast<long>(index[2]) + dz;
                    if (nx < 0 || ny < 0 || nz < 0 ||
                        nx >= static_cast<long>(size[0]) ||
                        ny >= static_cast<long>(size[1]) ||
                        nz >= static_cast<long>(size[2])) {
                        continue;
                    }

                    DistanceImageType::IndexType neighborIndex{};
                    neighborIndex[0] = static_cast<DistanceImageType::IndexType::IndexValueType>(nx);
                    neighborIndex[1] = static_cast<DistanceImageType::IndexType::IndexValueType>(ny);
                    neighborIndex[2] = static_cast<DistanceImageType::IndexType::IndexValueType>(nz);
                    if (distanceMap->GetPixel(neighborIndex) > centerValue) {
                        hasGreaterNeighbor = true;
                        break;
                    }
                }
            }
        }

        maximaIt.Set(hasGreaterNeighbor ? 0 : 1);
    }

    auto connectedComponentFilter = ConnectedComponentFilterType::New();
    connectedComponentFilter->SetInput(maximaMask);
    connectedComponentFilter->Update();
    return connectedComponentFilter->GetOutput();
}

SeedImageType::Pointer extractSeedsFromDistanceImage(DistanceImageType::Pointer distanceMap,
                                                     SeedExtractorKind extractorKind,
                                                     double minimalHeight) {
    switch (extractorKind) {
        case SeedExtractorKind::LocalMaxima:
            return extractLocalMaximaSeedsFromDistanceImage(distanceMap);
        case SeedExtractorKind::HConvex:
            return extractHConvexSeedsFromDistanceImage(distanceMap, minimalHeight);
        case SeedExtractorKind::All:
            break;
    }
    throw std::runtime_error("Seed extractor 'all' is not valid for a single extractor run.");
}

} // namespace distance_map_benchmark
