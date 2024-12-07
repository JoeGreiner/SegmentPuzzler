#include <iostream>

#include "QImageSelectionRadioButtons.h"


QImageSelectionRadioButtons::QImageSelectionRadioButtons(QString fileNameIn, QWidget *parent) :
        fileName(fileNameIn), QDialog(parent)
{
    grid = new QGridLayout;


    groupBox = new QGroupBox(QObject::tr("Please choose the type of file you want to load:"));

    radioGraph = new QRadioButton(QObject::tr("&Graph/Segments"));
    radioImage = new QRadioButton(QObject::tr("&Image"));
    radioBoundary = new QRadioButton(QObject::tr("&Boundary"));
    radioRefinement = new QRadioButton(QObject::tr("&Refinement Watershed"));
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

    evaluateButton = new QPushButton(QObject::tr("Load file"));
    connect(evaluateButton, &QPushButton::released, this, &QImageSelectionRadioButtons::evaluateButtons);
    grid->addWidget(evaluateButton);

    this->setLayout(grid);
    this->setWindowFlag(Qt::WindowStaysOnTopHint);


}

void QImageSelectionRadioButtons::evaluateButtons() {
    bool radioGraphclicked = radioGraph->isChecked();
    bool radioImageClicked = radioImage->isChecked();
    bool radioBoundaryClicked = radioBoundary->isChecked();
    bool radioRefinementClicked = radioRefinement->isChecked();
    bool radioSegmentationClicked = radioSegmentation->isChecked();
    if (radioGraphclicked) {
        emit sendButton(fileName, "Segments");
    } else if (radioImageClicked) {
        emit sendButton(fileName, "Image");
    } else if (radioBoundaryClicked) {
        emit sendButton(fileName, "Boundary");
    } else if (radioRefinementClicked) {
        emit sendButton(fileName, "Refinement Watershed");
    } else if (radioSegmentationClicked) {
        emit sendButton(fileName, "Segmentation");
    }
    this->close();
}
