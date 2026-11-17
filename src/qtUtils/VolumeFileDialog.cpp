#include "VolumeFileDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
QString shape(const volume_io::Coordinates &size) {
    return QStringLiteral("%1 × %2 × %3").arg(size[0]).arg(size[1]).arg(size[2]);
}

void addCoordinates(QGridLayout *layout, int row, const QString &label,
                    std::array<QDoubleSpinBox *, 3> &fields, const volume_io::Coordinates &values,
                    std::uint64_t minimum, const QString &prefix) {
    layout->addWidget(new QLabel(label), row, 0);
    for (int axis = 0; axis < 3; ++axis) {
        fields[axis] = new QDoubleSpinBox;
        fields[axis]->setObjectName(prefix + QString::number(axis));
        fields[axis]->setDecimals(0);
        fields[axis]->setRange(minimum, 9007199254740991.0);
        fields[axis]->setValue(values[axis]);
        fields[axis]->setKeyboardTracking(false);
        fields[axis]->setPrefix(QStringLiteral("%1: ").arg(QChar('X' + axis)));
        layout->addWidget(fields[axis], row, axis + 1);
    }
}

volume_io::Coordinates values(const std::array<QDoubleSpinBox *, 3> &fields) {
    return {{static_cast<std::uint64_t>(fields[0]->value()), static_cast<std::uint64_t>(fields[1]->value()),
             static_cast<std::uint64_t>(fields[2]->value())}};
}

void appendOptions(QFileDialog &dialog, QWidget *widget) {
    auto *layout = qobject_cast<QGridLayout *>(dialog.layout());
    if (!layout) throw std::runtime_error("The Qt file dialog has no widget layout.");
    layout->addWidget(widget, layout->rowCount(), 0, 1, layout->columnCount());
}

void selectPath(QFileDialog &dialog, const QString &path) {
    if (path.isEmpty()) return;
    const QFileInfo file(path);
    dialog.setDirectory(file.absolutePath());
    dialog.selectFile(file.fileName());
    // selectFile preserves active filename edits; update the field explicitly.
    if (auto *fileName = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        fileName->setText(file.fileName());
}
}

