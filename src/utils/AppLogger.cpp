#include "AppLogger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>

namespace segment_puzzler::app_logging {
namespace {

constexpr qint64 kMaxLogFileBytes = 1024 * 1024;
constexpr const char *kSettingsGroup = "logging";
constexpr const char *kSettingsMinimumLevel = "minimumLevel";
constexpr const char *kSettingsConsoleEnabled = "consoleEnabled";
constexpr const char *kSettingsFileEnabled = "fileEnabled";
constexpr const char *kSettingsFilePath = "filePath";
constexpr const char *kSettingsCategories = "categories";

const QList<LogCategoryDefinition> &categoryDefinitions() {
    static const QList<LogCategoryDefinition> definitions{
        {QStringLiteral("app"), QStringLiteral("Application")},
        {QStringLiteral("io"), QStringLiteral("I/O")},
        {QStringLiteral("tasks"), QStringLiteral("Tasks")},
        {QStringLiteral("watershed"), QStringLiteral("Watershed")},
        {QStringLiteral("segmentation"), QStringLiteral("Segmentation")},
        {QStringLiteral("viewer.interaction"), QStringLiteral("Viewer Interaction")},
        {QStringLiteral("viewer.render"), QStringLiteral("Viewer Render")},
        {QStringLiteral("viewer.three_d"), QStringLiteral("3D Viewer")},
        {QStringLiteral("network"), QStringLiteral("Network")}
    };
    return definitions;
}

LogLevel clampLevel(int rawLevel) {
    if (rawLevel <= static_cast<int>(LogLevel::Trace)) {
        return LogLevel::Trace;
    }
    if (rawLevel >= static_cast<int>(LogLevel::Error)) {
        return LogLevel::Error;
    }
    return static_cast<LogLevel>(rawLevel);
}

QString sanitizeMessage(const QString &message) {
    QString sanitized = message;
    sanitized.replace(QLatin1Char('\r'), QLatin1Char(' '));
    sanitized.replace(QLatin1Char('\n'), QStringLiteral(" | "));
    return sanitized.trimmed();
}

} // namespace

AppLogger &AppLogger::instance() {
    static AppLogger logger;
    return logger;
}

void AppLogger::initialize() {
    auto &logger = instance();
    std::lock_guard<std::mutex> guard(logger.mutex_);
    logger.ensureInitializedUnlocked();
}

const QList<LogCategoryDefinition> &AppLogger::categories() {
    return categoryDefinitions();
}

LogSettings AppLogger::defaultSettings() {
    LogSettings defaults;
    defaults.minimumLevel = LogLevel::Info;
    defaults.consoleEnabled = true;
    defaults.fileEnabled = false;
    defaults.logFilePath = defaultLogFilePath();
    for (const auto &category : categoryDefinitions()) {
        defaults.categoryEnabled.insert(category.id, true);
    }
    return defaults;
}

LogSettings AppLogger::settings() {
    auto &logger = instance();
    std::lock_guard<std::mutex> guard(logger.mutex_);
    logger.ensureInitializedUnlocked();
    return logger.settings_;
}

void AppLogger::setSettings(const LogSettings &settings, bool persist) {
    auto &logger = instance();
    std::lock_guard<std::mutex> guard(logger.mutex_);
    logger.ensureInitializedUnlocked();
    logger.settings_ = logger.normalizeSettingsUnlocked(settings);
    if (persist) {
        logger.writeSettingsToDiskUnlocked(logger.settings_);
    }
}

QString AppLogger::defaultLogFilePath() {
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (basePath.isEmpty()) {
        basePath = QDir::homePath() + QStringLiteral("/.segmentpuzzler");
    }
    return QDir(basePath).filePath(QStringLiteral("logs/segmentpuzzler.log"));
}

QString AppLogger::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return QStringLiteral("TRACE");
        case LogLevel::Debug:
            return QStringLiteral("DEBUG");
        case LogLevel::Info:
            return QStringLiteral("INFO");
        case LogLevel::Warning:
            return QStringLiteral("WARNING");
        case LogLevel::Error:
            return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

void AppLogger::log(LogLevel level,
                    const QString &category,
                    const QString &message,
                    const char *functionName) {
    auto &logger = instance();
    std::lock_guard<std::mutex> guard(logger.mutex_);
    logger.ensureInitializedUnlocked();
    if (!logger.shouldLogUnlocked(level, category)) {
        return;
    }

    const QString line = logger.formatLineUnlocked(level, category, message, functionName);
    if (logger.settings_.consoleEnabled) {
        logger.writeConsoleUnlocked(level, line);
    }
    if (logger.settings_.fileEnabled) {
        logger.writeFileUnlocked(line);
    }
}

void AppLogger::logChanged(LogLevel level,
                           const QString &category,
                           const QString &dedupeKey,
                           const QString &message,
                           const char *functionName) {
    auto &logger = instance();
    std::lock_guard<std::mutex> guard(logger.mutex_);
    logger.ensureInitializedUnlocked();
    if (!logger.shouldLogUnlocked(level, category)) {
        return;
    }

    const QString key = dedupeKey.isEmpty() ? category : dedupeKey;
    if (logger.lastMessages_.value(key) == message) {
        return;
    }
    logger.lastMessages_.insert(key, message);

    const QString line = logger.formatLineUnlocked(level, category, message, functionName);
    if (logger.settings_.consoleEnabled) {
        logger.writeConsoleUnlocked(level, line);
    }
    if (logger.settings_.fileEnabled) {
        logger.writeFileUnlocked(line);
    }
}

void AppLogger::ensureInitializedUnlocked() {
    if (initialized_) {
        return;
    }
    settings_ = readSettingsFromDiskUnlocked();
    initialized_ = true;
}

LogSettings AppLogger::normalizeSettingsUnlocked(const LogSettings &settings) const {
    LogSettings normalized = settings;
    normalized.minimumLevel = clampLevel(static_cast<int>(normalized.minimumLevel));
    if (normalized.logFilePath.isEmpty()) {
        normalized.logFilePath = defaultLogFilePath();
    }
    for (const auto &category : categoryDefinitions()) {
        if (!normalized.categoryEnabled.contains(category.id)) {
            normalized.categoryEnabled.insert(category.id, true);
        }
    }
    return normalized;
}

LogSettings AppLogger::readSettingsFromDiskUnlocked() const {
    LogSettings loaded = defaultSettings();

    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    loaded.minimumLevel = clampLevel(
        settings.value(QLatin1String(kSettingsMinimumLevel),
                       static_cast<int>(loaded.minimumLevel)).toInt());
    loaded.consoleEnabled = settings.value(QLatin1String(kSettingsConsoleEnabled),
                                           loaded.consoleEnabled).toBool();
    loaded.fileEnabled = settings.value(QLatin1String(kSettingsFileEnabled),
                                        loaded.fileEnabled).toBool();
    loaded.logFilePath = settings.value(QLatin1String(kSettingsFilePath),
                                        loaded.logFilePath).toString();

    settings.beginGroup(QLatin1String(kSettingsCategories));
    for (const auto &category : categoryDefinitions()) {
        loaded.categoryEnabled.insert(
            category.id,
            settings.value(category.id, loaded.categoryEnabled.value(category.id, true)).toBool());
    }
    settings.endGroup();
    settings.endGroup();

    return normalizeSettingsUnlocked(loaded);
}

void AppLogger::writeSettingsToDiskUnlocked(const LogSettings &settingsToWrite) const {
    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    settings.remove(QString());
    settings.setValue(QLatin1String(kSettingsMinimumLevel), static_cast<int>(settingsToWrite.minimumLevel));
    settings.setValue(QLatin1String(kSettingsConsoleEnabled), settingsToWrite.consoleEnabled);
    settings.setValue(QLatin1String(kSettingsFileEnabled), settingsToWrite.fileEnabled);
    settings.setValue(QLatin1String(kSettingsFilePath), settingsToWrite.logFilePath);
    settings.beginGroup(QLatin1String(kSettingsCategories));
    for (const auto &category : categoryDefinitions()) {
        settings.setValue(category.id, settingsToWrite.categoryEnabled.value(category.id, true));
    }
    settings.endGroup();
    settings.endGroup();
    settings.sync();
}

bool AppLogger::shouldLogUnlocked(LogLevel level, const QString &category) const {
    if (static_cast<int>(level) < static_cast<int>(settings_.minimumLevel)) {
        return false;
    }
    if (level == LogLevel::Warning || level == LogLevel::Error) {
        return true;
    }

    const QString effectiveCategory = category.isEmpty() ? QStringLiteral("app") : category;
    return settings_.categoryEnabled.value(effectiveCategory, true);
}

QString AppLogger::formatLineUnlocked(LogLevel level,
                                      const QString &category,
                                      const QString &message,
                                      const char *functionName) const {
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString effectiveCategory = category.isEmpty() ? QStringLiteral("app") : category;
    const QString effectiveFunction = functionName != nullptr && functionName[0] != '\0'
                                          ? QString::fromUtf8(functionName)
                                          : QStringLiteral("unknown");
    return QStringLiteral("%1 [%2] [%3] [%4] %5")
        .arg(timestamp,
             levelName(level),
             effectiveCategory,
             effectiveFunction,
             sanitizeMessage(message));
}

void AppLogger::writeConsoleUnlocked(LogLevel level, const QString &line) const {
    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    FILE *stream = (level == LogLevel::Warning || level == LogLevel::Error) ? stderr : stdout;
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stream);
    std::fflush(stream);
}

void AppLogger::writeFileUnlocked(const QString &line) {
    const QString logFilePath = settings_.logFilePath;
    if (logFilePath.isEmpty()) {
        return;
    }

    rotateLogFileUnlocked(logFilePath);

    const QFileInfo fileInfo(logFilePath);
    QDir directory;
    if (!directory.mkpath(fileInfo.absolutePath())) {
        return;
    }

    QFile file(logFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    file.write(bytes);
    file.close();
}

void AppLogger::rotateLogFileUnlocked(const QString &logFilePath) {
    QFileInfo fileInfo(logFilePath);
    if (!fileInfo.exists() || fileInfo.size() < kMaxLogFileBytes) {
        return;
    }

    const QString backupPath = logFilePath + QStringLiteral(".1");
    QFile::remove(backupPath);
    QFile logFile(logFilePath);
    logFile.rename(backupPath);
}

} // namespace segment_puzzler::app_logging
