#include "systemStats.h"

namespace {

void copyMemoryStats(const MemoryStats &memory, SystemStats &system) {
    system.memAvailGB = memory.availableSystemMemoryGB;
    system.memTotalGB = memory.totalSystemMemoryGB;
    system.swapUsedGB = memory.swapUsedGB;
    system.swapTotalGB = memory.swapTotalGB;
}

} // namespace

// ---------------------------------------------------------------------------
// macOS
// ---------------------------------------------------------------------------
#if defined(__APPLE__)

#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <vector>

namespace {

struct PerCoreTicks {
    uint32_t user = 0, sys = 0, idle = 0, nice = 0;
};

static std::vector<PerCoreTicks> sPrevTicks;

std::vector<PerCoreTicks> readCoreTicks() {
    natural_t numCPUs = 0;
    processor_info_array_t cpuInfo = nullptr;
    mach_msg_type_number_t numCpuInfo = 0;

    kern_return_t kr = host_processor_info(mach_host_self(),
                                           PROCESSOR_CPU_LOAD_INFO,
                                           &numCPUs, &cpuInfo, &numCpuInfo);
    if (kr != KERN_SUCCESS) return {};

    std::vector<PerCoreTicks> ticks(numCPUs);
    for (natural_t i = 0; i < numCPUs; ++i) {
        ticks[i].user = cpuInfo[CPU_STATE_MAX * i + CPU_STATE_USER];
        ticks[i].sys  = cpuInfo[CPU_STATE_MAX * i + CPU_STATE_SYSTEM];
        ticks[i].idle = cpuInfo[CPU_STATE_MAX * i + CPU_STATE_IDLE];
        ticks[i].nice = cpuInfo[CPU_STATE_MAX * i + CPU_STATE_NICE];
    }
    vm_deallocate(mach_task_self(), (vm_address_t)cpuInfo,
                  (vm_size_t)(sizeof(integer_t) * numCpuInfo));
    return ticks;
}

} // namespace

namespace systemStats {

MemoryStats queryMemory() {
    constexpr double bytesToGB = 1.0 / (1024.0 * 1024.0 * 1024.0);
    MemoryStats stats;

    uint64_t totalMemoryBytes = 0;
    size_t totalMemorySize = sizeof(totalMemoryBytes);
    sysctlbyname("hw.memsize", &totalMemoryBytes, &totalMemorySize, nullptr, 0);
    stats.totalSystemMemoryGB = static_cast<double>(totalMemoryBytes) * bytesToGB;

    vm_size_t pageSize = 0;
    host_page_size(mach_host_self(), &pageSize);
    vm_statistics64_data_t vmStats{};
    mach_msg_type_number_t infoCount = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(),
                          HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vmStats),
                          &infoCount) == KERN_SUCCESS) {
        const uint64_t availableBytes =
            static_cast<uint64_t>(vmStats.free_count + vmStats.inactive_count) *
            static_cast<uint64_t>(pageSize);
        stats.availableSystemMemoryGB = static_cast<double>(availableBytes) * bytesToGB;
    }

    xsw_usage swapUsage{};
    size_t swapSize = sizeof(swapUsage);
    if (sysctlbyname("vm.swapusage", &swapUsage, &swapSize, nullptr, 0) == 0) {
        stats.swapTotalGB = static_cast<double>(swapUsage.xsu_total) * bytesToGB;
        stats.swapUsedGB = static_cast<double>(swapUsage.xsu_used) * bytesToGB;
    }

    task_basic_info_data_t taskInfo{};
    mach_msg_type_number_t taskInfoCount = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&taskInfo),
                  &taskInfoCount) == KERN_SUCCESS) {
        stats.processResidentMemoryGB = static_cast<double>(taskInfo.resident_size) * bytesToGB;
    }

    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        stats.peakProcessResidentMemoryGB = static_cast<double>(usage.ru_maxrss) * bytesToGB;
    }
    return stats;
}

SystemStats query() {
    SystemStats stats{};

    // CPU
    auto cur = readCoreTicks();
    stats.numCores = (int)cur.size();

    if (!sPrevTicks.empty() && sPrevTicks.size() == cur.size()) {
        double totalBusy = 0.0;
        for (size_t i = 0; i < cur.size(); ++i) {
            auto dUser  = cur[i].user - sPrevTicks[i].user;
            auto dSys   = cur[i].sys  - sPrevTicks[i].sys;
            auto dIdle  = cur[i].idle - sPrevTicks[i].idle;
            auto dNice  = cur[i].nice - sPrevTicks[i].nice;
            auto dBusy  = dUser + dSys + dNice;
            auto dTotal = dBusy + dIdle;
            if (dTotal > 0)
                totalBusy += static_cast<double>(dBusy) / dTotal * 100.0;
        }
        stats.cpuTotalPercent = totalBusy;
    }
    sPrevTicks = cur;

    copyMemoryStats(queryMemory(), stats);

    return stats;
}

} // namespace systemStats

// ---------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------
#elif defined(__linux__)

#include <unistd.h>
#include <fstream>
#include <limits>

