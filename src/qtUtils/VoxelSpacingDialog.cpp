#include "VoxelSpacingDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QDoubleSpinBox *createSpacingSpinBox(QWidget *parent, double value) {
    auto *spinBox = new QDoubleSpinBox(parent);
    spinBox->setDecimals(9);
    spinBox->setRange(0.000000001, 1000000000.0);
    spinBox->setValue(value);
    spinBox->setSingleStep(std::max(0.001, value * 0.1));
    spinBox->setAccelerated(true);
    return spinBox;
}

QString formatSpacing(const voxel_geometry::VoxelSpacing &spacing) {
    return QStringLiteral("X: %1   Y: %2   Z: %3")
        .arg(spacing.x, 0, 'g', 12)
        .arg(spacing.y, 0, 'g', 12)
        .arg(spacing.z, 0, 'g', 12);
}

} // namespace

VoxelSpacingDialog::VoxelSpacingDialog(
    const voxel_geometry::VoxelSpacing &currentSpacing,
    QWidget *parent)
    : QDialog(parent), initialSpacing_(currentSpacing) {
    setWindowTitle(tr("Voxel Spacing"));
    setModal(true);

    auto *rootLayout = new QVBoxLayout(this);
    auto *description = new QLabel(
        tr("The fields are initialized from the spacing stored in the currently loaded data. "
           "Changing them updates the dataset metadata and viewer proportions; voxel samples "
           "are not resampled."),
        this);
    description->setWordWrap(true);
    rootLayout->addWidget(description);

    auto *currentLabel = new QLabel(
        tr("Current data: %1").arg(formatSpacing(currentSpacing)), this);
    currentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(currentLabel);

    auto *form = new QFormLayout();
    xSpacing_ = createSpacingSpinBox(this, currentSpacing.x);
    ySpacing_ = createSpacingSpinBox(this, currentSpacing.y);
    zSpacing_ = createSpacingSpinBox(this, currentSpacing.z);
    form->addRow(tr("X spacing:"), xSpacing_);
    form->addRow(tr("Y spacing:"), ySpacing_);
    form->addRow(tr("Z spacing:"), zSpacing_);

    linkXY_ = new QCheckBox(tr("Keep X and Y equal"), this);
    linkXY_->setChecked(voxel_geometry::nearlyEqual(currentSpacing.x, currentSpacing.y));
    form->addRow(QString(), linkXY_);
    rootLayout->addLayout(form);

    connect(xSpacing_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!linkXY_->isChecked()) {
            return;
        }
        const QSignalBlocker blocker(ySpacing_);
        ySpacing_->setValue(value);
    });
    connect(ySpacing_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!linkXY_->isChecked()) {
            return;
        }
        const QSignalBlocker blocker(xSpacing_);
        xSpacing_->setValue(value);
    });
    connect(linkXY_, &QCheckBox::toggled, this, [this](bool linked) {
        if (linked) {
            ySpacing_->setValue(xSpacing_->value());
        }
    });

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset,
        Qt::Horizontal,
        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this]() {
        setSpacing(initialSpacing_);
        linkXY_->setChecked(voxel_geometry::nearlyEqual(initialSpacing_.x, initialSpacing_.y));
    });
    rootLayout->addWidget(buttons);

    resize(460, sizeHint().height());
}

voxel_geometry::VoxelSpacing VoxelSpacingDialog::spacing() const {
    return {xSpacing_->value(), ySpacing_->value(), zSpacing_->value()};
}

void VoxelSpacingDialog::setSpacing(const voxel_geometry::VoxelSpacing &spacing) {
    const QSignalBlocker blockX(xSpacing_);
    const QSignalBlocker blockY(ySpacing_);
    const QSignalBlocker blockZ(zSpacing_);
    xSpacing_->setValue(spacing.x);
    ySpacing_->setValue(spacing.y);
    zSpacing_->setValue(spacing.z);
}
