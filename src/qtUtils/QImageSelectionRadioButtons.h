#ifndef SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
#define SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H

#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include "VolumeFileDialog.h"

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
        bool allowApplyToAll = false,
        const QString &file = {}, const CropLoadDefaults &defaults = {});
    ImageLoadChoice selectedChoice() const;
    bool applyToAll() const;
    volume_io::ImageLoadOptions loadOptions() const;

protected:
    void accept() override;

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
    CropOptionsWidget *cropOptions = nullptr;
};



#endif //SEGMENTCOUPLER_QIMAGESELECTIONRADIOBUTTONS_H
