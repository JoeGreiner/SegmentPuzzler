#include "SelectedSegmentationAutosave.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QWidget>

#include <itkExceptionObject.h>
#include <itkImageFileWriter.h>
#include <itkNrrdImageIO.h>

#include <algorithm>
#include <exception>
#include <utility>

#include "src/qtUtils/TaskRunner.h"
#include "src/segment_handling/graphBase.h"
#include "src/utils/AppLogger.h"
#include "src/utils/ExportPathUtils.h"

namespace {

constexpr auto kEnabledSettingsKey = "Recovery/selectedSegmentationAutosaveEnabled";
constexpr auto kIntervalSettingsKey = "Recovery/selectedSegmentationAutosaveIntervalMinutes";

QString describeCurrentException() {
    try {
        throw;
    } catch (const itk::ExceptionObject &exception) {
        return QString::fromStdString(exception.GetDescription());
    } catch (const std::exception &exception) {
        return QString::fromUtf8(exception.what());
    } catch (...) {
        return QStringLiteral("Unknown error");
    }
}

} // namespace

SelectedSegmentationAutosave::SelectedSegmentationAutosave(
    std::shared_ptr<GraphBase> graphBase,
    TaskRunner *taskRunner,
    QWidget *messageParent,
    QObject *parent)
    : QObject(parent),
      graphBase_(std::move(graphBase)),
      taskRunner_(taskRunner),
      messageParent_(messageParent),
      timer_(this),
      settings_(loadSettings())
{
    timer_.setObjectName(QStringLiteral("selectedSegmentationAutosaveTimer"));
    timer_.setTimerType(Qt::VeryCoarseTimer);
    connect(&timer_, &QTimer::timeout, this, &SelectedSegmentationAutosave::requestAutosave);
    if (taskRunner_ != nullptr) {
        connect(taskRunner_, &TaskRunner::busyChanged, this, [this](bool busy) {
            if (!busy && autosavePending_) {
                QTimer::singleShot(0, this, &SelectedSegmentationAutosave::requestAutosave);
            }
        }, Qt::QueuedConnection);
    }

    trackSelectedSegmentation();
    restartTimer();
}

SelectedSegmentationAutosave::~SelectedSegmentationAutosave() {
    cleanup();
}

SelectedSegmentationAutosave::Settings SelectedSegmentationAutosave::loadSettings() {
    QSettings storedSettings;
    Settings settings;
    settings.enabled = storedSettings.value(
        QString::fromLatin1(kEnabledSettingsKey), true).toBool();
    settings.intervalMinutes = std::clamp(
        storedSettings.value(QString::fromLatin1(kIntervalSettingsKey), 3).toInt(),
        1,
        120);
    return settings;
}

void SelectedSegmentationAutosave::storeSettings(const Settings &settings) {
    QSettings storedSettings;
    storedSettings.setValue(QString::fromLatin1(kEnabledSettingsKey), settings.enabled);
    storedSettings.setValue(
        QString::fromLatin1(kIntervalSettingsKey), settings.intervalMinutes);
}

SelectedSegmentationAutosave::Settings
SelectedSegmentationAutosave::currentSettings() const {
    return settings_;
}

void SelectedSegmentationAutosave::applySettings(const Settings &requestedSettings) {
    Settings newSettings = requestedSettings;
    newSettings.intervalMinutes = std::clamp(newSettings.intervalMinutes, 1, 120);
    const bool wasEnabled = settings_.enabled;
    settings_ = newSettings;
    storeSettings(settings_);

    if (!settings_.enabled) {
        cleanup();
        return;
    }

    failureWarningShown_ = false;
    if (!wasEnabled) {
        trackSelectedSegmentation();
    }
    restartTimer();
}

void SelectedSegmentationAutosave::restartTimer() {
    timer_.stop();
    if (!settings_.enabled) {
        return;
    }

    const qint64 intervalMs = static_cast<qint64>(settings_.intervalMinutes) * 60 * 1000;
    timer_.start(static_cast<int>(intervalMs));
}

void SelectedSegmentationAutosave::selectedSegmentationChanged() {
    const SegmentationKey currentKey = makeKey(
        graphBase_ != nullptr ? graphBase_->pSelectedSegmentation : nullptr,
        graphBase_ != nullptr ? graphBase_->pSelectedSegmentationSignal : nullptr);
    if (currentKey == selectedSegmentationKey_) {
        return;
    }

    trackSelectedSegmentation();
}

