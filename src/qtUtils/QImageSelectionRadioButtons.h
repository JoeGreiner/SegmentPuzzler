#ifndef SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
#define SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H


#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QPushButton>

class QImageSelectionRadioButtons : public QDialog {
Q_OBJECT
public:
    explicit QImageSelectionRadioButtons(QString fileNameIn, QWidget *parent= nullptr);

public slots:
    void evaluateButtons();

signals:
    void sendButton(QString fileName, QString choiceOfImage);
    void testSignal();

public:
    QPushButton* evaluateButton;

private:
    QString fileName;
    QGridLayout *grid;
    QGroupBox *groupBox;
    QRadioButton *radioGraph;
    QRadioButton *radioImage;
    QRadioButton *radioBoundary;
    QRadioButton *radioRefinement;
    QRadioButton *radioSegmentation;
    QVBoxLayout *vbox;
};



#endif //SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
