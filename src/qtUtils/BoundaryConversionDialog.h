#ifndef SEGMENTPUZZLER_BOUNDARYCONVERSIONDIALOG_H
#define SEGMENTPUZZLER_BOUNDARYCONVERSIONDIALOG_H

#include <QString>
#include <optional>

class QWidget;

namespace boundary_conversion_dialog {

enum class ConversionMode {
    CastValues,
    ScaleMinMax,
    ScaleZeroToOne
};

std::optional<ConversionMode> askForBoundaryConversionMode(QWidget *parent,
                                                           const QString &windowTitle,
                                                           const QString &text,
                                                           const QString &informativeText);

}

#endif // SEGMENTPUZZLER_BOUNDARYCONVERSIONDIALOG_H
