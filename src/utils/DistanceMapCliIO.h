#ifndef SEGMENTPUZZLER_DISTANCEMAPCLIIO_H
#define SEGMENTPUZZLER_DISTANCEMAPCLIIO_H

#include "DistanceMapSeedExtractors.h"

#include <itkImageFileWriter.h>
#include <itkImageIOBase.h>
#include <itkImageIOFactory.h>
#include <itkImageRegionConstIterator.h>
#include <itkStatisticsImageFilter.h>

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace distance_map_benchmark {

DistanceImageType::Pointer loadScalarImageAsFloat(const std::string &path);
BinaryImageType::Pointer loadBinaryImage(const std::string &path);
SeedImageType::Pointer loadSeedImage(const std::string &path);
BinaryImageType::Pointer thresholdScalarImage(DistanceImageType::Pointer inputImage, float lowerThreshold);

template <typename TImage>
void writeImage(typename TImage::Pointer image, const std::string &path) {
    using WriterType = itk::ImageFileWriter<TImage>;
    auto writer = WriterType::New();
    writer->SetInput(image);
    writer->SetFileName(path);
    writer->Update();
}

template <typename TImage>
void printImageSummary(const std::string &label, typename TImage::Pointer image) {
    using StatisticsFilterType = itk::StatisticsImageFilter<TImage>;
    auto statisticsFilter = StatisticsFilterType::New();
    statisticsFilter->SetInput(image);
    statisticsFilter->Update();

    const auto region = image->GetLargestPossibleRegion();
    const auto size = region.GetSize();
    const auto spacing = image->GetSpacing();
    const auto minValue = static_cast<double>(statisticsFilter->GetMinimum());
    const auto maxValue = static_cast<double>(statisticsFilter->GetMaximum());
    std::cout << std::fixed << std::setprecision(3)
              << label
              << " size=" << size[0] << "x" << size[1] << "x" << size[2]
              << " spacing=" << spacing[0] << "," << spacing[1] << "," << spacing[2]
              << " min=" << minValue
              << " max=" << maxValue
              << '\n';
}

} // namespace distance_map_benchmark

#endif