bool prepareVolumeLoad(QWidget *parent, const QString &path, volume_io::ImageLoadOptions &options) {
    if (!options.crop) return true;
    try {
        const auto info = volume_io::imageInformation(path);
        volume_io::validateRegion(*options.crop, volume_io::dimensions(*info));
        if (!options.allowFullRead && !volume_io::supportsBoundedRead(path, *options.crop)) {
            const long double bytes = static_cast<long double>(volume_io::voxelCount(volume_io::dimensions(*info)))
                * info->GetComponentSize() * info->GetNumberOfComponents();
            if (QMessageBox::warning(parent, QObject::tr("Full-Volume Read Required"),
                    QObject::tr("This file reader cannot load only the requested crop. It needs approximately %1 GiB for the full input, plus conversion buffers.\n\nLoad the full input and keep only the crop?")
                        .arg(static_cast<double>(bytes / (1024 * 1024 * 1024)), 0, 'f', 2),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return false;
            options.allowFullRead = true;
        }
        return true;
    } catch (const std::exception &error) {
        QMessageBox::warning(parent, QObject::tr("Cannot Load Crop"), QString::fromUtf8(error.what()));
        return false;
    }
}

CropOptionsWidget::CropOptionsWidget(const CropLoadDefaults &defaults, QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    enabled = new QCheckBox(tr("Crop"));
    enabled->setObjectName("cropCheckBox");
    enabled->setChecked(defaults.enabled);
    layout->addWidget(enabled);
    details = new QWidget;
    auto *grid = new QGridLayout(details);
    grid->setContentsMargins(0, 0, 0, 0);
    const auto region = defaults.region.value_or(volume_io::CropRegion{});
    initialized = defaults.region.has_value();
    addCoordinates(grid, 0, tr("Start (voxels, zero-based)"), start, region.start, 0, "cropStart");
    addCoordinates(grid, 1, tr("Size (voxels)"), size, region.size, 1, "cropSize");
    summary = new QLabel;
    summary->setObjectName("cropSummary");
    summary->setWordWrap(true);
    grid->addWidget(summary, 2, 0, 1, 4);
    layout->addWidget(details);
    details->setVisible(defaults.enabled);
    connect(enabled, &QCheckBox::toggled, this, [this](bool checked) { details->setVisible(checked); updateSummary(); });
    for (int axis = 0; axis < 3; ++axis) {
        connect(start[axis], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { updateSummary(); });
        connect(size[axis], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { initialized = true; updateSummary(); });
    }
}

void CropOptionsWidget::setFile(const QString &file) {
    path = file;
    originalSize.reset();
    if (QFileInfo(path).isFile()) {
        try {
            originalSize = volume_io::dimensions(*volume_io::imageInformation(path));
            if (!initialized) {
                for (int axis = 0; axis < 3; ++axis) {
                    QSignalBlocker blocker(size[axis]);
                    size[axis]->setValue((*originalSize)[axis]);
                }
            }
        } catch (const std::exception &) {}
    }
    updateSummary();
}

void CropOptionsWidget::updateSummary() {
    QString text = originalSize ? tr("Original: %1\nLoaded: %2").arg(shape(*originalSize), shape(values(size)))
                                : tr("Select a volume to see its dimensions.");
    if (originalSize) {
        try { volume_io::validateRegion({values(start), values(size)}, *originalSize); }
        catch (const std::exception &error) { text += "\n" + QString::fromUtf8(error.what()); }
    }
    summary->setText(text);
}

volume_io::ImageLoadOptions CropOptionsWidget::loadOptions() const {
    volume_io::ImageLoadOptions options;
    if (enabled->isChecked()) {
        options.crop = volume_io::CropRegion{values(start), values(size)};
        volume_io::validateRegion(*options.crop, volume_io::dimensions(*volume_io::imageInformation(path)));
    }
    return options;
}

CropLoadDefaults CropOptionsWidget::defaults() const {
    return {enabled->isChecked(), volume_io::CropRegion{values(start), values(size)}};
}

VolumeLoadDialog::VolumeLoadDialog(QWidget *parent, const QString &title, const QString &directory,
                                 const CropLoadDefaults &defaults) : QFileDialog(parent) {
    setOption(QFileDialog::DontUseNativeDialog);
    setWindowTitle(title);
    setAcceptMode(QFileDialog::AcceptOpen);
    setFileMode(QFileDialog::ExistingFile);
    setDirectory(QFileInfo(directory).isDir() ? directory : QFileInfo(directory).absolutePath());
    crop = new CropOptionsWidget(defaults, this);
    appendOptions(*this, crop);
    connect(this, &QFileDialog::currentChanged, crop, &CropOptionsWidget::setFile);
    // Qt's widget dialog also permits typing a path without selecting a file row.
    if (auto *fileName = findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        connect(fileName, &QLineEdit::textChanged, this, [this] { crop->setFile(selectedFiles().value(0)); });
}

void VolumeLoadDialog::accept() {
    if (selectedFiles().isEmpty()) return;
    try {
        crop->setFile(selectedFiles().front());
        crop->loadOptions();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Cannot Load Crop"), QString::fromUtf8(error.what()));
        return;
    }
    QFileDialog::accept();
}

volume_io::ImageLoadOptions VolumeLoadDialog::loadOptions() const { return crop->loadOptions(); }
CropLoadDefaults VolumeLoadDialog::cropDefaults() const { return crop->defaults(); }

VolumeExportDialog::VolumeExportDialog(QWidget *parent, const QString &suggestedPath,
                                     const volume_io::Coordinates &imageSizeIn,
                                     std::shared_ptr<const volume_io::VolumeContext> contextIn)
    : QFileDialog(parent), normalPath(suggestedPath), imageSize(imageSizeIn), context(std::move(contextIn)) {
    setOption(QFileDialog::DontUseNativeDialog);
    setWindowTitle(tr("Export Selected Segmentation"));
    setAcceptMode(QFileDialog::AcceptSave);
    setFileMode(QFileDialog::AnyFile);
    setNameFilter(tr("Volumes (*.nrrd *.nii *.nii.gz);;All files (*)"));
    selectFile(normalPath);
    auto *options = new QWidget(this);
    auto *layout = new QVBoxLayout(options);
    layout->setContentsMargins(0, 4, 0, 0);
    insert = new QCheckBox(tr("Insert into existing volume"));
    insert->setObjectName("insertRegionCheckBox");
    layout->addWidget(insert);
    details = new QWidget;
    auto *grid = new QGridLayout(details);
    grid->setContentsMargins(0, 0, 0, 0);
    const auto origin = context && context->crop ? context->crop->start : volume_io::Coordinates{};
    addCoordinates(grid, 0, tr("Target start (voxels)"), start, origin, 0, "targetStart");
    policy = new QComboBox;
    policy->setObjectName("targetLabelPolicy");
    policy->addItem(tr("Same segmentation — retain original object IDs"));
    policy->addItem(tr("Independent target — assign new object IDs"));
    policy->setCurrentIndex(1);
    grid->addWidget(new QLabel(tr("Labels")), 1, 0);
    grid->addWidget(policy, 1, 1, 1, 3);
    background = new QLineEdit("0");
    background->setObjectName("targetBackground");
    background->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{1,10}"), background));
    grid->addWidget(new QLabel(tr("Target background ID")), 2, 0);
    grid->addWidget(background, 2, 1, 1, 3);
    layout->addWidget(details);
    details->hide();
    appendOptions(*this, options);
    if (context) insertionPath = context->sourcePath;
    connect(insert, &QCheckBox::toggled, this, &VolumeExportDialog::changeMode);
    connect(this, &QFileDialog::currentChanged, this, [this](const QString &) { updateTarget(); });
    if (auto *fileName = findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        connect(fileName, &QLineEdit::textChanged, this, [this] { updateTarget(); });
    for (auto *field : start)
        connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { updateTarget(); });
    connect(policy, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        background->setText(QString::number(policy->currentIndex() == 0 && context ? context->background.value_or(0) : 0));
    });
}

void VolumeExportDialog::changeMode(bool inserting) {
    const QString selected = selectedFiles().value(0);
    if (inserting) {
        normalPath = selected;
        setAcceptMode(QFileDialog::AcceptOpen);
        setFileMode(QFileDialog::ExistingFile);
        selectPath(*this, insertionPath);
        setLabelText(QFileDialog::Accept, tr("Insert"));
    } else {
        insertionPath = selected;
        setAcceptMode(QFileDialog::AcceptSave);
        setFileMode(QFileDialog::AnyFile);
        selectPath(*this, normalPath);
        setLabelText(QFileDialog::Accept, tr("Save"));
    }
    details->setVisible(inserting);
    updateTarget();
}

void VolumeExportDialog::updateTarget() {
    if (!insertsIntoVolume()) return;
    const QString path = selectedFiles().value(0);
    const auto position = targetStart();
    const auto originalStart = context && context->crop ? context->crop->start : volume_io::Coordinates{};
    const bool same = context && volume_io::canonicalPath(path) == context->sourcePath && position == originalStart;
    policy->setCurrentIndex(same ? 0 : 1);
    background->setText(QString::number(same ? context->background.value_or(0) : 0));
}

bool VolumeExportDialog::insertsIntoVolume() const { return insert->isChecked(); }
volume_io::Coordinates VolumeExportDialog::targetStart() const { return values(start); }

volume_io::RegionWriteOptions VolumeExportDialog::writeOptions() const {
    bool valid = false;
    const auto value = background->text().toULongLong(&valid);
    if (!valid || value > std::numeric_limits<volume_io::Label>::max()) throw std::runtime_error("Invalid target background ID.");
    volume_io::RegionWriteOptions options;
    options.labelPolicy = policy->currentIndex() == 0 ? volume_io::LabelPolicy::SameSegmentation : volume_io::LabelPolicy::Independent;
    options.targetBackground = static_cast<volume_io::Label>(value);
    options.context = context;
    return options;
}

void VolumeExportDialog::accept() {
    if (selectedFiles().isEmpty()) return;
    try {
        if (insertsIntoVolume()) {
            writeOptions();
            volume_io::validateRegion({targetStart(), imageSize}, volume_io::dimensions(*volume_io::imageInformation(selectedFiles().front())));
        } else if (context && context->crop && volume_io::canonicalPath(selectedFiles().front()) == context->sourcePath) {
            throw std::runtime_error("Use 'Insert into existing volume' to update the source. A normal export would replace the full source with only this crop.");
        }
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Cannot Export Volume"), QString::fromUtf8(error.what()));
        return;
    }
    QFileDialog::accept();
}