std::size_t SelectedSegmentationAutosave::SegmentationKeyHash::operator()(
    const SegmentationKey &key) const noexcept
{
    const std::size_t imageHash = std::hash<quintptr>{}(key.imageIdentity);
    const std::size_t signalHash = std::hash<quintptr>{}(key.signalIdentity);
    return imageHash ^ (signalHash + static_cast<std::size_t>(0x9e3779b9U)
                        + (imageHash << 6U) + (imageHash >> 2U));
}

SelectedSegmentationAutosave::SegmentationKey SelectedSegmentationAutosave::makeKey(
    const SegmentsImagePointer &image,
    const void *signal)
{
    return {reinterpret_cast<quintptr>(image.GetPointer()),
            reinterpret_cast<quintptr>(signal)};
}

void SelectedSegmentationAutosave::trackSelectedSegmentation() {
    const SegmentsImagePointer image =
        graphBase_ != nullptr ? graphBase_->pSelectedSegmentation : nullptr;
    const auto *signal =
        graphBase_ != nullptr ? graphBase_->pSelectedSegmentationSignal : nullptr;
    if (image == nullptr || signal == nullptr) {
        selectedSegmentationKey_ = {};
        return;
    }

    selectedSegmentationKey_ = makeKey(image, signal);
    auto [stateIt, inserted] = segmentationStates_.try_emplace(selectedSegmentationKey_);
    SegmentationRecoveryState &state = stateIt->second;
    state.image = image;
    state.name = signal->name;
    if (inserted && !signal->sourceFilePath.trimmed().isEmpty()) {
        state.persistedPath = QFileInfo(signal->sourceFilePath).absoluteFilePath();
        state.persistedMTime = image->GetMTime();
    }
}

SelectedSegmentationAutosave::SegmentationRecoveryState *
SelectedSegmentationAutosave::selectedSegmentationState() {
    const auto stateIt = segmentationStates_.find(selectedSegmentationKey_);
    return stateIt != segmentationStates_.end() ? &stateIt->second : nullptr;
}

const SelectedSegmentationAutosave::SegmentationRecoveryState *
SelectedSegmentationAutosave::selectedSegmentationState() const {
    const auto stateIt = segmentationStates_.find(selectedSegmentationKey_);
    return stateIt != segmentationStates_.end() ? &stateIt->second : nullptr;
}

bool SelectedSegmentationAutosave::isCoveredByPersistedSave(
    const SegmentationRecoveryState &state,
    itk::ModifiedTimeType mTime)
{
    return !state.persistedPath.isEmpty() && state.persistedMTime >= mTime;
}

bool SelectedSegmentationAutosave::hasUnsavedChanges(
    const SegmentationRecoveryState &state)
{
    return state.image != nullptr
        && !isCoveredByPersistedSave(state, state.image->GetMTime());
}

bool SelectedSegmentationAutosave::needsRecoveryWrite(
    const SegmentationRecoveryState &state)
{
    return hasUnsavedChanges(state)
        && (state.recoveryFilePath.isEmpty()
            || state.image->GetMTime() > state.recoveryMTime);
}

std::optional<SelectedSegmentationAutosave::SegmentationKey>
SelectedSegmentationAutosave::nextSegmentationNeedingRecovery() const {
    const auto selectedState = segmentationStates_.find(selectedSegmentationKey_);
    if (selectedState != segmentationStates_.end()
        && needsRecoveryWrite(selectedState->second)) {
        return selectedState->first;
    }

    for (const auto &[key, state] : segmentationStates_) {
        if (!(key == selectedSegmentationKey_) && needsRecoveryWrite(state)) {
            return key;
        }
    }
    return std::nullopt;
}

bool SelectedSegmentationAutosave::hasRecoveryWork() const {
    return nextSegmentationNeedingRecovery().has_value();
}

