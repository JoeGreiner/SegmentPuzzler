#include "WorkerThreads.h"

#include <itksys/SystemInformation.hxx>

#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace segment_puzzler {
namespace {

int environmentThreadCount(const char *name, bool allowList) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return 0;
    }

    std::string_view remaining(value);
    int firstCount = 0;
    while (true) {
        const auto comma = remaining.find(',');
        auto item = remaining.substr(0, comma);
        const auto first = item.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return 0;
        }
        item = item.substr(first, item.find_last_not_of(" \t\r\n") - first + 1);
        int count = 0;
        const auto parsed = std::from_chars(item.data(), item.data() + item.size(), count);
        if (parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size() || count <= 0) {
            return 0;
        }
        if (firstCount == 0) {
            firstCount = count;
        }
        if (comma == std::string_view::npos) {
            return firstCount;
        }
        if (!allowList) {
            return 0;
        }
        remaining.remove_prefix(comma + 1);
    }
}

int physicalCoreCount() {
    static const int count = [] {
        itksys::SystemInformation system;
        system.RunCPUCheck();
        const auto cores = system.GetNumberOfPhysicalCPU();
        return cores > 0 && cores <= static_cast<unsigned int>(std::numeric_limits<int>::max())
            ? static_cast<int>(cores) : 1;
    }();
    return count;
}

} // namespace

int workerThreadCount(ThreadBackend backend) {
#ifndef USE_OMP
    if (backend == ThreadBackend::OpenMP) {
        return 1;
    }
#endif

    const char *environmentName = nullptr;
    switch (backend) {
        case ThreadBackend::OpenMP: environmentName = "OMP_NUM_THREADS"; break;
        case ThreadBackend::ITK: environmentName = "ITK_GLOBAL_DEFAULT_NUMBER_OF_THREADS"; break;
        case ThreadBackend::VTK: environmentName = "VTK_SMP_MAX_THREADS"; break;
        default: throw std::invalid_argument("Unknown threading backend.");
    }
    const int configured = environmentThreadCount(environmentName, backend == ThreadBackend::OpenMP);
    return configured > 0 ? configured : physicalCoreCount();
}

} // namespace segment_puzzler
