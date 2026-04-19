#include "qdialogprogressbarpassthrough.h"
#include "src/utils/AppLogger.h"

QDialogProgressbarPassthrough::QDialogProgressbarPassthrough(QWidget* parent) : QProgressDialog(parent)
{
    this->setParent(parent);
}


void QDialogProgressbarPassthrough::keyReleaseEvent(QKeyEvent *event) {
 if (event->key() == Qt::Key_P) {
    SP_LOG_DEBUG("app", QStringLiteral("Progress dialog received Key_P release"));

 }

}
