#ifndef SEGMENTPUZZLER_GRAPHICSSETTINGS_H
#define SEGMENTPUZZLER_GRAPHICSSETTINGS_H

#include <QStringList>
#include <QSettings>
#include <algorithm>

class QWidget;

namespace graphicsSettings {

struct LodSettings {
    bool enabled = true;
    int triangleThreshold = 50'000'000;
    int reductionFactor = 4;
};

inline LodSettings lodSettings() {
    QSettings settings;
    return {settings.value(QStringLiteral("graphics/lodEnabled"), true).toBool(),
            std::max(1, settings.value(QStringLiteral("graphics/lodTriangleThreshold"),
                                      LodSettings{}.triangleThreshold).toInt()),
            std::clamp(settings.value(QStringLiteral("graphics/lodReductionFactor"),
                                      LodSettings{}.reductionFactor).toInt(), 1, 64)};
}

struct StartupOptions {
    QString mode;
    QString platform = QStringLiteral("auto");
    QString source;
    QString warning;
    QString error;
    QStringList qtArguments;
    bool printSettings = false;
    bool reset = false;
    bool test = false;
};

StartupOptions readStartupOptions(const QStringList &arguments);
QString saveSettings(const QString &mode, const QString &platform);
QString resetSettings();
void applyStartupOptions(const StartupOptions &options);
int runRenderTest();
void showDialog(QWidget *parent);

} // namespace graphicsSettings

#endif
