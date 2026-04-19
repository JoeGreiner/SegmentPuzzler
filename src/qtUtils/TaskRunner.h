#ifndef SEGMENTPUZZLER_TASKRUNNER_H
#define SEGMENTPUZZLER_TASKRUNNER_H

#include <QFutureWatcher>
#include <QProgressDialog>
#include <QPointer>
#include <QString>
#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include <QWidget>

#include <exception>
#include <functional>
#include <memory>
#include <utility>
#include <stdexcept>
#include <type_traits>

#include "src/utils/AppLogger.h"

namespace task_runner_detail {
template<typename Result>
struct TaskOutcome {
    std::exception_ptr error;
    std::shared_ptr<Result> value;
};

template<>
struct TaskOutcome<void> {
    std::exception_ptr error;
};
}

// Runs one background task at a time.
//
// Usage:
//   taskRunner->run(
//       [=]() { return loadImage(path); },          // runs on worker thread
//       [=](Image result) { showInViewer(result); }  // runs on GUI thread
//   );
//   taskRunner->runWithLabel(
//       "Loading image...",
//       [=]() { return loadImage(path); },
//       [=](Image result) { showInViewer(result); }
//   );
//
// Sequence:
//   GUI thread              Worker thread
//       |
//       +-- run() called
//       |     busyChanged(true)
//       |         |
//       |         +---------> compute() runs
//       |                         |
//       |   <---------------------+ result returned
//       |
//       +-- commit(result) runs
//       |     busyChanged(false)
//       |
//
// If compute() throws, commit() is skipped and a QMessageBox is shown.
//
// afterIdle (optional third argument) runs on the GUI thread after
// busyChanged(false). It is called whether the task succeeded or failed,
// so it is safe for chaining sequential tasks.
//
// Both run() and runWithLabel() show a window-modal indeterminate progress
// dialog for the owning window while the task is running.
//
// Thread safety: compute lambdas run off the GUI thread. They must not
// touch QWidgets. Accessing GraphBase members from compute is currently
// tolerated only because one task runs at a time and the owning window is
// blocked while the task runs. If parallel tasks are ever allowed, those
// accesses become data races.
class TaskRunner : public QObject {
    Q_OBJECT
public:
    explicit TaskRunner(QWidget *messageParent = nullptr, QObject *parent = nullptr);

    template<typename Compute, typename Commit>
    void run(Compute compute,
             Commit commit,
             std::function<void()> afterIdle = {});

    template<typename Compute, typename Commit>
    void runWithLabel(QString labelText,
                      Compute compute,
                      Commit commit,
                      std::function<void()> afterIdle = {});

    bool isBusy() const;

signals:
    void busyChanged(bool busy);

private:
    template<typename Compute, typename Commit>
    void runImpl(QString labelText,
                 Compute compute,
                 Commit commit,
                 std::function<void()> afterIdle);

    QProgressDialog *createProgressDialog(const QString &labelText);
    void setBusy(bool busy);
    void handleError(std::exception_ptr error, const QString &context = {});

    bool busy_;
    QPointer<QWidget> messageParent_;
};

template<typename Compute, typename Commit>
void TaskRunner::run(Compute compute,
                     Commit commit,
                     std::function<void()> afterIdle)
{
    runImpl(QStringLiteral("Working..."),
            std::move(compute),
            std::move(commit),
            std::move(afterIdle));
}

template<typename Compute, typename Commit>
void TaskRunner::runWithLabel(QString labelText,
                              Compute compute,
                              Commit commit,
                              std::function<void()> afterIdle)
{
    runImpl(std::move(labelText),
            std::move(compute),
            std::move(commit),
            std::move(afterIdle));
}

template<typename Compute, typename Commit>
void TaskRunner::runImpl(QString labelText,
                         Compute compute,
                         Commit commit,
                         std::function<void()> afterIdle)
{
    using Result = std::invoke_result_t<Compute>;
    using StoredResult = std::decay_t<Result>;
    using Outcome = std::conditional_t<std::is_void_v<Result>,
                                       task_runner_detail::TaskOutcome<void>,
                                       task_runner_detail::TaskOutcome<StoredResult>>;

    Q_ASSERT(!busy_);
    if (busy_) {
        throw std::logic_error("TaskRunner is already busy");
    }

    setBusy(true);

    if (labelText.isEmpty()) {
        labelText = QStringLiteral("Working...");
    }

    const QString taskLabel = labelText;
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    SP_LOG_INFO("tasks", QStringLiteral("Task started: %1").arg(taskLabel));

    QPointer<QProgressDialog> progressDialog = createProgressDialog(labelText);

    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this,
            [this,
             watcher,
             progressDialog,
             taskLabel,
             startedAtMs,
             commit = std::move(commit),
             afterIdle = std::move(afterIdle)]() mutable {
                Outcome outcome;
                std::exception_ptr callbackError;
                bool taskSucceeded = false;
                try {
                    outcome = watcher->future().result();
                } catch (...) {
                    callbackError = std::current_exception();
                }

                try {
                    if (callbackError) {
                        handleError(callbackError, taskLabel);
                    } else if (outcome.error) {
                        handleError(outcome.error, taskLabel);
                    } else {
                        if constexpr (std::is_void_v<Result>) {
                            commit();
                        } else {
                            commit(std::move(*outcome.value));
                        }
                        taskSucceeded = true;
                    }
                } catch (...) {
                    try {
                        handleError(std::current_exception(), taskLabel);
                    } catch (...) {
                    }
                }

                if (progressDialog) {
                    progressDialog->close();
                    progressDialog->deleteLater();
                }
                watcher->deleteLater();
                setBusy(false);
                if (taskSucceeded) {
                    SP_LOG_INFO("tasks",
                                QStringLiteral("Task finished: %1 (%2 ms)")
                                    .arg(taskLabel)
                                    .arg(QDateTime::currentMSecsSinceEpoch() - startedAtMs));
                }

                // afterIdle runs after setBusy(false), so isBusy() is
                // briefly false between chained tasks. This is fine as
                // long as the GUI only reacts to busyChanged via queued
                // signal/slot connections (which is the default).
                if (afterIdle) {
                    try {
                        afterIdle();
                    } catch (...) {
                        try {
                            handleError(std::current_exception(), taskLabel);
                        } catch (...) {
                        }
                    }
                }
            });

    watcher->setFuture(QtConcurrent::run([compute = std::move(compute)]() mutable -> Outcome {
        Outcome outcome;
        try {
            if constexpr (std::is_void_v<Result>) {
                compute();
            } else {
                outcome.value = std::make_shared<StoredResult>(compute());
            }
        } catch (...) {
            outcome.error = std::current_exception();
        }
        return outcome;
    }));
}

#endif // SEGMENTPUZZLER_TASKRUNNER_H
