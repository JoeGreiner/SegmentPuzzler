#pragma once

#include "src/viewers/VolumeRegionIO.h"
#include <QFileDialog>
#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QLineEdit;

struct CropLoadDefaults {
    bool enabled = false;
    std::optional<volume_io::CropRegion> region;
};

bool prepareVolumeLoad(QWidget *parent, const QString &path, volume_io::ImageLoadOptions &options);

class CropOptionsWidget : public QWidget {
public:
    explicit CropOptionsWidget(const CropLoadDefaults &defaults, QWidget *parent = nullptr);
    void setFile(const QString &path);
    volume_io::ImageLoadOptions loadOptions() const;
    CropLoadDefaults defaults() const;
private:
    void updateSummary();
    QCheckBox *enabled;
    QWidget *details;
    std::array<QDoubleSpinBox *, 3> start{};
    std::array<QDoubleSpinBox *, 3> size{};
    QLabel *summary;
    QString path;
    std::optional<volume_io::Coordinates> originalSize;
    bool initialized = false;
};

class VolumeLoadDialog : public QFileDialog {
public:
    VolumeLoadDialog(QWidget *parent, const QString &title, const QString &directory,
                     const CropLoadDefaults &defaults);
    volume_io::ImageLoadOptions loadOptions() const;
    CropLoadDefaults cropDefaults() const;
protected:
    void accept() override;
private:
    CropOptionsWidget *crop;
};

class VolumeExportDialog : public QFileDialog {
public:
    VolumeExportDialog(QWidget *parent, const QString &suggestedPath,
                       const volume_io::Coordinates &imageSize,
                       std::shared_ptr<const volume_io::VolumeContext> context);
    bool insertsIntoVolume() const;
    volume_io::Coordinates targetStart() const;
    volume_io::RegionWriteOptions writeOptions() const;
protected:
    void accept() override;
private:
    void changeMode(bool inserting);
    void updateTarget();
    QCheckBox *insert;
    QWidget *details;
    std::array<QDoubleSpinBox *, 3> start{};
    QComboBox *policy;
    QLineEdit *background;
    QString normalPath;
    QString insertionPath;
    volume_io::Coordinates imageSize;
    std::shared_ptr<const volume_io::VolumeContext> context;
};