bool SelectedSegmentationAutosave::createRecoveryGenerationPath(
    const SegmentationRecoveryState &state,
    QString &path,
    QString &error) const
{
    const QString preferredPath = state.persistedPath;
    QString recoveryDirectoryPath;
    if (!preferredPath.trimmed().isEmpty()) {
        const QDir preferredDirectory = QFileInfo(preferredPath).absoluteDir();
        const QFileInfo preferredDirectoryInfo(preferredDirectory.absolutePath());
        if (preferredDirectory.exists() && preferredDirectoryInfo.isDir()
            && preferredDirectoryInfo.isWritable()) {
            recoveryDirectoryPath = preferredDirectory.absolutePath();
        }
    }

    if (recoveryDirectoryPath.isEmpty()) {
        const QString applicationDataPath =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (applicationDataPath.trimmed().isEmpty()) {
            error = tr("No writable application data directory is available.");
            return false;
        }
        QDir applicationDataDirectory(applicationDataPath);
        if (!applicationDataDirectory.mkpath(QStringLiteral("recovery"))) {
            error = tr("Could not create the recovery directory: %1")
                        .arg(applicationDataDirectory.filePath(QStringLiteral("recovery")));
            return false;
        }
        recoveryDirectoryPath =
            applicationDataDirectory.filePath(QStringLiteral("recovery"));
        const QDir recoveryDirectory(recoveryDirectoryPath);
        const QFileInfo fallbackInfo(recoveryDirectory.absolutePath());
        if (!fallbackInfo.isDir() || !fallbackInfo.isWritable()) {
            error = tr("The recovery directory is not writable: %1")
                        .arg(recoveryDirectory.absolutePath());
            return false;
        }
    }

    QString segmentationName = state.name;
    if (segmentationName.trimmed().isEmpty() && !preferredPath.trimmed().isEmpty()) {
        segmentationName = QFileInfo(preferredPath).completeBaseName();
    }
    QString fileNameStem = export_path_utils::sanitizedFileNameStem(segmentationName);
    fileNameStem = export_path_utils::sanitizedFileNameStem(fileNameStem.left(120));
    if (fileNameStem.isEmpty()) {
        fileNameStem = QStringLiteral("selected-segmentation");
    }
    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString generation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    path = QDir(recoveryDirectoryPath).filePath(
        QStringLiteral("%1-recovery-%2-%3.nrrd")
            .arg(fileNameStem, timestamp, generation));
    return true;
}

void SelectedSegmentationAutosave::requestAutosave() {
    if (!settings_.enabled || graphBase_ == nullptr) {
        return;
    }

    const SegmentationKey currentKey = makeKey(
        graphBase_->pSelectedSegmentation,
        graphBase_->pSelectedSegmentationSignal);
    if (!(currentKey == selectedSegmentationKey_)) {
        selectedSegmentationChanged();
    }
    const auto nextKey = nextSegmentationNeedingRecovery();
    if (!nextKey.has_value()) {
        autosavePending_ = false;
        return;
    }
    if (taskRunner_ == nullptr) {
        disableAfterFailure(tr("The background task runner is unavailable."));
        return;
    }
    if (taskRunner_->isBusy() || recoveryWriteInProgress_) {
        autosavePending_ = true;
        return;
    }

    startRecoveryWrite(*nextKey);
}

