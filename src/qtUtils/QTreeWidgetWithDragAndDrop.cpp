#include "src/qtUtils/QTreeWidgetWithDragAndDrop.h"

QTreeWidgetWithDragAndDrop::QTreeWidgetWithDragAndDrop(QTreeWidget *parent) {
        setParent(parent);
        setAcceptDrops(true);
};


void QTreeWidgetWithDragAndDrop::dragEnterEvent(QDragEnterEvent *e) {
if (e->mimeData()->hasUrls()) {
e->acceptProposedAction();
}
}

// seems to be needed that dropevent is fired
void QTreeWidgetWithDragAndDrop::dragMoveEvent(QDragMoveEvent *e) {
if (e->mimeData()->hasUrls()) {
e->acceptProposedAction();
}
}

void QTreeWidgetWithDragAndDrop::dropEvent(QDropEvent *e)  {
for (int i = 0; i < e->mimeData()->urls().size(); i++) {
QUrl url = e->mimeData()->urls().at(i);
QString fileName = url.toLocalFile();
emit urlDropped(fileName);
}
}