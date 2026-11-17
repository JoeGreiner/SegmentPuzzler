#include "src/qtUtils/VolumeFileDialog.h"
#include "src/qtUtils/QImageSelectionRadioButtons.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <itkImageFileWriter.h>
#include <iostream>

void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<typename Widget> Widget *field(QObject &parent, const char *name) {
    auto *widget = parent.findChild<Widget *>(name);
    require(widget, name);
    return widget;
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QTemporaryDir files;
    try {
        const auto path = files.filePath("source.nrrd");
        auto image = dataType::SegmentsImageType::New();
        image->SetRegions(dataType::SegmentsImageType::SizeType{{20, 30, 40}});
        image->Allocate(true);
        auto writer = itk::ImageFileWriter<dataType::SegmentsImageType>::New();
        writer->SetInput(image); writer->SetFileName(path.toStdString()); writer->Update();
        CropLoadDefaults defaults{true, volume_io::CropRegion{{{2, 3, 4}}, {{5, 6, 7}}}};
        VolumeLoadDialog load(nullptr, "Load image", files.path(), defaults);
        load.selectFile(path);
        load.show(); application.processEvents();
        auto *crop = field<QCheckBox>(load, "cropCheckBox");
        require(load.testOption(QFileDialog::DontUseNativeDialog) && crop->isChecked(), "Crop default missing");
        require(load.loadOptions().crop->start == defaults.region->start, "Remembered position lost");
        require(field<QLabel>(load, "cropSummary")->text().contains("20"), "Original dimensions missing");
        crop->setChecked(false);
        require(!load.loadOptions().crop, "Unchecked crop still active");
        crop->setChecked(true);
        require(load.loadOptions().crop->size == defaults.region->size, "Unchecking discarded coordinates");
        load.grab().save("/tmp/segmentpuzzler-crop-load.png");
        load.reject();

        QImageSelectionRadioButtons drop(nullptr, true, path, defaults);
        require(drop.loadOptions().crop->start == defaults.region->start, "Drop crop differs from load dialog");
        field<QCheckBox>(drop, "cropCheckBox")->setChecked(false);
        require(!drop.loadOptions().crop, "Drop cannot disable crop");

        auto context = std::make_shared<volume_io::VolumeContext>();
        context->sourcePath = volume_io::canonicalPath(path);
        context->originalSize = {{20, 30, 40}}; context->crop = defaults.region; context->background = 8;
        const auto exportPath = files.filePath("crop-export.nrrd");
        VolumeExportDialog save(nullptr, exportPath, {{5, 6, 7}}, context);
        save.show(); application.processEvents();
        auto *insert = field<QCheckBox>(save, "insertRegionCheckBox");
        require(!insert->isChecked(), "Normal export must be the default");
        insert->setChecked(true); application.processEvents();
        require(save.targetStart() == defaults.region->start, "Target position missing");
        require(volume_io::canonicalPath(save.selectedFiles().value(0)) == volume_io::canonicalPath(path), "Source target not selected");
        require(save.writeOptions().labelPolicy == volume_io::LabelPolicy::SameSegmentation && save.writeOptions().targetBackground == 8,
                "Original target did not default to same segmentation");
        field<QDoubleSpinBox>(save, "targetStart0")->setValue(1);
        require(save.writeOptions().labelPolicy == volume_io::LabelPolicy::Independent && save.writeOptions().targetBackground == 0,
                "Changed placement did not default to independent");
        field<QComboBox>(save, "targetLabelPolicy")->setCurrentIndex(0);
        field<QLineEdit>(save, "targetBackground")->setText("9");
        require(save.writeOptions().labelPolicy == volume_io::LabelPolicy::SameSegmentation && save.writeOptions().targetBackground == 9,
                "Manual label options were discarded");
        save.grab().save("/tmp/segmentpuzzler-crop-save.png");
        insert->setChecked(false); application.processEvents();
        require(volume_io::canonicalPath(save.selectedFiles().value(0)) == volume_io::canonicalPath(exportPath), "Normal export path was not restored");
        insert->setChecked(true);
        field<QDoubleSpinBox>(save, "targetStart0")->setValue(2);
        field<QLineEdit>(save, "fileNameEdit")->setText(files.filePath("different.nrrd"));
        require(save.writeOptions().labelPolicy == volume_io::LabelPolicy::Independent, "Typed target retained original-target defaults");
        save.reject();
        VolumeExportDialog unsourced(nullptr, exportPath, {{2, 2, 2}}, nullptr);
        require(unsourced.targetStart() == volume_io::Coordinates{}, "Missing provenance must default to zero");
        std::cout << "Crop and insertion dialog tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
