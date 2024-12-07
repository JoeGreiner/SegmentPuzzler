#ifndef QTreeWidgetWithDragAndDrop_h
#define QTreeWidgetWithDragAndDrop_h

#include <QTreeWidget>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QWidget>


class QTreeWidgetWithDragAndDrop : public QTreeWidget {
Q_OBJECT
public:
    QTreeWidgetWithDragAndDrop(QTreeWidget *parent = 0);


    void dragEnterEvent(QDragEnterEvent *e) override;

    // seems to be needed that dropevent is fired
    void dragMoveEvent(QDragMoveEvent *e) override;

    void dropEvent(QDropEvent *e) override;

signals:

    void urlDropped(QString url);

};




#endif /* QTreeWidgetWithDragAndDrop_h */
