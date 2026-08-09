#ifndef SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
#define SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H

#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>

enum class ImageLoadChoice {
    Supervoxels,
    Image,
    Boundaries,
    Refinement,
    Segmentation
};

bool imageLoadChoiceSupportsApplyToAll(ImageLoadChoice choice);

class QImageSelectionRadioButtons : public QDialog {
Q_OBJECT
public:
    explicit QImageSelectionRadioButtons(
        QWidget *parent = nullptr,
        bool allowApplyToAll = false);
    ImageLoadChoice selectedChoice() const;
    bool applyToAll() const;

private:
    void updateApplyToAllVisibility();

    const bool allowApplyToAll;
    QPushButton *evaluateButton;
    QGridLayout *grid;
    QGroupBox *groupBox;
    QRadioButton *radioGraph;
    QRadioButton *radioImage;
    QRadioButton *radioBoundary;
    QRadioButton *radioRefinement;
    QRadioButton *radioSegmentation;
    QCheckBox *applyToAllCheckBox;
    QVBoxLayout *vbox;
};



#endif //SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
