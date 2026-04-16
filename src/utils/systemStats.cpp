#include "systemStats.h"

// ---------------------------------------------------------------------------
// macOS
// ---------------------------------------------------------------------------
#if defined(__APPLE__)

#include <mach/mach.h>
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

    // RAM
    uint64_t totalMem = 0;
    size_t sz = sizeof(totalMem);
    sysctlbyname("hw.memsize", &totalMem, &sz, nullptr, 0);
    stats.memTotalGB = static_cast<double>(totalMem) / (1024.0 * 1024.0 * 1024.0);

    vm_size_t pageSize = 0;
    host_page_size(mach_host_self(), &pageSize);

    vm_statistics64_data_t vmStats{};
    mach_msg_type_number_t infoCount = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64,
                      reinterpret_cast<host_info64_t>(&vmStats), &infoCount);

    uint64_t availBytes = static_cast<uint64_t>(vmStats.free_count + vmStats.inactive_count)
                          * static_cast<uint64_t>(pageSize);
    stats.memAvailGB = static_cast<double>(availBytes) / (1024.0 * 1024.0 * 1024.0);

    // Swap
    xsw_usage swapUsage{};
    size_t swapSz = sizeof(swapUsage);
    sysctlbyname("vm.swapusage", &swapUsage, &swapSz, nullptr, 0);
    stats.swapTotalGB = static_cast<double>(swapUsage.xsu_total) / (1024.0 * 1024.0 * 1024.0);
    stats.swapUsedGB  = static_cast<double>(swapUsage.xsu_used)  / (1024.0 * 1024.0 * 1024.0);

    return stats;
}

} // namespace systemStats

// ---------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------
#elif defined(__linux__)

#include <unistd.h>
#include <fstream>

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

    // RAM + Swap (/proc/meminfo values are in kB)
    constexpr double kbToGB = 1.0 / (1024.0 * 1024.0);
    std::ifstream meminfo("/proc/meminfo");
    std::string key, unit;
    long long val;
    long long memTotal = 0, memAvail = 0, swapTotal = 0, swapFree = 0;
    while (meminfo >> key >> val >> unit) {
        if      (key == "MemTotal:")     memTotal  = val;
        else if (key == "MemAvailable:") memAvail  = val;
        else if (key == "SwapTotal:")    swapTotal = val;
        else if (key == "SwapFree:")     swapFree  = val;
    }
    stats.memTotalGB  = memTotal  * kbToGB;
    stats.memAvailGB  = memAvail  * kbToGB;
    stats.swapTotalGB = swapTotal * kbToGB;
    stats.swapUsedGB  = (swapTotal - swapFree) * kbToGB;

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

    // RAM + Swap
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    constexpr double toGB = 1.0 / (1024.0 * 1024.0 * 1024.0);
    stats.memTotalGB  = mem.ullTotalPhys * toGB;
    stats.memAvailGB  = mem.ullAvailPhys * toGB;
    stats.swapTotalGB = mem.ullTotalPageFile * toGB;
    stats.swapUsedGB  = (mem.ullTotalPageFile - mem.ullAvailPageFile) * toGB;

    return stats;
}

} // namespace systemStats

// ---------------------------------------------------------------------------
// Unsupported platform — no-op fallback
// ---------------------------------------------------------------------------
#else

namespace systemStats {
SystemStats query() { return {}; }
} // namespace systemStats

#endif

