#include <iostream>

#include "QImageSelectionRadioButtons.h"


QImageSelectionRadioButtons::QImageSelectionRadioButtons(QWidget *parent) :
        QDialog(parent)
{
    grid = new QGridLayout;


    groupBox = new QGroupBox(QObject::tr("Choose what to load:"));

    radioGraph = new QRadioButton(QObject::tr("&Supervoxels"));
    radioImage = new QRadioButton(QObject::tr("&Image"));
    radioBoundary = new QRadioButton(QObject::tr("&Boundaries"));
    radioRefinement = new QRadioButton(QObject::tr("&Refinement"));
    radioSegmentation = new QRadioButton(QObject::tr("&Segmentation"));

    radioGraph->setChecked(true);

    vbox = new QVBoxLayout;
    vbox->addWidget(radioGraph);
    vbox->addWidget(radioImage);
    vbox->addWidget(radioBoundary);
    vbox->addWidget(radioRefinement);
    vbox->addWidget(radioSegmentation);
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
