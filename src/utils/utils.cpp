#include <climits>
#include "utils.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <QFile>
#include <QString>
#include <itkImageRegionConstIterator.h>

#ifdef USE_OMP
#include <omp.h>
#else
#include <chrono>
#endif


double utils::tic(std::string description) {
    if (!description.empty()) {
        std::cout << description << "\n";
    }
#ifdef USE_OMP
    return omp_get_wtime();
#else
    // Use chrono if OpenMP is not available
    auto now = std::chrono::high_resolution_clock::now();
    // Store the time as a double (seconds)
    return std::chrono::duration<double>(now.time_since_epoch()).count();
#endif
}


void utils::toc(double start_time, std::string description) {
#ifdef USE_OMP
    double elapsed = omp_get_wtime() - start_time;
    std::cout << description << " " << elapsed << std::endl;
#else
    auto now = std::chrono::high_resolution_clock::now();
    double current_time = std::chrono::duration<double>(now.time_since_epoch()).count();
    double elapsed = current_time - start_time;
    std::cout << description << " " << elapsed << std::endl;
#endif
}

bool utils::check_if_file_exists(QString path_to_file) {
    QFile file(path_to_file);
    return file.open(QIODevice::ReadOnly);
}


dataType::EdgePairIdType utils::switchOrderOfPair(dataType::EdgePairIdType pairIn) {
    return {pairIn.second, pairIn.first};
}


utils::SegmentIdType utils::getOtherLabelOfPair(EdgePairIdType pair, SegmentIdType ownLabel) {
    SegmentIdType labelA = pair.first;
    SegmentIdType labelB = pair.second;
    return (ownLabel == labelA) ? labelB : labelA;
}


std::tuple<long, long, long, long, long, long> utils::calculateBoundingBoxForLabel(
        typename dataType::SegmentsImageType::Pointer segmentationImage,
        dataType::SegmentIdType labelValue
) {
    const auto &fullRegion = segmentationImage->GetBufferedRegion();
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> itBB(segmentationImage, fullRegion);

    long fx = std::numeric_limits<long>::max();
    long fy = std::numeric_limits<long>::max();
    long fz = std::numeric_limits<long>::max();
    long tx = std::numeric_limits<long>::lowest();
    long ty = std::numeric_limits<long>::lowest();
    long tz = std::numeric_limits<long>::lowest();

    for (itBB.GoToBegin(); !itBB.IsAtEnd(); ++itBB) {
        if (itBB.Get() == labelValue) {
            auto idx = itBB.GetIndex();
            long xI = idx[0], yI = idx[1], zI = idx[2];
            if (xI < fx) fx = xI;
            if (yI < fy) fy = yI;
            if (zI < fz) fz = zI;
            if (xI > tx) tx = xI;
            if (yI > ty) ty = yI;
            if (zI > tz) tz = zI;
        }
    }

    return std::make_tuple(fx, fy, fz, tx, ty, tz);
}

bool utils::findRepresentativeVoxelForLabel(
        typename dataType::SegmentsImageType::Pointer segmentationImage,
        dataType::SegmentIdType labelValue,
        itk::Index<3> &indexOut,
        const Roi &cachedBounds,
        std::uint64_t boundsModifiedTime
) {
    if (segmentationImage == nullptr || labelValue == 0) {
        return false;
    }

    auto searchRegion = segmentationImage->GetLargestPossibleRegion();
    if (boundsModifiedTime == segmentationImage->GetMTime()
        && cachedBounds.minX <= cachedBounds.maxX
        && cachedBounds.minY <= cachedBounds.maxY
        && cachedBounds.minZ <= cachedBounds.maxZ) {
        const itk::Index<3> first{{cachedBounds.minX, cachedBounds.minY, cachedBounds.minZ}};
        const itk::Index<3> last{{cachedBounds.maxX, cachedBounds.maxY, cachedBounds.maxZ}};
        if (searchRegion.IsInside(first) && searchRegion.IsInside(last)) {
            searchRegion.SetIndex(first);
            searchRegion.SetUpperIndex(last);
        }
    }
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(segmentationImage, searchRegion);

    double sumX = 0.0;
    double sumY = 0.0;
    double sumZ = 0.0;
    size_t count = 0;

    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        if (it.Get() != labelValue) {
            continue;
        }

        const auto index = it.GetIndex();
        sumX += static_cast<double>(index[0]);
        sumY += static_cast<double>(index[1]);
        sumZ += static_cast<double>(index[2]);
        ++count;
    }

    if (count == 0) {
        return false;
    }

    const double centroidX = sumX / static_cast<double>(count);
    const double centroidY = sumY / static_cast<double>(count);
    const double centroidZ = sumZ / static_cast<double>(count);

    double minDistanceSquared = std::numeric_limits<double>::max();
    bool found = false;

    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        if (it.Get() != labelValue) {
            continue;
        }

        const auto candidate = it.GetIndex();
        const double dx = static_cast<double>(candidate[0]) - centroidX;
        const double dy = static_cast<double>(candidate[1]) - centroidY;
        const double dz = static_cast<double>(candidate[2]) - centroidZ;
        const double distanceSquared = dx * dx + dy * dy + dz * dz;
        if (distanceSquared >= minDistanceSquared) {
            continue;
        }

        minDistanceSquared = distanceSquared;
        indexOut = candidate;
        found = true;
    }

    return found;
}

