#ifndef SEGMENTPUZZLER_SELECTEDSEGMENTATIONAUTOSAVE_H
#define SEGMENTPUZZLER_SELECTEDSEGMENTATIONAUTOSAVE_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <itkIntTypes.h>

#include <memory>
#include <optional>
#include <unordered_map>

#include "src/file_definitions/dataTypes.h"

class GraphBase;
class TaskRunner;
class QWidget;

class SelectedSegmentationAutosave : public QObject {
public:
    struct Settings {
        bool enabled = true;
        int intervalMinutes = 3;
    };

    SelectedSegmentationAutosave(std::shared_ptr<GraphBase> graphBase,
                                 TaskRunner *taskRunner,
                                 QWidget *messageParent = nullptr,
                                 QObject *parent = nullptr);
    ~SelectedSegmentationAutosave() override;

    Settings currentSettings() const;
    void applySettings(const Settings &settings);

    void selectedSegmentationChanged();
    void recordSelectedSegmentationSave(quintptr imageIdentity,
                                        quintptr signalIdentity,
                                        quint64 savedMTime,
                                        const QString &absolutePath);
    void requestAutosave();

    // Stops autosaving and removes only recovery generations that are already
    // covered by a regular save. Unsaved recovery data is always retained.
    void cleanup();
    // The caller may use this only after an explicit user confirmation.
    bool discardUnsavedRecoveries();

    bool hasUnsavedSegmentations() const;
    bool hasUnsavedRecovery() const;
    bool isWriteInProgress() const;
    QString selectedSegmentationRecoveryPath() const;

private:
    using SegmentsImagePointer = dataType::SegmentsImageType::Pointer;

    struct SegmentationKey {
        quintptr imageIdentity = 0;
        quintptr signalIdentity = 0;

        bool operator==(const SegmentationKey &other) const {
            return imageIdentity == other.imageIdentity
                && signalIdentity == other.signalIdentity;
        }
    };

    struct SegmentationKeyHash {
        std::size_t operator()(const SegmentationKey &key) const noexcept;
    };

    struct SegmentationRecoveryState {
        SegmentsImagePointer image;
        QString name;
        QString persistedPath;
        itk::ModifiedTimeType persistedMTime = 0;
        QString recoveryFilePath;
        itk::ModifiedTimeType recoveryMTime = 0;
        bool discardRecoveryWhenWriteFinishes = false;
    };

    struct WriteResult {
        bool success = false;
        QString error;
    };

    enum class RecoveryRemovalReason {
        RegularSave,
        ExplicitDiscard,
        SupersededGeneration
    };

    static Settings loadSettings();
    static void storeSettings(const Settings &settings);
    static SegmentationKey makeKey(const SegmentsImagePointer &image, const void *signal);
    static bool isCoveredByPersistedSave(const SegmentationRecoveryState &state,
                                         itk::ModifiedTimeType mTime);

    void restartTimer();
    void trackSelectedSegmentation();
    SegmentationRecoveryState *selectedSegmentationState();
    const SegmentationRecoveryState *selectedSegmentationState() const;
    static bool hasUnsavedChanges(const SegmentationRecoveryState &state);
    static bool needsRecoveryWrite(const SegmentationRecoveryState &state);
    std::optional<SegmentationKey> nextSegmentationNeedingRecovery() const;
    bool hasRecoveryWork() const;
    bool createRecoveryGenerationPath(const SegmentationRecoveryState &state,
                                      QString &path,
                                      QString &error) const;
    void startRecoveryWrite(const SegmentationKey &key);
    void disableAfterFailure(const QString &error, const QString &partialPath = {});
    bool removeRecoveryIfAllowed(SegmentationRecoveryState &state,
                                 const QString &path,
                                 itk::ModifiedTimeType recoveryMTime,
                                 RecoveryRemovalReason reason);
    void removePersistedRecoveries();

    std::shared_ptr<GraphBase> graphBase_;
    TaskRunner *taskRunner_ = nullptr;
    QPointer<QWidget> messageParent_;
    QTimer timer_;
    Settings settings_;
    std::unordered_map<SegmentationKey, SegmentationRecoveryState, SegmentationKeyHash>
        segmentationStates_;
    SegmentationKey selectedSegmentationKey_;
    SegmentationKey writingSegmentationKey_;
    bool autosavePending_ = false;
    bool recoveryWriteInProgress_ = false;
    bool failureWarningShown_ = false;
};

#endif // SEGMENTPUZZLER_SELECTEDSEGMENTATIONAUTOSAVE_H
