#include "utils.h"
#include <cmath>
#include <limits>
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
        itk::Index<3> &indexOut
) {
    if (segmentationImage == nullptr || labelValue == 0) {
        return false;
    }

    const auto &fullRegion = segmentationImage->GetLargestPossibleRegion();
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(segmentationImage, fullRegion);

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
