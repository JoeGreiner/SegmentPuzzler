#ifndef SEGMENTPUZZLER_GRAPHICSSETTINGS_H
#define SEGMENTPUZZLER_GRAPHICSSETTINGS_H

#include <QStringList>
#include <QSettings>
#include <algorithm>

class QWidget;

namespace graphicsSettings {

struct LodSettings {
    bool enabled = true;
    int voxelThreshold = 50'000'000;
    int reductionFactor = 4;
};

inline constexpr char lodEnabledKey[] = "graphics/lodEnabled";
inline constexpr char lodThresholdKey[] = "graphics/lodVoxelThreshold";
inline constexpr char lodFactorKey[] = "graphics/lodReductionFactor";

inline LodSettings lodSettings() {
    QSettings settings;
    const LodSettings defaults;
    return {settings.value(QLatin1String(lodEnabledKey), defaults.enabled).toBool(),
            std::max(1, settings.value(QLatin1String(lodThresholdKey),
                                      defaults.voxelThreshold).toInt()),
            std::clamp(settings.value(QLatin1String(lodFactorKey),
                                      defaults.reductionFactor).toInt(), 1, 64)};
}

struct StartupOptions {
    QString mode;
    QString platform = QStringLiteral("auto");
    QString source;
    QString warning;
    QString error;
    QStringList qtArguments;
    QStringList remainingArguments;
    bool printSettings = false;
    bool reset = false;
    bool test = false;
};

StartupOptions parseCommandLine(const QStringList &arguments);
StartupOptions readStartupOptions(const QStringList &arguments);
QString saveSettings(const QString &mode, const QString &platform);
QString resetSettings();
void applyStartupOptions(const StartupOptions &options);
int runRenderTest();
void showDialog(QWidget *parent);

} // namespace graphicsSettings

#endif
