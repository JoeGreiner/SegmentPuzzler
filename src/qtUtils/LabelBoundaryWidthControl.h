#ifndef SEGMENTPUZZLER_LABELBOUNDARYWIDTHCONTROL_H
#define SEGMENTPUZZLER_LABELBOUNDARYWIDTHCONTROL_H

#include "src/viewers/itkSignalBase.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <functional>

inline void addLabelBoundaryWidthControl(QVBoxLayout *layout,
                                         itkSignalBase *signal,
                                         const std::function<void()> &onWidthChanged) {
    if (!signal->usesCategoricalColors() ||
        signal->getLabelRenderMode() != itkSignalBase::LabelRenderMode::Boundaries) {
        return;
    }

    auto *row = new QWidget(layout->parentWidget());
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    auto *slider = new QSlider(Qt::Horizontal, row);
    auto *spinBox = new QSpinBox(row);
    slider->setRange(1, itkSignalBase::MaximumLabelBoundaryWidth);
    spinBox->setRange(1, itkSignalBase::MaximumLabelBoundaryWidth);
    spinBox->setSuffix(" px");
    slider->setValue(signal->getLabelBoundaryWidth());
    spinBox->setValue(signal->getLabelBoundaryWidth());
    rowLayout->addWidget(new QLabel("Line width", row));
    rowLayout->addWidget(slider, 1);
    rowLayout->addWidget(spinBox);
    row->setToolTip("Outline width inside each segment, in image pixels. Scales with zoom.");
    layout->addWidget(row);

    QObject::connect(slider, &QSlider::valueChanged, spinBox, &QSpinBox::setValue);
    QObject::connect(spinBox, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    QObject::connect(spinBox, qOverload<int>(&QSpinBox::valueChanged), row,
                     [signal, onWidthChanged](int width) {
                         signal->setLabelBoundaryWidth(width);
                         onWidthChanged();
                     });
}

#endif // SEGMENTPUZZLER_LABELBOUNDARYWIDTHCONTROL_H
