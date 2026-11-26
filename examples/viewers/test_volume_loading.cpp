#include "src/controllers/SignalControl.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SelectedSegmentationAutosave.h"
#include "src/viewers/OrthoViewer.h"
#include "src/utils/StartupVolumes.h"
#include <itkImageFileWriter.h>
#include <itkRGBPixel.h>
#include <QApplication>
#include <QEventLoop>
#include <QFileDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <iostream>
#include <limits>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void testStartupVolumeArguments() {
    const auto volumes = parseStartupVolumes({
        "--segmentation", "cells.tif", "Cell labels", "--view", "outline", "--hidden",
        "--image", "raw.tif", "-pre-treatment", "--opacity", "0", "--segmentation", "reference.tif"});
    require(volumes.size() == 3 && volumes[0].path == "raw.tif" && volumes[0].name == "-pre-treatment" &&
            volumes[0].display.opacityPercent == 0 && !volumes[0].display.visible.has_value(),
            "Image options or startup order changed");
    require(volumes[1].name == "Cell labels" && volumes[1].display.visible == false &&
            volumes[1].display.view == itkSignalBase::LabelRenderMode::Boundaries &&
            !volumes[2].display.view && !volumes[2].display.visible.has_value(),
            "Layer modifiers leaked between declarations");

    const auto ordered = parseStartupVolumes({"--refinement", "r",
        "--boundary", "b", "--segmentation", "s",
        "--image", "-scan.tif", "--segments", "v"});
    QStringList paths;
    for (const auto &volume : ordered) paths.append(volume.path);
    require(paths == QStringList{"v", "-scan.tif", "s", "b", "r"}, "Wrong startup dependency order");

    for (const QString &unexpected : {QString("--opactiy"), QString("stray")}) {
        try {
            parseStartupVolumes({"--image", "raw", "Name", unexpected, "--opacity", "50"});
        } catch (const std::invalid_argument &error) {
            require(QString::fromStdString(error.what()).contains(unexpected), "Wrong argument in diagnostic");
            continue;
        }
        throw std::runtime_error("Unexpected argument was ignored");
    }

    const auto styled = parseStartupVolumes({
        "--image", "first", "--color", "#4080ff", "--range", "-2.5", "9e2",
        "--boundary", "second", "--color", "64", "128", "255", "--blend", "over",
        "--segmentation", "third", "--outline-width", "3"});
    require(styled[0].display.color == QColor(64, 128, 255) &&
            styled[0].display.color == styled[2].display.color &&
            styled[0].display.range && styled[0].display.range->lower == -2.5 &&
            styled[0].display.range->upper == 900 &&
            styled[1].display.outlineWidth == 3 && !styled[1].display.view &&
            styled[2].display.blend == itkSignalBase::BlendMode::SourceOver,
            "Layer style arguments were parsed incorrectly");

    for (const QStringList &args : std::vector<QStringList>{
             {"--hidden"}, {"--image", "raw", "--view", "outline"}, {"--image", "--visible"},
             {"--image", "raw", "--opacity", "101"}, {"--image", "raw", "--opacity"},
             {"--refinement", "mask", "--visible", "--hidden"},
             {"--image", "raw", "--color", "#abc"}, {"--image", "raw", "--color", "#zz0011"},
             {"--image", "raw", "--color", "256", "0", "0"},
             {"--image", "raw", "--color", "10", "20", "--image", "next"},
             {"--image", "raw", "--color", "1", "2", "3", "4"},
             {"--image", "raw", "--range", "0", "2", "-1"},
             {"--image", "raw", "--color", "#123456", "--color", "1", "2", "3"},
             {"--image", "raw", "--range", "nan", "1"}, {"--image", "raw", "--range", "1", "inf"},
             {"--boundary", "map", "--range", "1", "0"}, {"--image", "raw", "--blend", "normal"},
             {"--segmentation", "mask", "--range", "0", "1"}, {"--segments", "mask", "--color", "#ffffff"},
             {"--refinement", "mask", "--blend", "over"}, {"--image", "raw", "--outline-width", "2"},
             {"--segmentation", "mask", "--outline-width", "11"}}) {
        try {
            parseStartupVolumes(args);
        } catch (const std::invalid_argument &) {
            continue;
        }
        throw std::runtime_error(("Accepted invalid arguments: " + args.join(' ')).toStdString());
    }
}

