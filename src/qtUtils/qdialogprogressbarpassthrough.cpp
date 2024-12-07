#include "qdialogprogressbarpassthrough.h"
#include <iostream>

QDialogProgressbarPassthrough::QDialogProgressbarPassthrough(QWidget* parent) : QProgressDialog(parent)
{
    this->setParent(parent);
}


void QDialogProgressbarPassthrough::keyReleaseEvent(QKeyEvent *event) {
 if (event->key() == Qt::Key_P) {
    std::cout << "RELEASED P!!!" << std::endl;

 }

}
