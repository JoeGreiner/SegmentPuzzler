#pragma once

#include "WorkerThreads.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <unordered_set>
#include <vector>

namespace segment_puzzler {

// Returns one past the greatest non-background ID, or 0 if none; optionally collects labels.
inline std::uint64_t scanLabels(const std::uint32_t *pixels, std::size_t count, std::uint32_t background,
                               std::unordered_set<std::uint32_t> *labels = nullptr) {
    struct LocalResult {
        std::unordered_set<std::uint32_t> labels;
        std::uint64_t nextLabel = 0;
        std::exception_ptr error;
    };
    const int threadCount = workerThreadCount();
    std::vector<LocalResult> results(threadCount);
    const auto chunkSize = count / threadCount;
    const auto remainder = count % threadCount;
#ifdef USE_OMP
#pragma omp parallel for num_threads(threadCount) schedule(static)
#endif
    for (int worker = 0; worker < threadCount; ++worker) {
        auto &local = results[worker];
        const auto begin = worker * chunkSize + std::min<std::size_t>(worker, remainder);
        const auto end = begin + chunkSize + (worker < remainder);
        try {
            for (auto index = begin; index < end;) {
                const auto label = pixels[index++];
                while (index < end && pixels[index] == label) ++index;
                if (labels) local.labels.insert(label);
                if (label != background)
                    local.nextLabel = std::max(local.nextLabel, std::uint64_t(label) + 1);
            }
        } catch (...) {
            local.error = std::current_exception();
        }
    }
    for (const auto &local : results)
        if (local.error) std::rethrow_exception(local.error);
    std::uint64_t nextLabel = 0;
    for (auto &local : results) {
        if (labels) labels->merge(local.labels);
        nextLabel = std::max(nextLabel, local.nextLabel);
    }
    return nextLabel;
}

} // namespace segment_puzzler
