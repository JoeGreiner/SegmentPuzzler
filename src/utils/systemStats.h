#ifndef SEGMENTPUZZLER_SYSTEMSTATS_H
#define SEGMENTPUZZLER_SYSTEMSTATS_H

// Lightweight system resource snapshot.
struct SystemStats {
    double cpuTotalPercent = 0.0; // sum of per-core usage, range 0..numCores*100
    int    numCores        = 0;
    double memAvailGB      = 0.0;
    double memTotalGB      = 0.0;
    double swapUsedGB      = 0.0;
    double swapTotalGB     = 0.0;
};

namespace systemStats {
    // Returns current resource usage.
    // cpuTotalPercent is 0 on the very first call; subsequent calls return
    // the usage accumulated since the previous call.
    // Call at a fixed interval (recommended: 1-2 s) from the GUI thread.
    SystemStats query();
}

#endif // SEGMENTPUZZLER_SYSTEMSTATS_H

