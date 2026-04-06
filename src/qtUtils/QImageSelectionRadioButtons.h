#ifndef SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
#define SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H

#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QPushButton>
#include <QVBoxLayout>

enum class ImageLoadChoice {
    Supervoxels,
    Image,
    Boundaries,
    Refinement,
    Segmentation
};

class QImageSelectionRadioButtons : public QDialog {
Q_OBJECT
public:
    explicit QImageSelectionRadioButtons(QWidget *parent = nullptr);
    ImageLoadChoice selectedChoice() const;

private:
    QPushButton *evaluateButton;
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
