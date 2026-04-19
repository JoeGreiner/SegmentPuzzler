#ifndef SEGMENTPUZZLER_LOGGINGSETTINGSDIALOG_H
#define SEGMENTPUZZLER_LOGGINGSETTINGSDIALOG_H

#include <QDialog>
#include <QMap>

#include "src/utils/AppLogger.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;

class LoggingSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoggingSettingsDialog(QWidget *parent = nullptr);

private slots:
    void applySettings();
    void browseForLogFile();
    void openLogFolder();
    void restoreDefaults();

private:
    void configureMinimumLevelComboBox() const;
    void loadSettingsIntoWidgets(const segment_puzzler::app_logging::LogSettings &settings);
    segment_puzzler::app_logging::LogSettings collectSettingsFromWidgets() const;
    int minimumLevelComboBoxWidth() const;
    QString resolvedLogDirectoryPath() const;
    void updateLogFileControls();

    QComboBox *minimumLevelComboBox_ = nullptr;
    QCheckBox *consoleEnabledCheckBox_ = nullptr;
    QCheckBox *fileEnabledCheckBox_ = nullptr;
    QLineEdit *logFilePathEdit_ = nullptr;
    QPushButton *browseLogFileButton_ = nullptr;
    QPushButton *openLogFolderButton_ = nullptr;
    QMap<QString, QCheckBox *> categoryCheckBoxes_;
};

#endif // SEGMENTPUZZLER_LOGGINGSETTINGSDIALOG_H
