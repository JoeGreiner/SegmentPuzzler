#include "TaskRunner.h"

#include <QMessageBox>

namespace {
QString describeException(std::exception_ptr error)
{
    if (!error) {
        return QStringLiteral("Unknown error");
    }

    try {
        std::rethrow_exception(error);
    } catch (const std::exception &e) {
        return QString::fromStdString(e.what());
    } catch (...) {
        return QStringLiteral("Unknown error");
    }
}
}

TaskRunner::TaskRunner(QWidget *messageParent, QObject *parent)
    : QObject(parent),
      busy_(false),
      messageParent_(messageParent) {
}

QProgressDialog *TaskRunner::createProgressDialog(const QString &labelText)
{
    auto *progressDialog = new QProgressDialog(messageParent_);
    progressDialog->setWindowTitle(QStringLiteral("Please Wait"));
    progressDialog->setLabelText(labelText);
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setCancelButton(nullptr);
    progressDialog->setRange(0, 0);
    progressDialog->setMinimumDuration(0);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setValue(0);
    progressDialog->show();
    return progressDialog;
}

bool TaskRunner::isBusy() const {
    return busy_;
}

void TaskRunner::setBusy(bool busy) {
    if (busy_ == busy) {
        return;
    }
    busy_ = busy;
    emit busyChanged(busy_);
}

void TaskRunner::handleError(std::exception_ptr error)
{
    QMessageBox::critical(messageParent_,
                          QStringLiteral("Error"),
                          describeException(error));
}
