#include "DistanceMapCliIO.h"

#include <itkBinaryThresholdImageFilter.h>
#include <itkCastImageFilter.h>
#include <itkImageFileReader.h>

namespace distance_map_benchmark {
namespace {

template <typename TImage>
typename TImage::Pointer readImage(const std::string &path) {
    using ReaderType = itk::ImageFileReader<TImage>;
    auto reader = ReaderType::New();
    reader->SetFileName(path);
    reader->Update();
    return reader->GetOutput();
}

template <typename TInputImage, typename TOutputImage>
typename TOutputImage::Pointer readAndCastImage(const std::string &path) {
    using ReaderType = itk::ImageFileReader<TInputImage>;
    using CastType = itk::CastImageFilter<TInputImage, TOutputImage>;
    auto reader = ReaderType::New();
    reader->SetFileName(path);
    auto castFilter = CastType::New();
    castFilter->SetInput(reader->GetOutput());
    castFilter->Update();
    return castFilter->GetOutput();
}

template <typename TImage>
typename TImage::Pointer tryReadImage(const std::string &path) {
    try {
        return readImage<TImage>(path);
    } catch (const itk::ExceptionObject &) {
        return nullptr;
    }
}

template <typename TInputImage, typename TOutputImage>
typename TOutputImage::Pointer tryReadAndCastImage(const std::string &path) {
    try {
        return readAndCastImage<TInputImage, TOutputImage>(path);
    } catch (const itk::ExceptionObject &) {
        return nullptr;
    }
}

} // namespace

DistanceImageType::Pointer loadScalarImageAsFloat(const std::string &path) {
    if (auto image = tryReadImage<DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned char, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<char, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned short, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<short, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned int, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<int, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned long, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<long, 3>, DistanceImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<double, 3>, DistanceImageType>(path)) {
        return image;
    }
    throw std::runtime_error("Unsupported scalar input type for: " + path);
}

BinaryImageType::Pointer loadBinaryImage(const std::string &path) {
    if (auto image = tryReadImage<BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<char, 3>, BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned short, 3>, BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<short, 3>, BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned int, 3>, BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<int, 3>, BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<DistanceImageType, BinaryImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<double, 3>, BinaryImageType>(path)) {
        return image;
    }
    throw std::runtime_error("Unsupported binary image type for: " + path);
}

SeedImageType::Pointer loadSeedImage(const std::string &path) {
    if (auto image = tryReadImage<SeedImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned short, 3>, SeedImageType>(path)) {
        return image;
    }
    if (auto image = tryReadAndCastImage<itk::Image<unsigned long, 3>, SeedImageType>(path)) {
        return image;
    }
    throw std::runtime_error("Unsupported seed image type for: " + path);
}

BinaryImageType::Pointer thresholdScalarImage(DistanceImageType::Pointer inputImage, float lowerThreshold) {
    using ThresholdFilterType = itk::BinaryThresholdImageFilter<DistanceImageType, BinaryImageType>;
    auto thresholdFilter = ThresholdFilterType::New();
    thresholdFilter->SetInput(inputImage);
    thresholdFilter->SetLowerThreshold(lowerThreshold);
    thresholdFilter->SetInsideValue(1);
    thresholdFilter->SetOutsideValue(0);
    thresholdFilter->Update();
    return thresholdFilter->GetOutput();
}

} // namespace distance_map_benchmark
