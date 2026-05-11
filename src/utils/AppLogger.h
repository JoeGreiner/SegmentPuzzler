#ifndef SEGMENTPUZZLER_APPLOGGER_H
#define SEGMENTPUZZLER_APPLOGGER_H

#include <QHash>
#include <QList>
#include <QString>

#include <mutex>
#include <string>

namespace segment_puzzler::app_logging {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

struct LogCategoryDefinition {
    QString id;
    QString label;
};

struct LogSettings {
    LogLevel minimumLevel = LogLevel::Info;
    bool consoleEnabled = true;
    bool fileEnabled = false;
    QString logFilePath;
    QHash<QString, bool> categoryEnabled;
};

class AppLogger {
public:
    static void initialize();
    static const QList<LogCategoryDefinition> &categories();
    static LogSettings defaultSettings();
    static LogSettings settings();
    static void setSettings(const LogSettings &settings, bool persist = true);
    static QString defaultLogFilePath();
    static QString levelName(LogLevel level);

    static void log(LogLevel level,
                    const QString &category,
                    const QString &message,
                    const char *functionName);
    static void logChanged(LogLevel level,
                           const QString &category,
                           const QString &dedupeKey,
                           const QString &message,
                           const char *functionName);

private:
    AppLogger() = default;

    static AppLogger &instance();

    void ensureInitializedUnlocked();
    LogSettings normalizeSettingsUnlocked(const LogSettings &settings) const;
    LogSettings readSettingsFromDiskUnlocked() const;
    void writeSettingsToDiskUnlocked(const LogSettings &settings) const;
    bool shouldLogUnlocked(LogLevel level, const QString &category) const;
    QString formatLineUnlocked(LogLevel level,
                               const QString &category,
                               const QString &message,
                               const char *functionName) const;
    void writeConsoleUnlocked(LogLevel level, const QString &line) const;
    void writeFileUnlocked(const QString &line);
    void rotateLogFileUnlocked(const QString &logFilePath);

    mutable std::mutex mutex_;
    LogSettings settings_;
    bool initialized_ = false;
    QHash<QString, QString> lastMessages_;
};

namespace detail {

inline QString asQString(const QString &value) {
    return value;
}

inline QString asQString(const char *value) {
    return value != nullptr ? QString::fromUtf8(value) : QString();
}

inline QString asQString(const std::string &value) {
    return QString::fromStdString(value);
}

} // namespace detail

} // namespace segment_puzzler::app_logging

#define SP_LOG(level, category, message) \
    ::segment_puzzler::app_logging::AppLogger::log( \
        level, \
        ::segment_puzzler::app_logging::detail::asQString(category), \
        ::segment_puzzler::app_logging::detail::asQString(message), \
        __func__)

#define SP_LOG_CHANGED(level, category, dedupeKey, message) \
    ::segment_puzzler::app_logging::AppLogger::logChanged( \
        level, \
        ::segment_puzzler::app_logging::detail::asQString(category), \
        ::segment_puzzler::app_logging::detail::asQString(dedupeKey), \
        ::segment_puzzler::app_logging::detail::asQString(message), \
        __func__)

#define SP_LOG_TRACE(category, message) SP_LOG(::segment_puzzler::app_logging::LogLevel::Trace, category, message)
#define SP_LOG_DEBUG(category, message) SP_LOG(::segment_puzzler::app_logging::LogLevel::Debug, category, message)
#define SP_LOG_INFO(category, message) SP_LOG(::segment_puzzler::app_logging::LogLevel::Info, category, message)
#define SP_LOG_WARNING(category, message) SP_LOG(::segment_puzzler::app_logging::LogLevel::Warning, category, message)
#define SP_LOG_ERROR(category, message) SP_LOG(::segment_puzzler::app_logging::LogLevel::Error, category, message)

#define SP_LOG_TRACE_CHANGED(category, dedupeKey, message) \
    SP_LOG_CHANGED(::segment_puzzler::app_logging::LogLevel::Trace, category, dedupeKey, message)
#define SP_LOG_DEBUG_CHANGED(category, dedupeKey, message) \
    SP_LOG_CHANGED(::segment_puzzler::app_logging::LogLevel::Debug, category, dedupeKey, message)
#define SP_LOG_INFO_CHANGED(category, dedupeKey, message) \
    SP_LOG_CHANGED(::segment_puzzler::app_logging::LogLevel::Info, category, dedupeKey, message)

#endif // SEGMENTPUZZLER_APPLOGGER_H
