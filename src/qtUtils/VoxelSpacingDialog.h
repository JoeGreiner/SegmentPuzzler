#ifndef SEGMENTPUZZLER_VOXELSPACINGDIALOG_H
#define SEGMENTPUZZLER_VOXELSPACINGDIALOG_H

#include <QDialog>

#include "src/viewers/VoxelSpacing.h"

class QCheckBox;
class QDoubleSpinBox;

class VoxelSpacingDialog final : public QDialog {
public:
    explicit VoxelSpacingDialog(const voxel_geometry::VoxelSpacing &currentSpacing,
                                QWidget *parent = nullptr);

    voxel_geometry::VoxelSpacing spacing() const;

private:
    void setSpacing(const voxel_geometry::VoxelSpacing &spacing);

    voxel_geometry::VoxelSpacing initialSpacing_;
    QDoubleSpinBox *xSpacing_ = nullptr;
    QDoubleSpinBox *ySpacing_ = nullptr;
    QDoubleSpinBox *zSpacing_ = nullptr;
    QCheckBox *linkXY_ = nullptr;
};

#endif // SEGMENTPUZZLER_VOXELSPACINGDIALOG_H