std::unordered_set<utils::SegmentIdType> utils::findPresentLabels(
        const dataType::SegmentsImageType::Pointer &segmentationImage,
        const std::unordered_set<SegmentIdType> &candidateLabels) {
    if (candidateLabels.empty()) {
        return {};
    }
    if (segmentationImage.IsNull() || segmentationImage->GetBufferPointer() == nullptr) {
        throw std::invalid_argument("Cannot search labels in a null or unallocated image.");
    }

    std::unordered_set<SegmentIdType> remaining = candidateLabels;
    std::unordered_set<SegmentIdType> present;
    present.reserve(candidateLabels.size());

    const auto numberOfPixels =
        segmentationImage->GetLargestPossibleRegion().GetNumberOfPixels();
    const SegmentIdType *buffer = segmentationImage->GetBufferPointer();
    for (std::size_t index = 0; index < numberOfPixels && !remaining.empty(); ++index) {
        const auto found = remaining.find(buffer[index]);
        if (found == remaining.end()) {
            continue;
        }
        present.insert(*found);
        remaining.erase(found);
    }
    return present;
}

std::tuple<long, long, long, long, long, long> utils::calculateBoundingBoxForLabel(
        dataType::SegmentsStorage::Pointer image, dataType::SegmentIdType label) {
    long fx = LONG_MAX, fy = LONG_MAX, fz = LONG_MAX, tx = LONG_MIN, ty = LONG_MIN, tz = LONG_MIN;
    if (const auto stats = image->labelStatistics(label)) {
        const auto first = stats->bounds.GetIndex(), last = stats->bounds.GetUpperIndex();
        return {first[0], first[1], first[2], last[0], last[1], last[2]};
    }
    return {fx, fy, fz, tx, ty, tz};
}

bool utils::findRepresentativeVoxelForLabel(dataType::SegmentsStorage::Pointer image,
        dataType::SegmentIdType label, itk::Index<3> &indexOut, const Roi &cachedBounds,
        std::uint64_t boundsModifiedTime) {
    if (!image || !label) return false;
    auto bounds = cachedBounds;
    if (boundsModifiedTime != image->GetMTime() || bounds.minX > bounds.maxX ||
        bounds.minY > bounds.maxY || bounds.minZ > bounds.maxZ) {
        const auto [fx, fy, fz, tx, ty, tz] = calculateBoundingBoxForLabel(image, label);
        if (fx > tx) return false;
        bounds.minX = fx; bounds.minY = fy; bounds.minZ = fz;
        bounds.maxX = tx; bounds.maxY = ty; bounds.maxZ = tz;
    }
    dataType::SegmentsImageType::RegionType region;
    region.SetIndex({bounds.minX, bounds.minY, bounds.minZ});
    region.SetUpperIndex({bounds.maxX, bounds.maxY, bounds.maxZ});
    if (!region.Crop(image->GetLargestPossibleRegion())) return false;
    return findRepresentativeVoxelForLabel(image->denseRegion(region), label, indexOut);
}

std::unordered_set<utils::SegmentIdType> utils::findPresentLabels(
        const dataType::SegmentsStorage::Pointer &image,
        const std::unordered_set<SegmentIdType> &candidates) {
    if (!image) throw std::invalid_argument("Cannot search labels in a null image.");
    std::unordered_set<SegmentIdType> present;
    if (!candidates.empty()) image->forEachRun([&](const auto &, std::size_t, auto label) {
        if (candidates.count(label)) present.insert(label);
    });
    return present;
}