void SelectedSegmentationAutosave::startRecoveryWrite(const SegmentationKey &key) {
    const auto stateIt = segmentationStates_.find(key);
    if (stateIt == segmentationStates_.end() || stateIt->second.image == nullptr) {
        return;
    }
    SegmentationRecoveryState *state = &stateIt->second;

    QString path;
    QString pathError;
    if (!createRecoveryGenerationPath(*state, path, pathError)) {
        disableAfterFailure(pathError);
        return;
    }

    const SegmentsImagePointer image = state->image;
    const itk::ModifiedTimeType imageMTime = image->GetMTime();
    const QString previousPath = state->recoveryFilePath;
    const itk::ModifiedTimeType previousRecoveryMTime = state->recoveryMTime;
    writingSegmentationKey_ = key;
    autosavePending_ = false;
    recoveryWriteInProgress_ = true;
    const QPointer<SelectedSegmentationAutosave> guardedAutosave(this);

    taskRunner_->runInBackground(
        tr("Saving selected segmentation recovery..."),
        [image, path]() {
            WriteResult result;
            try {
                using Writer = itk::ImageFileWriter<dataType::SegmentsImageType>;
                auto imageIO = itk::NrrdImageIO::New();
                auto writer = Writer::New();
                writer->SetImageIO(imageIO);
                writer->SetFileName(path.toStdString());
                writer->SetInput(image);
                writer->SetUseCompression(true);
                writer->SetCompressionLevel(1);
                writer->Update();
                result.success = true;
            } catch (...) {
                result.error = describeCurrentException();
                QFile::remove(path);
            }
            return result;
        },
        [guardedAutosave, image, imageMTime, path, previousPath, previousRecoveryMTime,
         key = writingSegmentationKey_](WriteResult result) {
            if (guardedAutosave == nullptr) {
                return;
            }
            auto *autosave = guardedAutosave.data();
            autosave->recoveryWriteInProgress_ = false;
            if (!result.success) {
                const auto stateIt = autosave->segmentationStates_.find(key);
                if (stateIt != autosave->segmentationStates_.end()) {
                    stateIt->second.discardRecoveryWhenWriteFinishes = false;
                }
                autosave->disableAfterFailure(
                    autosave->tr("Could not write the selected segmentation recovery file.\n\n%1")
                        .arg(result.error),
                    path);
                return;
            }

            auto stateIt = autosave->segmentationStates_.find(key);
            if (stateIt == autosave->segmentationStates_.end()) {
                SP_LOG_WARNING(
                    "recovery",
                    QStringLiteral("Preserving recovery file with no matching in-memory state: %1")
                        .arg(path));
                return;
            }
            SegmentationRecoveryState &state = stateIt->second;
            state.recoveryFilePath = path;
            state.recoveryMTime = imageMTime;
            SP_LOG_INFO(
                "recovery",
                QStringLiteral("Saved selected segmentation recovery to %1 (mtime=%2)")
                    .arg(path)
                    .arg(static_cast<qulonglong>(imageMTime)));

            if (!previousPath.isEmpty() && previousPath != path) {
                autosave->removeRecoveryIfAllowed(
                    state,
                    previousPath,
                    previousRecoveryMTime,
                    RecoveryRemovalReason::SupersededGeneration);
            }
            if (state.discardRecoveryWhenWriteFinishes) {
                autosave->removeRecoveryIfAllowed(
                    state,
                    path,
                    imageMTime,
                    RecoveryRemovalReason::ExplicitDiscard);
                state.discardRecoveryWhenWriteFinishes = false;
            } else if (SelectedSegmentationAutosave::isCoveredByPersistedSave(
                           state, imageMTime)) {
                autosave->removeRecoveryIfAllowed(
                    state,
                    path,
                    imageMTime,
                    RecoveryRemovalReason::RegularSave);
            }

            if (autosave->settings_.enabled && autosave->hasRecoveryWork()) {
                autosave->autosavePending_ = true;
            }
        },
        {},
        true);
}

void SelectedSegmentationAutosave::disableAfterFailure(const QString &error,
                                                        const QString &partialPath) {
    if (!partialPath.isEmpty()) {
        QFile::remove(partialPath);
    }

    settings_.enabled = false;
    storeSettings(settings_);
    timer_.stop();
    autosavePending_ = false;
    recoveryWriteInProgress_ = false;

    SP_LOG_ERROR("recovery", QStringLiteral("Selected segmentation autosave disabled: %1").arg(error));
    if (!failureWarningShown_) {
        failureWarningShown_ = true;
        if (messageParent_ != nullptr) {
            QMessageBox::warning(
                messageParent_,
                tr("Selected Segmentation Recovery"),
                tr("Automatic recovery saving was disabled.\n\n%1").arg(error));
        }
    }
}

bool SelectedSegmentationAutosave::removeRecoveryIfAllowed(
    SegmentationRecoveryState &state,
    const QString &path,
    itk::ModifiedTimeType recoveryMTime,
    RecoveryRemovalReason reason)
{
    if (path.isEmpty()) {
        return true;
    }

    const bool coveredByRegularSave = isCoveredByPersistedSave(state, recoveryMTime);
    const bool supersededByRecovery =
        !state.recoveryFilePath.isEmpty()
        && state.recoveryFilePath != path
        && state.recoveryMTime >= recoveryMTime;
    const bool removalAllowed =
        reason == RecoveryRemovalReason::ExplicitDiscard
        || (reason == RecoveryRemovalReason::RegularSave && coveredByRegularSave)
        || (reason == RecoveryRemovalReason::SupersededGeneration
            && supersededByRecovery);
    if (!removalAllowed) {
        SP_LOG_WARNING(
            "recovery",
            QStringLiteral("Refused to remove unsaved selected segmentation recovery file %1")
                .arg(path));
        return false;
    }

    if (!QFile::remove(path) && QFile::exists(path)) {
        SP_LOG_WARNING(
            "recovery",
            QStringLiteral("Could not remove selected segmentation recovery file %1")
                .arg(path));
        return false;
    }
    SP_LOG_INFO(
        "recovery",
        QStringLiteral("Removed selected segmentation recovery file %1")
            .arg(path));
    if (state.recoveryFilePath == path) {
        state.recoveryFilePath.clear();
        state.recoveryMTime = 0;
    }
    return true;
}