namespace {

struct CpuSnapshot {
    long long user = 0, nice = 0, system = 0, idle = 0,
              iowait = 0, irq = 0, softirq = 0, steal = 0;
    long long busy()  const { return user + nice + system + irq + softirq + steal; }
    long long total() const { return busy() + idle + iowait; }
};

static CpuSnapshot sPrev{};
static bool sHasPrev = false;

CpuSnapshot readCpuSnapshot() {
    std::ifstream f("/proc/stat");
    std::string label;
    CpuSnapshot s;
    f >> label >> s.user >> s.nice >> s.system >> s.idle
               >> s.iowait >> s.irq >> s.softirq >> s.steal;
    return s;
}

} // namespace

namespace systemStats {

MemoryStats queryMemory() {
    constexpr double kilobytesToGB = 1.0 / (1024.0 * 1024.0);
    MemoryStats stats;

    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::string unit;
    long long value = 0;
    long long memoryTotalKB = 0;
    long long memoryAvailableKB = 0;
    long long swapTotalKB = 0;
    long long swapFreeKB = 0;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemTotal:") {
            memoryTotalKB = value;
        } else if (key == "MemAvailable:") {
            memoryAvailableKB = value;
        } else if (key == "SwapTotal:") {
            swapTotalKB = value;
        } else if (key == "SwapFree:") {
            swapFreeKB = value;
        }
    }
    stats.totalSystemMemoryGB = static_cast<double>(memoryTotalKB) * kilobytesToGB;
    stats.availableSystemMemoryGB = static_cast<double>(memoryAvailableKB) * kilobytesToGB;
    stats.swapTotalGB = static_cast<double>(swapTotalKB) * kilobytesToGB;
    stats.swapUsedGB = static_cast<double>(swapTotalKB - swapFreeKB) * kilobytesToGB;

    std::ifstream statm("/proc/self/statm");
    long totalPages = 0;
    long residentPages = 0;
    if (statm >> totalPages >> residentPages) {
        const double pageSizeGB = static_cast<double>(sysconf(_SC_PAGESIZE)) /
                                  (1024.0 * 1024.0 * 1024.0);
        stats.processResidentMemoryGB = static_cast<double>(residentPages) * pageSizeGB;
    }

    std::ifstream status("/proc/self/status");
    while (status >> key) {
        if (key == "VmHWM:") {
            long peakResidentKB = 0;
            status >> peakResidentKB;
            stats.peakProcessResidentMemoryGB = static_cast<double>(peakResidentKB) * kilobytesToGB;
            break;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return stats;
}

SystemStats query() {
    SystemStats stats{};
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    stats.numCores = (n > 0) ? static_cast<int>(n) : 1;

    // CPU
    CpuSnapshot cur = readCpuSnapshot();
    if (sHasPrev) {
        long long dBusy  = cur.busy()  - sPrev.busy();
        long long dTotal = cur.total() - sPrev.total();
        if (dTotal > 0)
            stats.cpuTotalPercent =
                static_cast<double>(dBusy) / dTotal * 100.0 * stats.numCores;
    }
    sPrev    = cur;
    sHasPrev = true;

    copyMemoryStats(queryMemory(), stats);

    return stats;
}

} // namespace systemStats

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
#elif defined(_WIN32)

#include <windows.h>

namespace {
static ULONGLONG sPrevIdle = 0, sPrevKernel = 0, sPrevUser = 0;
static bool sHasPrev = false;
} // namespace

namespace systemStats {

MemoryStats queryMemory() {
    constexpr double bytesToGB = 1.0 / (1024.0 * 1024.0 * 1024.0);
    MemoryStats stats;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        stats.totalSystemMemoryGB = static_cast<double>(memory.ullTotalPhys) * bytesToGB;
        stats.availableSystemMemoryGB = static_cast<double>(memory.ullAvailPhys) * bytesToGB;
        stats.swapTotalGB = static_cast<double>(memory.ullTotalPageFile) * bytesToGB;
        stats.swapUsedGB = static_cast<double>(memory.ullTotalPageFile - memory.ullAvailPageFile) * bytesToGB;
    }
    return stats;
}

SystemStats query() {
    SystemStats stats{};

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    stats.numCores = static_cast<int>(si.dwNumberOfProcessors);

    // CPU
    FILETIME ftIdle{}, ftKernel{}, ftUser{};
    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        auto toULL = [](FILETIME ft) -> ULONGLONG {
            return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        ULONGLONG idle   = toULL(ftIdle);
        ULONGLONG kernel = toULL(ftKernel); // includes idle time on Windows
        ULONGLONG user   = toULL(ftUser);

        if (sHasPrev) {
            ULONGLONG dIdle   = idle   - sPrevIdle;
            ULONGLONG dKernel = kernel - sPrevKernel;
            ULONGLONG dUser   = user   - sPrevUser;
            ULONGLONG dBusy   = (dKernel - dIdle) + dUser;
            ULONGLONG dTotal  = dKernel + dUser;
            if (dTotal > 0)
                stats.cpuTotalPercent =
                    static_cast<double>(dBusy) / dTotal * 100.0 * stats.numCores;
        }
        sPrevIdle   = idle;
        sPrevKernel = kernel;
        sPrevUser   = user;
        sHasPrev    = true;
    }

    copyMemoryStats(queryMemory(), stats);

    return stats;
}

} // namespace systemStats

// ---------------------------------------------------------------------------
// Unsupported platform — no-op fallback
// ---------------------------------------------------------------------------
#else

namespace systemStats {
MemoryStats queryMemory() { return {}; }
SystemStats query() { return {}; }
} // namespace systemStats

#endif
