#include "src/controllers/SignalControl.h"
#include "src/qtUtils/TaskRunner.h"
#include "src/qtUtils/SelectedSegmentationAutosave.h"
#include "src/viewers/OrthoViewer.h"
#include <itkImageFileWriter.h>
#include <QApplication>
#include <QEventLoop>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <iostream>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
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
    control.loadSegmentationVolume(path, {}, [&](auto result) { loaded = result.has_value(); loop.quit(); });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec(); dialogs.stop();
    require(loaded && graph.initialNodes.size() == 2, "Duplicating segmentation as supervoxels failed");
    const auto region = image->GetBufferedRegion();
    for (const auto storage : {base->pWorkingSegmentsImage, base->pSelectedSegmentation}) {
        const auto actual = storage->denseRegion(region);
        require(std::equal(image->GetBufferPointer(), image->GetBufferPointer() + region.GetNumberOfPixels(),
                           actual->GetBufferPointer()), "Duplicating segmentation changed voxels");
    }
    base->pSelectedSegmentation->SetPixel({1, 1, 1}, 9);
    require(base->pWorkingSegmentsImage->GetPixel({1, 1, 1}) == 1, "Duplicated volumes share mutable storage");
    control.loadRefinement(path);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (!runner.isBusy()) loop.quit(); });
    poll.start(10); loop.exec(); poll.stop();
    require(base->pSelectedRefinement && base->pSelectedRefinement->GetPixel({1, 1, 1}) == 1,
            "Streaming refinement load failed");
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
    auto awaitLoad = [&](auto start) {
        QEventLoop loop;
        bool complete = false;
        SignalControl::LoadResult result;
        QTimer deadline; deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        start([&](SignalControl::LoadResult loaded) { result = loaded; complete = true; loop.quit(); });
        deadline.start(30000); loop.exec();
        require(complete && result && !runner.isBusy(), "Mixed crop load failed");
    };
    volume_io::ImageLoadOptions options;
    options.crop = volume_io::CropRegion{{{2, 2, 2}}, {{4, 3, 2}}};
    awaitLoad([&](auto done) { control.addSegmentsGraphAsync(path, done, options); });
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
    awaitLoad([&](auto done) { control.addImageAsync(external, "External crop", done); });
    awaitLoad([&](auto done) { control.loadMembraneProbabilityAsync(path, "Other origin",
        SignalControl::BoundaryLoadMode::BoundaryOnly, SignalControl::FloatBoundaryConversionMode::CastValues, done,
        volume_io::ImageLoadOptions{volume_io::CropRegion{{{0, 0, 0}}, {{4, 3, 2}}}}); });
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
        testSegmentationAsSupervoxels(files.path());
        testMixedCrops(files.path());
        std::cout << "Volume loading and per-layer context tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
