#ifndef SEGMENTPUZZLER_GRAPHICSSETTINGS_H
#define SEGMENTPUZZLER_GRAPHICSSETTINGS_H

#include <QStringList>

class QWidget;

namespace graphicsSettings {

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
