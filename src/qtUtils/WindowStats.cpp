#include "WindowStats.h"
#include <QApplication>
#include <QStringList>
#include <QTimer>
#include "src/utils/systemStats.h"
#include <algorithm>

namespace windowStats {

namespace {

QString escapedLogValue(QString value) {
    value.replace('\\', QStringLiteral("\\\\"));
    value.replace('"', QStringLiteral("\\\""));
    value.replace('\n', QStringLiteral("\\n"));
    value.replace('\r', QStringLiteral("\\r"));
    return value;
}

} // namespace

QString describeTopLevelWindows() {
    const QWidgetList windows = QApplication::topLevelWidgets();
    QStringList descriptions;
    descriptions.reserve(windows.size());

    int visibleWindowCount = 0;
    int visibleQuitOnCloseWindowCount = 0;
    for (QWidget *window : windows) {
        if (window == nullptr) {
            continue;
        }

        const bool visible = window->isVisible();
        const bool quitOnClose = window->testAttribute(Qt::WA_QuitOnClose);
        if (visible) {
            ++visibleWindowCount;
            if (quitOnClose) {
                ++visibleQuitOnCloseWindowCount;
            }
        }

        descriptions.push_back(
            QStringLiteral("{class=\"%1\",object=\"%2\",title=\"%3\",visible=%4,hidden=%5,"
                           "minimized=%6,active=%7,enabled=%8,quit_on_close=%9,window_type=%10}")
                .arg(escapedLogValue(QString::fromLatin1(window->metaObject()->className())))
                .arg(escapedLogValue(window->objectName()))
                .arg(escapedLogValue(window->windowTitle()))
                .arg(visible)
                .arg(window->isHidden())
                .arg(window->isMinimized())
                .arg(window->isActiveWindow())
                .arg(window->isEnabled())
                .arg(quitOnClose)
                .arg(static_cast<int>(window->windowFlags() & Qt::WindowType_Mask)));
    }

    descriptions.sort();
    return QStringLiteral("quit_on_last_window_closed=%1 top_level_count=%2 visible_count=%3 "
                          "visible_quit_on_close_count=%4 windows=[%5]")
        .arg(QApplication::quitOnLastWindowClosed())
        .arg(windows.size())
        .arg(visibleWindowCount)
        .arg(visibleQuitOnCloseWindowCount)
        .arg(descriptions.join(QStringLiteral(", ")));
}

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
