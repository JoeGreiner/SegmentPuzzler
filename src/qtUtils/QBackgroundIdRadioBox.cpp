#include "QBackgroundIdRadioBox.h"
#include "src/utils/AppLogger.h"


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
    bool radioHighestIdIsBackgroundIsChecked = radioHighestIdIsBackground->isChecked();
    bool radioLowestIdIsBackgroundIsChecked = radioLowestIdIsBackground->isChecked();
    if (radioHighestIdIsBackgroundIsChecked) {
        strategy = "backgroundIsHighestId";
        SP_LOG_INFO("segmentation", QStringLiteral("Background id strategy selected: %1").arg(strategy));
        emit sendBackgroundIdStrategy(strategy);
    } else if (radioLowestIdIsBackgroundIsChecked) {
        strategy = "backgroundIsLowestId";
        SP_LOG_INFO("segmentation", QStringLiteral("Background id strategy selected: %1").arg(strategy));
        emit sendBackgroundIdStrategy(strategy);
    }
    this->close();
}