void SelectedSegmentationAutosave::removePersistedRecoveries() {
    for (auto &[key, state] : segmentationStates_) {
        Q_UNUSED(key);
        if (!state.recoveryFilePath.isEmpty()
            && isCoveredByPersistedSave(state, state.recoveryMTime)) {
            removeRecoveryIfAllowed(state,
                                    state.recoveryFilePath,
                                    state.recoveryMTime,
                                    RecoveryRemovalReason::RegularSave);
        }
    }
}

void SelectedSegmentationAutosave::cleanup() {
    timer_.stop();
    autosavePending_ = false;
    removePersistedRecoveries();
}

bool SelectedSegmentationAutosave::discardUnsavedRecoveries() {
    bool removedAllRecoveries = true;
    for (auto &[key, state] : segmentationStates_) {
        if (!hasUnsavedChanges(state)) {
            continue;
        }
        if (!state.recoveryFilePath.isEmpty()) {
            removedAllRecoveries = removeRecoveryIfAllowed(
                                       state,
                                       state.recoveryFilePath,
                                       state.recoveryMTime,
                                       RecoveryRemovalReason::ExplicitDiscard)
                && removedAllRecoveries;
        }
        if (recoveryWriteInProgress_ && writingSegmentationKey_ == key) {
            state.discardRecoveryWhenWriteFinishes = true;
        }
    }
    return removedAllRecoveries;
}

void SelectedSegmentationAutosave::recordSelectedSegmentationSave(
    quintptr imageIdentity,
    quintptr signalIdentity,
    quint64 savedMTime,
    const QString &absolutePath)
{
    const SegmentationKey key{imageIdentity, signalIdentity};
    auto stateIt = segmentationStates_.find(key);
    if (stateIt == segmentationStates_.end()) {
        trackSelectedSegmentation();
        stateIt = segmentationStates_.find(key);
    }
    if (stateIt == segmentationStates_.end()) {
        SP_LOG_WARNING(
            "recovery",
            QStringLiteral("Could not associate a successful segmentation export with its autosave state"));
        return;
    }

    SegmentationRecoveryState &state = stateIt->second;
    state.persistedMTime = static_cast<itk::ModifiedTimeType>(savedMTime);
    state.persistedPath = QFileInfo(absolutePath).absoluteFilePath();
    if (!state.recoveryFilePath.isEmpty()
        && isCoveredByPersistedSave(state, state.recoveryMTime)) {
        removeRecoveryIfAllowed(state,
                                state.recoveryFilePath,
                                state.recoveryMTime,
                                RecoveryRemovalReason::RegularSave);
    }
}

bool SelectedSegmentationAutosave::hasUnsavedSegmentations() const {
    return std::any_of(
        segmentationStates_.begin(),
        segmentationStates_.end(),
        [](const auto &entry) { return hasUnsavedChanges(entry.second); });
}

bool SelectedSegmentationAutosave::hasUnsavedRecovery() const {
    return std::any_of(
        segmentationStates_.begin(),
        segmentationStates_.end(),
        [](const auto &entry) {
            const SegmentationRecoveryState &state = entry.second;
            return hasUnsavedChanges(state)
                && !state.recoveryFilePath.isEmpty()
                && QFile::exists(state.recoveryFilePath);
        });
}

bool SelectedSegmentationAutosave::isWriteInProgress() const {
    return recoveryWriteInProgress_;
}

QString SelectedSegmentationAutosave::selectedSegmentationRecoveryPath() const {
    const SegmentationRecoveryState *state = selectedSegmentationState();
    return state != nullptr ? state->recoveryFilePath : QString();
}