void testSegmentationAsSupervoxels(const QString &directory) {
    auto image = dataType::SegmentsImageType::New();
    image->SetRegions(dataType::SegmentsImageType::SizeType{{8, 6, 35}});
    image->Allocate(true);
    for (int z = 0; z < 35; ++z) for (int y = 1; y < 5; ++y) for (int x = 1; x < 7; ++x)
        image->SetPixel({x, y, z}, x < 4 ? 1 : 2);
    const auto path = directory + "/duplicate-segmentation.nrrd";
    auto writer = itk::ImageFileWriter<dataType::SegmentsImageType>::New();
    writer->SetInput(image); writer->SetFileName(path.toStdString()); writer->Update();
    auto base = std::make_shared<GraphBase>();
    Graph graph(base, false); base->pGraph = &graph;
    TaskRunner runner; OrthoViewer viewer(base, &runner);
    SignalControl control(base, &viewer, &runner, nullptr, false);
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &control, [] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) message->button(QMessageBox::Yes)->click();
        else if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->accept();
    });
    dialogs.start(10);
    QEventLoop loop;
    bool loaded = false;
    control.loadSegmentationVolume(path, {}, [&](auto result) { loaded = result.has_value(); loop.quit(); },
                                   {}, {false, 0, itkSignalBase::LabelRenderMode::Boundaries});
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec(); dialogs.stop();
    require(loaded && graph.initialNodes.size() == 2, "Duplicating segmentation as supervoxels failed");
    require(!base->pSelectedSegmentationSignal->getIsActive() && base->pSelectedSegmentationSignal->getAlpha() == 0 &&
            base->pSelectedSegmentationSignal->getLabelRenderMode() == itkSignalBase::LabelRenderMode::Boundaries &&
            base->pWorkingSegments->getIsActive(), "Segmentation options did not target the requested layer");
    const auto region = image->GetBufferedRegion();
    for (const auto storage : {base->pWorkingSegmentsImage, base->pSelectedSegmentation}) {
        const auto actual = storage->denseRegion(region);
        require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + region.GetNumberOfPixels(),
                           actual->GetBufferPointer()), "Duplicating segmentation changed voxels");
    }
    base->pSelectedSegmentation->SetPixel({1, 1, 1}, 9);
    require(base->pWorkingSegmentsImage->GetPixel({1, 1, 1}) == 1, "Duplicated volumes share mutable storage");
    control.loadRefinementAsync(path, {}, {}, {}, {true, 80, itkSignalBase::LabelRenderMode::Filled});
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (!runner.isBusy()) loop.quit(); });
    poll.start(10); loop.exec(); poll.stop();
    require(base->pSelectedRefinement && base->pSelectedRefinement->GetPixel({1, 1, 1}) == 1,
            "Streaming refinement load failed");
    require(base->pSelectedRefinementSignal->getIsActive() && base->pSelectedRefinementSignal->getAlpha() == 204,
            "Refinement defaults overrode the requested visibility or opacity");
    for (auto *signal : control.allSignalList) viewer.removeSignal(signal);
}

