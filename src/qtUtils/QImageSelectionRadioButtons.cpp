#include "QImageSelectionRadioButtons.h"

#include <QSettings>

namespace {

constexpr char kApplyToAllSettingsKey[] =
    "FileLoading/applySelectionToAllRemainingFiles";

}

bool imageLoadChoiceSupportsApplyToAll(ImageLoadChoice choice) {
    return choice == ImageLoadChoice::Image
        || choice == ImageLoadChoice::Segmentation;
}

QImageSelectionRadioButtons::QImageSelectionRadioButtons(
        QWidget *parent,
        bool allowApplyToAllIn) :
        QDialog(parent),
        allowApplyToAll(allowApplyToAllIn)
{
    grid = new QGridLayout;


    groupBox = new QGroupBox(QObject::tr("Choose what to load:"));

    radioGraph = new QRadioButton(QObject::tr("&Supervoxels"));
    radioImage = new QRadioButton(QObject::tr("&Image"));
    radioBoundary = new QRadioButton(QObject::tr("&Boundaries"));
    radioRefinement = new QRadioButton(QObject::tr("&Refinement"));
    radioSegmentation = new QRadioButton(QObject::tr("&Segmentation"));
    applyToAllCheckBox = new QCheckBox(
        QObject::tr("Apply selection to all remaining files"));
    applyToAllCheckBox->setObjectName(QStringLiteral("applyToAllCheckBox"));
    applyToAllCheckBox->setChecked(
        QSettings().value(QString::fromLatin1(kApplyToAllSettingsKey), false).toBool());
    applyToAllCheckBox->setVisible(false);
    connect(applyToAllCheckBox, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QString::fromLatin1(kApplyToAllSettingsKey), checked);
    });
    connect(radioImage, &QRadioButton::toggled,
            this, &QImageSelectionRadioButtons::updateApplyToAllVisibility);
    connect(radioSegmentation, &QRadioButton::toggled,
            this, &QImageSelectionRadioButtons::updateApplyToAllVisibility);

    radioGraph->setChecked(true);

    vbox = new QVBoxLayout;
    vbox->addWidget(radioGraph);
    vbox->addWidget(radioImage);
    vbox->addWidget(radioBoundary);
    vbox->addWidget(radioRefinement);
    vbox->addWidget(radioSegmentation);
    vbox->addWidget(applyToAllCheckBox);
    vbox->addStretch(1);
    groupBox->setLayout(vbox);
    grid->addWidget(groupBox);

    evaluateButton = new QPushButton(QObject::tr("Load"));
    connect(evaluateButton, &QPushButton::released, this, &QDialog::accept);
    grid->addWidget(evaluateButton);

    this->setLayout(grid);
    this->setWindowFlag(Qt::WindowStaysOnTopHint);


}

ImageLoadChoice QImageSelectionRadioButtons::selectedChoice() const {
    if (radioGraph->isChecked()) {
        return ImageLoadChoice::Supervoxels;
    }
    if (radioImage->isChecked()) {
        return ImageLoadChoice::Image;
    }
    if (radioBoundary->isChecked()) {
        return ImageLoadChoice::Boundaries;
    }
    if (radioRefinement->isChecked()) {
        return ImageLoadChoice::Refinement;
    }
    Q_ASSERT(radioSegmentation->isChecked());
    return ImageLoadChoice::Segmentation;
}

bool QImageSelectionRadioButtons::applyToAll() const {
    return applyToAllCheckBox->isChecked();
}

void QImageSelectionRadioButtons::updateApplyToAllVisibility() {
    const bool visible = allowApplyToAll
        && imageLoadChoiceSupportsApplyToAll(selectedChoice());
    applyToAllCheckBox->setVisible(visible);
}
