#include "WindowStats.h"
#include <QTimer>
#include "src/utils/systemStats.h"
#include <algorithm>

namespace windowStats {

void setupWindowTitleStatsTimer(QWidget *window, const QString &baseTitle) {
    if (!window) return;

    window->setWindowTitle(baseTitle);
    systemStats::query();
    auto *statsTimer = new QTimer(window);
    QObject::connect(statsTimer, &QTimer::timeout, window, [window, baseTitle]() {
        const SystemStats s = systemStats::query();
        const int cpuUsed = std::max(0, std::min(static_cast<int>(s.cpuTotalPercent + 0.5),
                                                  s.numCores * 100));
        const int cpuMax  = s.numCores * 100;
        QString title = baseTitle
                        + QString("  |  CPU %1/%2%").arg(cpuUsed).arg(cpuMax)
                        + QString("  |  RAM %1/%2 GB").arg(s.memTotalGB - s.memAvailGB, 0, 'f', 1).arg(static_cast<int>(s.memTotalGB + 0.5));
        if (s.swapTotalGB > 0.1) {
            title += QString("  |  Swap %1/%2 GB")
                         .arg(s.swapUsedGB, 0, 'f', 1)
                         .arg(static_cast<int>(s.swapTotalGB + 0.5));
        }
        window->setWindowTitle(title);
    });
    statsTimer->start(500);
}

} // namespace windowStats
