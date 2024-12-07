#ifndef SEGMENTCOUPLER_QBACKGROUNDIDRADIOBOX_H
#define SEGMENTCOUPLER_QBACKGROUNDIDRADIOBOX_H

#include <QWidget>
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QPushButton>

class QBackgroundIdRadioBox : public QDialog {
    Q_OBJECT
public:
    explicit QBackgroundIdRadioBox(QWidget *parent= nullptr);

public slots:
            void evaluateSelection();

    signals:
            void sendBackgroundIdStrategy(QString backgroundIdStrategyIn);

public:
    QPushButton* evaluateButton;

private:
    QGridLayout *widgetLayout;
    QGroupBox *groupBox;
    QRadioButton *radioHighestIdIsBackground;
    QRadioButton *radioLowestIdIsBackground;
    QVBoxLayout *groupBoxLayout;
};


#endif //SEGMENTCOUPLER_QBACKGROUNDIDRADIOBOX_H