void testMixedCrops(const QString &directory) {
    using Image = dataType::SegmentsImageType;
    auto input = Image::New();
    input->SetRegions(Image::SizeType{{10, 8, 6}}); input->Allocate(); input->FillBuffer(4000000000U);
    input->SetPixel({{0, 0, 0}}, 0);
    input->SetPixel({{3, 3, 3}}, 8);
    const auto path = directory + "/sparse-labels.nrrd";
    auto writer = itk::ImageFileWriter<Image>::New();
    writer->SetInput(input); writer->SetFileName(path.toStdString()); writer->Update();
    auto base = std::make_shared<GraphBase>();
    Graph graph(base, false); base->pGraph = &graph;
    graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
    TaskRunner runner; OrthoViewer viewer(base, &runner);
    SignalControl control(base, &viewer, &runner, nullptr, false);
    SelectedSegmentationAutosave autosave(base, &runner);
    QObject::connect(&control, &SignalControl::selectedSegmentationChanged,
                     &autosave, &SelectedSegmentationAutosave::selectedSegmentationChanged);
    auto awaitLoad = [&](auto start, bool expectSuccess = true) {
        QEventLoop loop;
        bool complete = false;
        SignalControl::LoadResult result;
        QTimer deadline; deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        start([&](SignalControl::LoadResult loaded) { result = loaded; complete = true; loop.quit(); });
        deadline.start(30000); loop.exec();
        require(complete && result.has_value() == expectSuccess && !runner.isBusy(), "Mixed crop load failed");
    };
    volume_io::ImageLoadOptions options;
    options.crop = volume_io::CropRegion{{{2, 2, 2}}, {{4, 3, 2}}};
    const auto outline = itkSignalBase::LabelRenderMode::Boundaries;
    LayerDisplayOptions supervoxels;
    supervoxels.visible = false;
    supervoxels.opacityPercent = 40;
    supervoxels.outlineWidth = 4;
    awaitLoad([&](auto done) { control.addSegmentsGraphAsync(path, done, options, supervoxels, "Nuclei"); });
    require(base->pWorkingSegments->name == "Nuclei", "Supervoxel display name was lost");
    require(base->pWorkingSegments->getLabelRenderMode() == itkSignalBase::LabelRenderMode::Filled,
            "Outline width unexpectedly enabled outlines");
    base->pWorkingSegments->setLabelRenderMode(outline);
    require(!base->pWorkingSegments->getIsActive() && base->pWorkingSegments->getAlpha() == 102 &&
            base->pWorkingSegments->getLabelBoundaryWidth() == 4, "Supervoxel display options were lost");
    require(base->pSelectedSegmentationSignal->getIsActive() &&
            base->pSelectedSegmentationSignal->getLabelRenderMode() != outline &&
            base->pEdgesInitialSegmentsITKSignal->getLabelRenderMode() != outline,
            "Supervoxel options leaked into generated layers");
    require(base->pWorkingSegmentsImage->GetBufferedRegion().GetNumberOfPixels() == 24 &&
            graph.initialNodes.size() == 2 && graph.backgroundId == 0,
            "Sparse high IDs or source background failed");
    require(base->pWorkingSegments->volumeContext->crop->start == options.crop->start,
            "Working graph lost provenance");
    require(base->pSelectedSegmentationSignal->volumeContext->crop->start == options.crop->start &&
            base->pSelectedSegmentationSignal->volumeContext->originalLabels.empty(),
            "Derived segmentation inherited object identities");
    require(!autosave.hasUnsavedSegmentations(), "Empty initial segmentation needs recovery");
    volume_io::useLabel(*base->selectedSegmentationContext, 100);
    base->pSelectedSegmentation->FillBuffer(100);
    require(autosave.hasUnsavedSegmentations(), "Edited initial segmentation skipped recovery");
    const auto first = graph.reserveSelectedLabelIds(2);
    base->pSelectedSegmentation->FillBuffer(0);
    require(first == 101 && graph.reserveSelectedLabelIds(1) == 103, "Deleted local IDs were recycled");

    input->SetRegions(Image::SizeType{{4, 3, 2}}); input->Allocate(); input->FillBuffer(7);
    const auto external = directory + "/external-crop.nrrd";
    writer->SetFileName(external.toStdString()); writer->Update();
    using FloatImage = itk::Image<float, 3>;
    auto intensity = FloatImage::New();
    intensity->SetRegions(FloatImage::SizeType{{4, 3, 2}});
    intensity->Allocate(true);
    intensity->SetPixel({0, 0, 0}, std::numeric_limits<float>::infinity());
    intensity->SetPixel({1, 0, 0}, -std::numeric_limits<float>::infinity());
    const auto intensityPath = directory + "/intensity.nrrd";
    auto intensityWriter = itk::ImageFileWriter<FloatImage>::New();
    intensityWriter->SetFileName(intensityPath.toStdString());
    intensityWriter->SetInput(intensity);
    intensityWriter->Update();
    const auto tinted = parseStartupVolumes({"--image", intensityPath, "--color", "64", "128", "255",
                                             "--blend", "over", "--range", "4.96875", "67.0459"}).front().display;
    awaitLoad([&](auto done) { control.addImageAsync(intensityPath, "Tinted", done, {}, tinted); });
    auto *scalar = control.allSignalList.back();
    require(scalar->getColor() == qRgb(64, 128, 255) && scalar->getNormLower() == 4.96875 &&
            scalar->getNormUpper() == 67.0459 && scalar->getBlendMode() == itkSignalBase::BlendMode::SourceOver,
            "Scalar display overrides were lost");
    bool foundRange = false;
    for (auto *button : control.signalTreeWidget->findChildren<QToolButton *>()) {
        if (button->text() == "5..67")
            foundRange = true;
    }
    require(foundRange, "Layer list did not show rounded contrast bounds");
    control.setLocale(QLocale(QLocale::German));
    scalar->setNorm(0.25, 0.75);
    auto *imageTree = control.signalTreeWidget;
    control.setUserNorm(imageTree->topLevelItem(imageTree->topLevelItemCount() - 1));
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    require(menu != nullptr, "Contrast popup did not open");
    auto *lower = menu->findChild<QLineEdit *>("contrastLower");
    auto *upper = menu->findChild<QLineEdit *>("contrastUpper");
    require(lower && upper && lower->text() == "0,25" && upper->text() == "0,75",
            "Contrast fields lost fractional bounds");
    upper->setText("2,5");
    QMetaObject::invokeMethod(upper, "editingFinished");
    require(scalar->getNormLower() == 0.25 && scalar->getNormUpper() == 2.5,
            "Editing the upper contrast limit changed the lower limit");
    lower->setText("-1e-20");
    QMetaObject::invokeMethod(lower, "editingFinished");
    require(scalar->getNormLower() == -1e-20 && scalar->getNormUpper() == 2.5,
            "Editing the lower contrast limit lost precision");
    for (auto *button : menu->findChildren<QPushButton *>())
        if (button->text() == "Reset") button->click();
    require(scalar->getNormLower() == -1e-20 && scalar->getNormUpper() == 2.5,
            "Reset replaced a valid contrast range with non-finite data extrema");
    menu->close();
    awaitLoad([&](auto done) { control.addImageAsync(external, "External crop", done); });
    require(control.allSignalList.back()->getColor() == qRgb(0, 255, 102),
            "Explicit colour consumed a default palette slot");
    awaitLoad([&](auto done) { control.addImageAsync(intensityPath, "No range", done); });
    auto *unbounded = control.allSignalList.back();
    control.setUserNorm(imageTree->topLevelItem(imageTree->topLevelItemCount() - 1));
    menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    require(menu != nullptr, "Missing contrast popup for non-finite extrema");
    lower = menu->findChild<QLineEdit *>("contrastLower");
    require(lower != nullptr, "Missing lower contrast field");
    lower->setText("0,5");
    QMetaObject::invokeMethod(lower, "editingFinished");
    require(unbounded->getNormLower() == 0.5 && unbounded->getNormUpper() == 0.5,
            "Cannot recover a contrast range with non-finite extrema");
    menu->close();
    using RgbImage = itk::Image<itk::RGBPixel<unsigned char>, 3>;
    auto rgb = RgbImage::New();
    rgb->SetRegions(RgbImage::SizeType{{4, 3, 2}});
    rgb->Allocate(true);
    const auto rgbPath = directory + "/channels.tif";
    auto rgbWriter = itk::ImageFileWriter<RgbImage>::New();
    rgbWriter->SetFileName(rgbPath.toStdString());
    rgbWriter->SetInput(rgb);
    RgbImage::SpacingType spacing;
    spacing.Fill(2.0);
    rgb->SetSpacing(spacing);
    rgbWriter->Update();
    const auto firstChannel = control.allSignalList.size();
    bool promptedForSkippedImage = false;
    QTimer::singleShot(0, &control, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            promptedForSkippedImage = true;
            dialog->reject();
        }
    });
    awaitLoad([&](auto done) { control.addImageAsync(rgbPath, {}, done, {}, tinted); }, false);
    require(!promptedForSkippedImage, "Rejected RGB tint opened a spacing dialog");
    spacing.Fill(1.0);
    rgb->SetSpacing(spacing);
    rgbWriter->Update();
    require(control.allSignalList.size() == firstChannel, "Rejected RGB tint inserted channels");
    const auto channels = parseStartupVolumes({"--image", rgbPath, "--range", "-2", "0.75",
        "--hidden", "--opacity", "60", "--blend", "over"}).front().display;
    awaitLoad([&](auto done) { control.addImageAsync(rgbPath, {}, done, {}, channels); });
    require(control.allSignalList.size() == firstChannel + 3, "Expected three RGB channels");
    const QRgb rgbColors[] = {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255)};
    for (size_t i = 0; i < 3; ++i) {
        auto *channel = control.allSignalList[firstChannel + i];
        require(!channel->getIsActive() && channel->getAlpha() == 153 && channel->getColor() == rgbColors[i] &&
                channel->getNormLower() == -2 && channel->getNormUpper() == 0.75 &&
                channel->getBlendMode() == itkSignalBase::BlendMode::SourceOver, "Incorrect RGB channel display");
    }
    awaitLoad([&](auto done) { control.loadMembraneProbabilityAsync(path, "Other origin",
        SignalControl::BoundaryLoadMode::BoundaryOnly, SignalControl::FloatBoundaryConversionMode::CastValues, done,
        volume_io::ImageLoadOptions{volume_io::CropRegion{{{0, 0, 0}}, {{4, 3, 2}}}}, tinted); });
    require(base->pSelectedBoundarySignal->getColor() == qRgb(64, 128, 255) &&
            base->pSelectedBoundarySignal->getNormLower() == 4.96875 &&
            base->pSelectedBoundarySignal->getBlendMode() == itkSignalBase::BlendMode::SourceOver,
            "Boundary display overrides were lost");
    require(base->pSelectedBoundary->GetBufferedRegion().GetNumberOfPixels() == 24,
            "Matching loaded dimensions were rejected");
    const auto beforeDrop = control.allSignalList.size();
    int choices = 0;
    bool rememberedCrop = false;
    QEventLoop dropLoop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &dropLoop, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QImageSelectionRadioButtons *>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            ++choices;
            rememberedCrop = dialog->findChild<QCheckBox *>("cropCheckBox")->isChecked();
            for (auto *radio : dialog->findChildren<QRadioButton *>())
                if (radio->text() == "&Image") radio->setChecked(true);
            dialog->findChild<QCheckBox *>("applyToAllCheckBox")->setChecked(true);
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->text() == "Load") button->click();
        }
        if (!runner.isBusy() && control.allSignalList.size() == beforeDrop + 2) dropLoop.quit();
    });
    QTimer dropDeadline;
    dropDeadline.setSingleShot(true);
    QObject::connect(&dropDeadline, &QTimer::timeout, &dropLoop, &QEventLoop::quit);
    poll.start(10); dropDeadline.start(30000);
    control.handleDroppedFiles({path, path});
    dropLoop.exec(); poll.stop();
    require(choices == 1 && rememberedCrop && control.allSignalList.size() == beforeDrop + 2,
            "Drop batch did not remember and apply the crop to remaining files");
    for (std::size_t index = beforeDrop; index < control.allSignalList.size(); ++index)
        require(control.allSignalList[index]->volumeContext->crop->size == options.crop->size,
                "Drop batch loaded different crop dimensions");

    std::vector<QTreeWidgetItem *> items;
    std::vector<std::shared_ptr<volume_io::VolumeContext>> contexts;
    for (const auto label : {20U, 40U}) {
        input->FillBuffer(0);
        input->SetPixel({{1, 1, 1}}, label);
        const auto segmentationPath = directory + QString("/layer-%1.nrrd").arg(label);
        writer->SetFileName(segmentationPath.toStdString()); writer->Update();
        awaitLoad([&](auto done) { control.loadSegmentationVolume(segmentationPath, {}, done); });
        auto *item = control.segmentationTreeWidget->topLevelItem(control.segmentationTreeWidget->topLevelItemCount() - 1);
        control.segmentationTreeWidget->setCurrentItem(item);
        control.segmentationClicked(item, 0);
        items.push_back(item);
        contexts.push_back(base->pSelectedSegmentationSignal->volumeContext);
        require(base->selectedSegmentationContext == contexts.back() && graph.reserveSelectedLabelIds(1) == label + 1,
                "Selection did not bind the loaded layer context");
    }
    control.segmentationTreeWidget->setCurrentItem(items.front());
    control.segmentationClicked(items.front(), 0);
    require(base->selectedSegmentationContext == contexts.front() && graph.reserveSelectedLabelIds(1) == 22 &&
            contexts.back()->nextLabel == 42, "Switching layers lost or shared the allocation cursor");

    auto exportTo = [&](const QString &destination) {
        QTimer dialogs;
        QObject::connect(&dialogs, &QTimer::timeout, &control, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialog->setOption(QFileDialog::DontConfirmOverwrite);
                dialog->selectFile(destination);
                auto *fileName = dialog->findChild<QLineEdit *>("fileNameEdit");
                fileName->setFocus();
                fileName->setText(destination);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            } else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                message->accept();
            }
        });
        dialogs.start(10);
        control.exportSelectedSegmentation();
        if (runner.isBusy()) {
            QEventLoop exportLoop;
            QObject::connect(&runner, &TaskRunner::busyChanged, &exportLoop, [&](bool busy) { if (!busy) exportLoop.quit(); });
            QTimer::singleShot(30000, &exportLoop, &QEventLoop::quit);
            exportLoop.exec();
            require(!runner.isBusy(), "Export did not finish");
        }
    };
    const auto beforeExport = volume_io::fileFingerprint(external);
    volume_io::TargetBinding target;
    target.path = volume_io::canonicalPath(external);
    contexts.back()->targets.push_back(target);
    exportTo(external);
    require(volume_io::fileFingerprint(external) == beforeExport,
            "Normal export overwrote another layer's insertion target");

    const auto exportedPath = directory + "/export.nrrd";
    volume_io::setImageContext(base->pSelectedSegmentation->GetMetaDataDictionary(), *contexts.front());
    exportTo(exportedPath);
    auto exported = volume_io::readImage<volume_io::Label>(exportedPath, {}).image;
    require(!volume_io::imageContext(volume_io::imageInformation(exportedPath)->GetMetaDataDictionary()),
            "Normal export retained recovery provenance");
    require(exported->GetBufferedRegion() == base->pSelectedSegmentation->GetBufferedRegion() &&
            exported->GetOrigin() == base->pSelectedSegmentation->GetOrigin() &&
            exported->GetSpacing() == base->pSelectedSegmentation->GetSpacing() &&
            exported->GetDirection() == base->pSelectedSegmentation->GetDirection(),
            "Normal export changed crop geometry");
    require(std::equal(exported->GetBufferPointer(), exported->GetBufferPointer() +
            exported->GetBufferedRegion().GetNumberOfPixels(), base->pSelectedSegmentation->denseRegion(base->pSelectedSegmentation->GetLargestPossibleRegion())->GetBufferPointer()),
            "Normal export changed crop pixels");
    for (auto *signal : control.allSignalList) viewer.removeSignal(signal);
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("SegmentPuzzlerTests");
    QCoreApplication::setApplicationName("VolumeLoading");
    QTemporaryDir files;
    if (!files.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, files.path());
    try {
        testStartupVolumeArguments();
        testSegmentationAsSupervoxels(files.path());
        testMixedCrops(files.path());
        std::cout << "Volume loading and per-layer context tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
