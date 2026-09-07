#pragma once

#include "WorkerThreads.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace segment_puzzler {

template<typename T>
std::pair<T, T> minMaxValues(const T *buffer, std::size_t voxelCount) {
    const int threadCount = workerThreadCount();
    const std::pair<T, T> initial{std::numeric_limits<T>::max(), std::numeric_limits<T>::lowest()};
    std::vector<std::pair<T, T>> localExtrema(threadCount, initial);
    const std::size_t chunkSize = voxelCount / threadCount;
    const std::size_t remainder = voxelCount % threadCount;
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount) schedule(static)
#endif
    for (int worker = 0; worker < threadCount; ++worker) {
        const std::size_t begin = worker * chunkSize + std::min<std::size_t>(worker, remainder);
        const std::size_t end = begin + chunkSize + (worker < remainder);
        T minimum = initial.first;
        T maximum = initial.second;
        for (std::size_t index = begin; index < end; ++index) {
            const T value = buffer[index];
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        localExtrema[worker] = {minimum, maximum};
    }
    auto result = initial;
    for (const auto &extrema : localExtrema) {
        result.first = std::min(result.first, extrema.first);
        result.second = std::max(result.second, extrema.second);
    }
    return result;
}

} // namespace segment_puzzler
