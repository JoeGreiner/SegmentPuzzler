#include "BoundaryConversionDialog.h"

#include <QMessageBox>
#include <QPushButton>

namespace boundary_conversion_dialog {

std::optional<ConversionMode> askForBoundaryConversionMode(QWidget *parent,
                                                           const QString &windowTitle,
                                                           const QString &text,
                                                           const QString &informativeText) {
    QMessageBox msgBox(parent);
    msgBox.setWindowTitle(windowTitle);
    msgBox.setText(text);
    msgBox.setInformativeText(informativeText);
    QPushButton *scaleZeroToOneButton = msgBox.addButton(QObject::tr("Scale 0..1"), QMessageBox::AcceptRole);
    QPushButton *scaleMinMaxButton = msgBox.addButton(QObject::tr("Scale Min-Max"), QMessageBox::ActionRole);
    QPushButton *castValuesButton = msgBox.addButton(QObject::tr("Cast Values"), QMessageBox::DestructiveRole);
    msgBox.addButton(QMessageBox::Cancel);
    msgBox.exec();

    if (msgBox.clickedButton() == scaleZeroToOneButton) {
        return ConversionMode::ScaleZeroToOne;
    }
    if (msgBox.clickedButton() == scaleMinMaxButton) {
        return ConversionMode::ScaleMinMax;
    }
    if (msgBox.clickedButton() == castValuesButton) {
        return ConversionMode::CastValues;
    }
    return std::nullopt;
}

}
