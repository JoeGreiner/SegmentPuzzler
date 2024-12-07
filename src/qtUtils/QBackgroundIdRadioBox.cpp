#include <iostream>
#include "QBackgroundIdRadioBox.h"


QBackgroundIdRadioBox::QBackgroundIdRadioBox(QWidget *parent) :
        QDialog(parent)
{
    widgetLayout = new QGridLayout;

    groupBox = new QGroupBox(QObject::tr("Should the highest or the lowest Id be considered as background?"));

    radioHighestIdIsBackground = new QRadioButton(QObject::tr("&Highest Id"));
    radioLowestIdIsBackground = new QRadioButton(QObject::tr("&Lowest Id"));
    radioLowestIdIsBackground->setChecked(true);

    groupBoxLayout = new QVBoxLayout;
    groupBoxLayout->addWidget(radioHighestIdIsBackground);
    groupBoxLayout->addWidget(radioLowestIdIsBackground);
    groupBoxLayout->addStretch(1);
    groupBox->setLayout(groupBoxLayout);

    widgetLayout->addWidget(groupBox);

    evaluateButton = new QPushButton(QObject::tr("Use Selection"));
    connect(evaluateButton, &QPushButton::clicked, this, &QBackgroundIdRadioBox::evaluateSelection);
    widgetLayout->addWidget(evaluateButton);

    this->setLayout(widgetLayout);
}

void QBackgroundIdRadioBox::evaluateSelection() {
    std::cout << "evaluating background selection!" << std::endl;
    bool radioHighestIdIsBackgroundIsChecked = radioHighestIdIsBackground->isChecked();
    bool radioLowestIdIsBackgroundIsChecked = radioLowestIdIsBackground->isChecked();
    if (radioHighestIdIsBackgroundIsChecked) {
        std::cout << "highest" << std::endl;
        emit sendBackgroundIdStrategy(QString("backgroundIsHighestId"));
    } else if (radioLowestIdIsBackgroundIsChecked) {
        std::cout << "lowest" << std::endl;
        emit sendBackgroundIdStrategy(QString("backgroundIsLowestId"));
    }
    this->close();
}
