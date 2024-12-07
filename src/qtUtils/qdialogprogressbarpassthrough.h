#ifndef QDIALOGPROGRESSBARPASSTHROUGH_H
#define QDIALOGPROGRESSBARPASSTHROUGH_H
#include <QProgressDialog>
#include <QKeyEvent>

class QDialogProgressbarPassthrough : public QProgressDialog
{
    Q_OBJECT
public:
    QDialogProgressbarPassthrough(QWidget *parent);

protected:
    void keyReleaseEvent(QKeyEvent *event) override;

};

#endif // QDIALOGPROGRESSBARPASSTHROUGH_H
