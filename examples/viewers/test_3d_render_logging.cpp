#include "src/viewers/Segment3DViewerDialog.h"
#include "src/viewers/SegmentMeshBatchRenderer.h"
#include "src/utils/AppLogger.h"
#include "src/viewers/itkSignalBase.h"

#include <QApplication>
#include <QCheckBox>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QShortcut>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QSettings>
#include <QTemporaryDir>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkFeatureEdges.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkPointData.h>
#include <vtkTypeInt32Array.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRendererCollection.h>
#include <vtkSphereSource.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitUntil(const std::function<bool()> &ready) {
    QElapsedTimer timeout;
    timeout.start();
    while (!ready() && timeout.elapsed() < 10000) {
        QEventLoop turn;
        QTimer::singleShot(5, &turn, &QEventLoop::quit);
        turn.exec();
    }
    return ready();
}

QString readLog(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Could not read render log");
    return QString::fromUtf8(file.readAll());
}

double value(const QString &line, const QString &key) {
    const auto match = QRegularExpression(QStringLiteral("(?:^| )%1=([^ ]+)").arg(key)).match(line);
    bool ok = false;
    const double result = match.captured(1).toDouble(&ok);
    require(ok, "Missing or non-numeric timing field");
    return result;
}

void testBatchGeometry() {
    // Check chunk splitting and float precision far from the origin.
    const std::array<double, 3> center{1e12, -1e12, 1e12};
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    for (const auto &offset : std::array<std::array<double, 3>, 3>{{{0, 0, 0}, {0.125, 0, 0}, {0, 0.25, 0}}})
        points->InsertNextPoint(center[0] + offset[0], center[1] + offset[1], center[2]);
    vtkNew<vtkTypeInt32Array> indices;
    constexpr vtkIdType triangles = 4'000'001;
    indices->SetNumberOfValues(3 * triangles);
    auto *values = indices->GetPointer(0);
    for (vtkIdType i = 0; i < 3 * triangles; ++i) values[i] = i % 3;
    vtkNew<vtkCellArray> cells;
    cells->SetData(3, indices);
    vtkNew<vtkPolyData> mesh;
    mesh->SetPoints(points);
    mesh->SetPolys(cells);
    auto sceneCenter = center;
    sceneCenter[0] -= 1;
    SegmentMeshBatchRenderer batches({{1, mesh, 0xFF123456, center}}, sceneCenter);
    std::vector<vtkSmartPointer<vtkActor>> actors;
    vtkIdType total = 0;
    int count = 0;
    while (batches.hasPendingMeshes()) {
        auto actor = batches.buildNext();
        actors.push_back(actor);
        auto *data = vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput();
        require(data->GetNumberOfPolys() <= 4'000'000, "Batch exceeded its picking/upload limit");
        require(data->GetNumberOfPoints() == 3, "Batch did not compact referenced points");
        require(data->GetPoint(1)[0] == 1.125 && data->GetPoint(2)[1] == 0.25,
                "Batch lost precision for a distant image");
        total += data->GetNumberOfPolys();
        ++count;
    }
    require(count == 2 && total == triangles && batches.progress() == 100,
            "Splitting a large segment lost triangles or progress");
    require(batches.visibleCounts() == std::make_pair(std::size_t{1}, triangles),
            "Diagnostics counted a split segment twice");
    batches.setExplode(2);
    batches.setVisible(1, false);
    require(batches.visibleCounts() == std::make_pair(std::size_t{0}, vtkIdType{0}),
            "Diagnostics counted a hidden segment");
    for (const auto &actor : actors) {
        require(actor->GetBounds()[0] == center[0] + 2, "Culling bounds ignored shader Explode");
        require(!actor->GetVisibility(), "An empty batch remained visible");
    }
    batches.setVisible(1, true);
    for (const auto &actor : actors) require(actor->GetVisibility(), "An empty batch could not be restored");
}

void testReducedDetail() {
    auto image = dataType::SegmentsImageType::New();
    image->SetRegions(dataType::SegmentsImageType::RegionType({{0, 0, 0}}, {{64, 24, 24}}));
    const double spacing[] = {0.4, 0.8, 1.2};
    const double origin[] = {10.0, 20.0, 30.0};
    image->SetSpacing(spacing);
    image->SetOrigin(origin);
    image->Allocate();
    image->FillBuffer(0);
    for (int z = 4; z < 20; ++z) {
        for (int y = 4; y < 20; ++y) {
            for (int x = 2; x < 18; ++x) {
                image->SetPixel({{x, y, z}}, 1);
                image->SetPixel({{x + 42, y, z}}, 2);
            }
        }
    }
    image->SetPixel({{30, 12, 12}}, 3);
    const std::vector<Segment3DViewerDialog::LabelWithColor> labels{
        {1, 0xFFFF0000}, {2, 0xFF00FF00}, {3, 0xFF0000FF}};
    QSettings settings;
    settings.setValue("graphics/lodEnabled", false);
    auto original = Segment3DViewerDialog::prepareScene(image, labels);
    require(original.meshes.size() == 3, "Preparation lost a segment");
    require(original.meshes[0].polyData->GetPoints() == original.meshes[1].polyData->GetPoints(),
            "Shared-point fixture no longer exercises shared points");
    constexpr int roiVoxels = 60 * 18 * 18;
    settings.setValue("graphics/lodEnabled", true);
    settings.setValue("graphics/lodVoxelThreshold", roiVoxels + 1);
    auto belowThreshold = Segment3DViewerDialog::prepareScene(image, labels);
    for (std::size_t i = 0; i < labels.size(); ++i)
        require(belowThreshold.meshes[i].polyData->GetNumberOfPolys() == original.meshes[i].polyData->GetNumberOfPolys(),
                "Scene below threshold was simplified");
    settings.setValue("graphics/lodVoxelThreshold", roiVoxels);
    auto scene = Segment3DViewerDialog::prepareScene(image, labels);
    require(scene.meshes.size() == 3, "LOD preparation lost a segment");
    require(scene.meshes[2].polyData->GetNumberOfPolys() == original.meshes[2].polyData->GetNumberOfPolys(),
            "Tiny segment did not retain its geometry");
    require(scene.meshes[2].polyData->GetNumberOfPoints() < original.meshes[2].polyData->GetNumberOfPoints(),
            "Tiny segment retained the shared original point buffer");
    for (int i = 0; i < 2; ++i) {
        const auto &mesh = scene.meshes[i];
        require(mesh.polyData->GetNumberOfPolys() > 0 &&
                    mesh.polyData->GetNumberOfPolys() < original.meshes[i].polyData->GetNumberOfPolys() &&
                    mesh.lodFactor == 4,
                "LOD did not replace the full mesh");
        require(mesh.polyData->GetActualMemorySize() < original.meshes[i].polyData->GetActualMemorySize(),
                "LOD did not reduce mesh memory");
        double bounds[6], reducedBounds[6];
        original.meshes[i].polyData->GetCellsBounds(bounds);
        mesh.polyData->GetCellsBounds(reducedBounds);
        for (int axis = 0; axis < 3; ++axis) {
            require(std::abs(reducedBounds[2 * axis] - bounds[2 * axis]) <= 4 * spacing[axis] &&
                        std::abs(reducedBounds[2 * axis + 1] - bounds[2 * axis + 1]) <= 4 * spacing[axis],
                    "Downsampling shifted the mesh out of its physical region");
        }
    }
    settings.setValue("graphics/lodReductionFactor", 8);
    const auto coarser = Segment3DViewerDialog::prepareScene(image, labels);
    require(coarser.meshes[0].polyData->GetNumberOfPolys() > 0 &&
                coarser.meshes[0].polyData->GetNumberOfPolys() < scene.meshes[0].polyData->GetNumberOfPolys(),
            "Increasing the LOD factor did not produce coarser geometry");
    settings.remove("graphics/lodReductionFactor");
    settings.setValue("graphics/lodVoxelThreshold", 1);
    settings.setValue("graphics/lodEnabled", false);
    auto disabled = Segment3DViewerDialog::prepareScene(image, labels);
    require(disabled.meshes[0].polyData->GetNumberOfPolys() == original.meshes[0].polyData->GetNumberOfPolys(),
            "Disabled LOD still simplified geometry");
    settings.setValue("graphics/lodEnabled", true);
    auto splitScene = Segment3DViewerDialog::prepareScene(image, labels, Roi(), false);
    require(splitScene.meshes[0].polyData->GetNumberOfPolys() == original.meshes[0].polyData->GetNumberOfPolys(),
            "Split preparation used reduced geometry");
    auto allLabels = Segment3DViewerDialog::prepareAllLabelsScene(image, LabelColorSnapshot{});
    require(allLabels.meshes.size() == scene.meshes.size(), "LOD lost labels in the full scene");
    for (std::size_t i = 0; i < scene.meshes.size(); ++i)
        require(allLabels.meshes[i].polyData->GetNumberOfPolys() == scene.meshes[i].polyData->GetNumberOfPolys(),
                "Full scene used different LOD geometry");

    settings.setValue("graphics/lodReductionFactor", 1);
    const auto factorOne = Segment3DViewerDialog::prepareScene(image, labels);
    for (std::size_t i = 0; i < labels.size(); ++i)
        require(factorOne.meshes[i].polyData->GetNumberOfPolys() == original.meshes[i].polyData->GetNumberOfPolys(),
                "Factor one changed full-detail geometry");
    settings.setValue("graphics/lodReductionFactor", 4);

    auto direction = image->GetDirection();
    direction[0][0] = direction[1][1] = 0;
    direction[0][1] = -1;
    direction[1][0] = 1;
    image->SetDirection(direction);
    const auto rotated = Segment3DViewerDialog::prepareScene(image, labels);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        double bounds[6], rotatedBounds[6];
        scene.meshes[i].polyData->GetCellsBounds(bounds);
        rotated.meshes[i].polyData->GetCellsBounds(rotatedBounds);
        const double expected[] = {origin[0] - bounds[3] + origin[1], origin[0] - bounds[2] + origin[1],
                                   origin[1] + bounds[0] - origin[0], origin[1] + bounds[1] - origin[0],
                                   bounds[4], bounds[5]};
        for (int j = 0; j < 6; ++j)
            require(std::abs(rotatedBounds[j] - expected[j]) < 1e-4, "LOD lost the image direction or origin");
    }

    auto borderImage = dataType::SegmentsImageType::New();
    borderImage->SetRegions(dataType::SegmentsImageType::RegionType({{0, 0, 0}}, {{9, 10, 11}}));
    borderImage->Allocate();
    borderImage->FillBuffer(70000);
    auto borderScene = Segment3DViewerDialog::prepareScene(borderImage, {{70000, 0xFFFFFFFF}});
    require(borderScene.meshes.size() == 1 && borderScene.meshes[0].lodFactor == 4,
            "LOD lost a high label ID at the image boundary");
    vtkNew<vtkFeatureEdges> edges;
    edges->SetInputData(borderScene.meshes[0].polyData);
    edges->BoundaryEdgesOn();
    edges->NonManifoldEdgesOn();
    edges->FeatureEdgesOff();
    edges->ManifoldEdgesOff();
    edges->Update();
    require(edges->GetOutput()->GetNumberOfCells() == 0, "Downsampled boundary mesh is not closed");

    borderImage->FillBuffer(0);
    borderImage->SetPixel({{2, 2, 2}}, 70000);
    borderImage->SetPixel({{5, 5, 5}}, 2);
    settings.setValue("graphics/lodReductionFactor", 64);
    const auto allMissing = Segment3DViewerDialog::prepareScene(borderImage, {{70000, 0xFFFFFFFF}, {2, 0xFFFFFFFF}});
    require(allMissing.meshes.size() == 2 && allMissing.meshes[0].labelId == 70000 &&
                allMissing.meshes[1].labelId == 2 && allMissing.meshes[0].lodFactor == 1 &&
                allMissing.meshes[1].lodFactor == 1, "Fallback did not restore missing labels in order");
    borderImage->SetPixel({{8, 8, 8}}, 70000);
    Roi roi;
    roi.minX = roi.minY = roi.minZ = 1;
    roi.maxX = roi.maxY = roi.maxZ = 3;
    const auto clipped = Segment3DViewerDialog::prepareScene(borderImage, {{70000, 0xFFFFFFFF}}, roi);
    require(clipped.meshes.size() == 1 && clipped.meshes[0].polyData->GetBounds()[1] < 4,
            "Fallback restored geometry outside the requested ROI");
    settings.remove("graphics/lodReductionFactor");

    scene.meshes[0].touchesImageBoundary = true;
    auto dialog = std::make_unique<Segment3DViewerDialog>(scene);
    require(!dialog->findChild<QCheckBox *>("reducedDetailCheckBox"), "Viewer retained the LOD switch");
    auto *widget = dialog->findChild<QVTKOpenGLNativeWidget *>();
    auto *renderer = widget->interactor()->GetInteractorStyle()->GetDefaultRenderer();
    int swaps = 0;
    QObject::connect(widget, &QOpenGLWidget::frameSwapped, dialog.get(), [&] { ++swaps; });
    require(widget->renderWindow()->GetNeverRendered(), "LOD rendered before initialization");
    dialog->show();
    require(waitUntil([&] { return swaps > 0 && !dialog->findChild<QProgressDialog *>(); }),
            "LOD initial frame was not presented");
    require(renderer->GetActors()->GetNumberOfItems() == 1, "LOD meshes were not batched");
    auto *batchActor = renderer->GetActors()->GetLastActor();
    auto *combined = vtkPolyDataMapper::SafeDownCast(batchActor->GetMapper())->GetInput();
    vtkIdType combinedCell = 0;
    for (const auto &mesh : scene.meshes) {
        for (vtkIdType cell = 0; cell < mesh.polyData->GetNumberOfPolys(); ++cell, ++combinedCell) {
            vtkIdType size, combinedSize;
            const vtkIdType *ids, *combinedIds;
            mesh.polyData->GetPolys()->GetCellAtId(cell, size, ids);
            combined->GetPolys()->GetCellAtId(combinedCell, combinedSize, combinedIds);
            require(size == combinedSize, "Batch changed the triangle topology");
            for (int corner = 0; corner < 3; ++corner) {
                double expected[3], actual[3];
                mesh.polyData->GetPoint(ids[corner], expected);
                combined->GetPoint(combinedIds[corner], actual);
                for (int axis = 0; axis < 3; ++axis)
                    require(std::abs(actual[axis] + batchActor->GetPosition()[axis] - expected[axis]) < 1e-5,
                            "Batch changed a vertex or its winding");
            }
        }
    }
    require(combinedCell == combined->GetNumberOfPolys(), "Batch lost or duplicated triangles");

    dataType::SegmentIdType picked = 0;
    dialog->setNavigateToLabelHandler([&](auto label) { picked = label; });
    auto click = [&](std::size_t index, double factor) {
        const auto &center = scene.meshes[index].centerWorld;
        renderer->SetWorldPoint(center[0] + factor * (center[0] - scene.sceneCenterWorld[0]),
                               center[1] + factor * (center[1] - scene.sceneCenterWorld[1]),
                               center[2] + factor * (center[2] - scene.sceneCenterWorld[2]), 1);
        renderer->WorldToDisplay();
        const auto *display = renderer->GetDisplayPoint();
        const auto ratio = widget->devicePixelRatioF();
        const QPointF point(std::round(display[0]) / ratio,
                            (widget->renderWindow()->GetSize()[1] - 1 - std::round(display[1])) / ratio);
        QMouseEvent event(QEvent::MouseButtonPress, point, widget->mapToGlobal(point.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        picked = 0;
        QApplication::sendEvent(widget, &event);
    };
    const int maximumExplode = dialog->findChild<QSlider *>("explodeSlider")->maximum();
    for (const int explode : {0, 20, 200, maximumExplode}) {
        dialog->findChild<QSlider *>("explodeSlider")->setValue(explode);
        renderer->ResetCamera();
        if (explode == maximumExplode) {
            auto *camera = renderer->GetActiveCamera();
            double position[3], focal[3];
            camera->GetPosition(position);
            camera->GetFocalPoint(focal);
            for (int axis = 0; axis < 3; ++axis) {
                const auto target = scene.meshes[1].centerWorld[axis]
                    + explode / 100.0 * (scene.meshes[1].centerWorld[axis] - scene.sceneCenterWorld[axis]);
                position[axis] += target - focal[axis];
                focal[axis] = target;
            }
            camera->SetPosition(position);
            camera->SetFocalPoint(focal);
            camera->Zoom(3);
        }
        widget->renderWindow()->Render();
        const auto batchedFrame = widget->grabFramebuffer();
        batchActor->VisibilityOff();
        std::vector<vtkSmartPointer<vtkActor>> referenceActors;
        for (const auto &mesh : scene.meshes) {
            vtkNew<vtkPolyDataMapper> mapper;
            mapper->SetInputData(mesh.polyData);
            mapper->ScalarVisibilityOff();
            auto actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);
            actor->GetProperty()->DeepCopy(batchActor->GetProperty());
            actor->GetProperty()->SetColor(qRed(mesh.lutColor) / 255.0,
                                           qGreen(mesh.lutColor) / 255.0,
                                           qBlue(mesh.lutColor) / 255.0);
            actor->SetPosition((mesh.centerWorld[0] - scene.sceneCenterWorld[0]) * explode / 100.0,
                               (mesh.centerWorld[1] - scene.sceneCenterWorld[1]) * explode / 100.0,
                               (mesh.centerWorld[2] - scene.sceneCenterWorld[2]) * explode / 100.0);
            renderer->AddActor(actor);
            referenceActors.push_back(actor);
        }
        widget->renderWindow()->Render();
        const auto referenceFrame = widget->grabFramebuffer();
        int changedPixels = 0;
        for (int y = 0; y < referenceFrame.height(); ++y) {
            for (int x = 0; x < referenceFrame.width(); ++x) {
                const auto actual = batchedFrame.pixel(x, y), expected = referenceFrame.pixel(x, y);
                changedPixels += std::abs(qRed(actual) - qRed(expected)) > 2
                    || std::abs(qGreen(actual) - qGreen(expected)) > 2
                    || std::abs(qBlue(actual) - qBlue(expected)) > 2;
            }
        }
        require(changedPixels < referenceFrame.width() * referenceFrame.height() / 1000,
                "Shader rendering differs from individual actors");
        for (const auto &actor : referenceActors) renderer->RemoveActor(actor);
        batchActor->VisibilityOn();
        widget->renderWindow()->Render();
        click(1, explode / 100.0);
        require(picked == 2, "Picking an exploded LOD segment failed");
    }
    dialog->findChild<QSlider *>("explodeSlider")->setValue(20);
    renderer->ResetCamera();
    dialog->findChild<QCheckBox *>("hideImageBoundarySegmentsCheckBox")->setChecked(true);
    click(0, 0.2);
    require(picked == 0, "Filtered segment was still pickable");
    click(1, 0.2);
    require(picked == 2, "Filtering hid the wrong segment");
    auto *colors = combined->GetPointData()->GetScalars();
    const double oldColor = colors->GetComponent(0, 0);
    for (auto *shortcut : dialog->findChildren<QShortcut *>()) {
        if (shortcut->key() == QKeySequence(Qt::Key_R))
            QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
    }
    require(colors->GetComponent(0, 0) != oldColor || colors->GetComponent(0, 1) != 0,
            "Color cycling did not update the batch colors");
    click(1, 0.2);
    require(picked == 2, "Recoloring broke picking");
    dataType::SegmentIdType deleted = 0;
    dialog->setDeleteLabelHandler([&](auto label) { deleted = label; return true; });
    QKeyEvent keyDown(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier);
    QApplication::sendEvent(widget, &keyDown);
    click(1, 0.2);
    require(deleted == 2, "Delete dispatched the wrong label");
    QKeyEvent keyUp(QEvent::KeyRelease, Qt::Key_D, Qt::NoModifier);
    QApplication::sendEvent(widget, &keyUp);
    click(1, 0.2);
    require(picked == 0, "Deleted segment was still pickable");
    dialog->findChild<QCheckBox *>("hideImageBoundarySegmentsCheckBox")->setChecked(false);
    click(0, 0.2);
    require(picked == 1, "Clearing the filter did not restore the segment");
    click(1, 0.2);
    require(picked == 0, "Clearing the filter restored a deleted segment");
    settings.remove("graphics/lodEnabled");
    settings.remove("graphics/lodVoxelThreshold");
}

} // namespace

// frameSwapped requires a native Qt display.
int main(int argc, char **argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("SegmentPuzzlerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RenderLogging"));
    QTemporaryDir directory;
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    try {
        require(directory.isValid(), "Could not create temporary log directory");
        const auto logPath = directory.filePath(QStringLiteral("render.log"));
        using segment_puzzler::app_logging::AppLogger;
        auto settings = AppLogger::defaultSettings();
        settings.consoleEnabled = false;
        settings.fileEnabled = true;
        settings.logFilePath = logPath;
        AppLogger::setSettings(settings, false);
        const auto summaries = [&] {
            return readLog(logPath).split(QLatin1Char('\n')).filter(QStringLiteral("[3DView] interaction "));
        };

        vtkNew<vtkSphereSource> sphere;
        sphere->Update();
        Segment3DViewerDialog::PreparedScene scene;
        scene.windowTitle = QStringLiteral("3D render logging test");
        scene.sceneExtent = 2.0;
        scene.meshes = {{1, sphere->GetOutput(), 0xFFAAAAAA, {-0.5, 0.0, 0.0}},
                        {2, sphere->GetOutput(), 0xFFAAAAAA, {0.5, 0.0, 0.0}}};
        auto dialog = std::make_unique<Segment3DViewerDialog>(scene);
        auto *widget = dialog->findChild<QVTKOpenGLNativeWidget *>();
        require(widget != nullptr, "Missing VTK widget");
        vtkSmartPointer<vtkRenderWindow> window = widget->renderWindow();
        std::array<int, 2> renderCounts{};
        auto &renders = renderCounts[0];
        auto &sceneRenders = renderCounts[1];
        vtkNew<vtkCallbackCommand> countRenders;
        countRenders->SetClientData(&renderCounts);
        countRenders->SetCallback([](vtkObject *object, unsigned long event, void *data, void *) {
            auto &counts = *static_cast<std::array<int, 2> *>(data);
            if (event == vtkCommand::StartEvent) {
                ++counts[0];
            } else if (vtkRenderWindow::SafeDownCast(object)->GetRenderers()->GetFirstRenderer()->GetNumberOfPropsRendered() > 0) {
                ++counts[1];
            }
        });
        window->AddObserver(vtkCommand::StartEvent, countRenders);
        window->AddObserver(vtkCommand::EndEvent, countRenders);
        int swaps = 0;
        QObject::connect(widget, &QOpenGLWidget::frameSwapped, &app, [&] { ++swaps; });
        dialog->show();
        require(waitUntil([&] { return swaps > 0; }), "Initial frame was not presented");
        require(renders == 2 && sceneRenders == 1, "Expected one empty frame and one scene render");
        const auto backgroundColor = widget->grabFramebuffer().pixelColor(10, 10);
        require(readLog(logPath).contains("vtk_render_ms="), "Render time was not renamed");
        vtkSmartPointer<vtkRenderWindowInteractor> interactor = widget->interactor();
        auto *style = vtkInteractorStyleTrackballCamera::SafeDownCast(interactor->GetInteractorStyle());
        require(style != nullptr, "Missing trackball style");
        style->SetCurrentRenderer(style->GetDefaultRenderer());

        // Three renders share one Qt swap; the last follows EndInteractionEvent.
        vtkNew<vtkCallbackCommand> delay;
        delay->SetCallback([](vtkObject *, unsigned long, void *, void *) { QThread::msleep(80); });
        const auto delayTag = window->AddObserver(vtkCommand::StartEvent, delay, -1.0f);
        style->StartRotate();
        window->Render();
        window->Render();
        style->EndRotate();
        require(summaries().isEmpty(), "Interaction was logged before the last swap");
        require(waitUntil([&] { return summaries().size() == 1; }), "Missing coalesced interaction summary");
        auto line = summaries().last();
        require(value(line, "vtk_renders") == 3 && value(line, "qt_frames") == 1, "Wrong coalesced frame counts");
        require(value(line, "vtk_render_ms_avg") >= 75, "Render samples were lost");
        require(value(line, "vtk_render_ms_max") >= value(line, "vtk_render_ms_avg"), "Invalid render maximum");
        require(value(line, "render_to_swap_ms_avg") >= 3 * value(line, "vtk_render_ms_avg") - 5,
                "Swap timer was restarted by a later render");
        std::cout << line.toStdString() << '\n';
        window->RemoveObserver(delayTag);

        // A drag remains active across multiple Qt swaps.
        style->StartRotate();
        for (int i = 0; i < 2; ++i) {
            const int previousSwaps = swaps;
            window->Render();
            require(waitUntil([&] { return swaps > previousSwaps; }), "Drag frame was not presented");
            require(summaries().size() == 1, "Active drag was logged early");
        }
        style->EndRotate();
        require(waitUntil([&] { return summaries().size() == 2; }), "Missing drag summary");
        line = summaries().last();
        require(value(line, "vtk_renders") == 3 && value(line, "qt_frames") == 3, "Drag counts did not reset");
        const int previousSwaps = swaps;
        widget->update();
        require(waitUntil([&] { return swaps > previousSwaps; }), "Idle repaint did not complete");
        require(summaries().size() == 2, "Idle repaint generated an interaction summary");

        interactor->SetEventPosition(window->GetSize()[0] / 2, window->GetSize()[1] / 2);
        style->FindPokedRenderer(interactor->GetEventPosition()[0], interactor->GetEventPosition()[1]);
        auto *camera = style->GetCurrentRenderer()->GetActiveCamera();
        const double initialDistance = camera->GetDistance();
        for (const bool forward : {true, false}) {
            const auto previousSummaries = summaries().size();
            const int previousRenders = renders;
            if (forward) {
                style->OnMouseWheelForward();
            } else {
                style->OnMouseWheelBackward();
            }
            require(waitUntil([&] { return summaries().size() == previousSummaries + 1; }),
                    "Missing wheel summary");
            require(renders == previousRenders + 1 && value(summaries().last(), "vtk_renders") == 1,
                    "Wheel step did not render exactly once");
            require(interactor->GetEnableRender(), "Wheel step left rendering disabled");
            require(forward ? camera->GetDistance() < initialDistance
                            : std::abs(camera->GetDistance() - initialDistance) < 1e-6,
                    "Wheel zoom changed camera behaviour");
        }
        const int rendersBeforeDisabledWheel = renders;
        interactor->EnableRenderOff();
        style->OnMouseWheelForward();
        style->OnMouseWheelBackward();
        require(!interactor->GetEnableRender() && renders == rendersBeforeDisabledWheel,
                "Wheel step ignored the disabled render state");
        QApplication::processEvents();
        require(summaries().size() == 4, "Disabled rendering generated a wheel summary");
        interactor->EnableRenderOn();

        auto *slider = dialog->findChild<QSlider *>(QStringLiteral("explodeSlider"));
        require(slider != nullptr, "Missing explode slider");
        slider->setSliderDown(true);
        slider->setValue(10);
        slider->setValue(20);
        slider->setSliderDown(false);
        require(waitUntil([&] { return summaries().size() == 5; }), "Missing explode drag summary");
        require(value(summaries().last(), "vtk_renders") == 2, "Explode drag was not grouped");
        slider->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        require(waitUntil([&] { return summaries().size() == 6; }), "Missing explode step summary");

        style->StartRotate();
        window->Render();
        style->EndRotate();
        dialog->hide();
        require(summaries().size() == 7, "Hidden widget retained a pending interaction");
        require(value(summaries().last(), "qt_frames") == 0 &&
                    summaries().last().contains("render_to_swap_ms_avg=unavailable"), "Missing swap was reported as zero time");
        window->RemoveObserver(countRenders);
        dialog.reset();
        // Check observer cleanup on retained VTK objects.
        interactor->InvokeEvent(vtkCommand::StartInteractionEvent);
        interactor->InvokeEvent(vtkCommand::EndInteractionEvent);
        window->InvokeEvent(vtkCommand::StartEvent);
        window->InvokeEvent(vtkCommand::EndEvent);
        QApplication::processEvents();

        // Preserve a disabled interactor while still displaying the initial scene.
        auto fallbackDialog = std::make_unique<Segment3DViewerDialog>(scene);
        auto *fallbackWidget = fallbackDialog->findChild<QVTKOpenGLNativeWidget *>();
        fallbackWidget->interactor()->EnableRenderOff();
        renderCounts = {};
        fallbackWidget->renderWindow()->AddObserver(vtkCommand::StartEvent, countRenders);
        fallbackWidget->renderWindow()->AddObserver(vtkCommand::EndEvent, countRenders);
        int fallbackSwaps = 0;
        QObject::connect(fallbackWidget, &QOpenGLWidget::frameSwapped, fallbackDialog.get(),
                         [&] { ++fallbackSwaps; });
        fallbackDialog->show();
        require(waitUntil([&] { return fallbackSwaps > 0; }), "Fallback initial frame was not presented");
        require(renders == 2 && sceneRenders == 1 && !fallbackWidget->renderWindow()->GetNeverRendered(),
                "Fallback initial scene did not render exactly once");
        require(!fallbackWidget->interactor()->GetEnableRender(), "Initial render changed the render state");
        const auto fallbackFrame = fallbackWidget->grabFramebuffer();
        require(fallbackFrame.pixelColor(10, 10) == backgroundColor
                    && fallbackFrame.pixelColor(fallbackFrame.width() / 2, fallbackFrame.height() / 2) != backgroundColor,
                "Fallback initial frame did not display the scene");
        fallbackWidget->renderWindow()->RemoveObserver(countRenders);
        fallbackDialog.reset();

        for (const int segmentCount : {0, 1}) {
            auto simpleScene = scene;
            simpleScene.meshes.resize(segmentCount);
            auto simpleDialog = std::make_unique<Segment3DViewerDialog>(simpleScene);
            auto *simpleWidget = simpleDialog->findChild<QVTKOpenGLNativeWidget *>();
            renderCounts = {};
            simpleWidget->renderWindow()->AddObserver(vtkCommand::StartEvent, countRenders);
            simpleWidget->renderWindow()->AddObserver(vtkCommand::EndEvent, countRenders);
            int simpleSwaps = 0;
            QObject::connect(simpleWidget, &QOpenGLWidget::frameSwapped, simpleDialog.get(), [&] { ++simpleSwaps; });
            simpleDialog->show();
            require(waitUntil([&] { return simpleSwaps > 0; }), "Empty or single-segment scene did not finish");
            require(renders == 2 && sceneRenders == segmentCount, "Single batch rendered geometry repeatedly");
            require(waitUntil([&] { return simpleDialog->findChild<QProgressDialog *>() == nullptr; }),
                    "Single batch retained its loading dialog");
            simpleWidget->renderWindow()->RemoveObserver(countRenders);
        }

        // Large scenes yield between groups, preserve filtering and present the full scene last.
        auto largeScene = scene;
        largeScene.meshes.clear();
        for (int i = 0; i < 513; ++i) {
            auto mesh = scene.meshes.front();
            mesh.labelId = i + 1;
            mesh.touchesImageBoundary = i == 0;
            largeScene.meshes.push_back(mesh);
        }
        auto grouped = std::make_unique<Segment3DViewerDialog>(largeScene);
        auto *groupedWidget = grouped->findChild<QVTKOpenGLNativeWidget *>();
        grouped->findChild<QCheckBox *>(QStringLiteral("hideImageBoundarySegmentsCheckBox"))->setChecked(true);
        int groupCount = 0;
        int eventTurns = 0;
        bool progressReported = false;
        bool progressVisibleAtFinalRender = false;
        std::function<void(vtkRenderWindow *)> onRender = [&](vtkRenderWindow *window) {
            if (!window->GetSwapBuffers()) {
                ++groupCount;
                QTimer::singleShot(0, grouped.get(), [&] {
                    ++eventTurns;
                    auto *progress = grouped->findChild<QProgressDialog *>();
                    progressReported |= progress && progress->isVisible() && progress->value() > 0;
                });
            } else if (groupCount > 0) {
                auto *progress = grouped->findChild<QProgressDialog *>();
                progressVisibleAtFinalRender |= progress && progress->isVisible()
                    && progress->value() == progress->maximum();
            }
        };
        vtkNew<vtkCallbackCommand> observeGroups;
        observeGroups->SetClientData(&onRender);
        observeGroups->SetCallback([](vtkObject *object, unsigned long, void *data, void *) {
            (*static_cast<std::function<void(vtkRenderWindow *)> *>(data))(
                vtkRenderWindow::SafeDownCast(object));
        });
        groupedWidget->renderWindow()->AddObserver(vtkCommand::StartEvent, observeGroups);
        grouped->show();
        require(waitUntil([&] {
            auto *progress = grouped->findChild<QProgressDialog *>(QStringLiteral("initialRenderProgress"));
            return progressVisibleAtFinalRender && (!progress || !progress->isVisible());
        }), "Grouped initialization did not finish");
        require(progressReported && eventTurns == groupCount && groupCount > 0,
                "Initialization did not yield between groups");
        auto *renderer = groupedWidget->interactor()->GetInteractorStyle()->GetDefaultRenderer();
        int visibleActors = 0;
        auto *actors = renderer->GetActors();
        actors->InitTraversal();
        while (auto *actor = actors->GetNextActor()) visibleActors += actor->GetVisibility() != 0;
        require(visibleActors == 3, "Grouped initialization did not retain the three batches");
        require(groupedWidget->interactor()->GetEnableRender(), "Grouped initialization left rendering disabled");
        require(renderer->GetNumberOfPropsRendered() == 3, "Final render did not contain the complete selection");
        const auto groupedFrame = groupedWidget->grabFramebuffer();
        require(groupedFrame.pixelColor(10, 10) == backgroundColor,
                "Grouped initialization did not display the rendered framebuffer");
        require(groupedFrame.pixelColor(groupedFrame.width() / 2, groupedFrame.height() / 2) != backgroundColor,
                "Grouped initialization displayed only the background");
        grouped.reset();

        auto hiddenScene = largeScene;
        for (auto &mesh : hiddenScene.meshes) mesh.touchesImageBoundary = true;
        auto hidden = std::make_unique<Segment3DViewerDialog>(std::move(hiddenScene));
        auto *hiddenWidget = hidden->findChild<QVTKOpenGLNativeWidget *>();
        auto *hideBorder = hidden->findChild<QCheckBox *>("hideImageBoundarySegmentsCheckBox");
        hideBorder->setChecked(true);
        hidden->show();
        require(waitUntil([&] {
            return !hiddenWidget->renderWindow()->GetNeverRendered() && !hidden->findChild<QProgressDialog *>();
        }), "Initialization with every segment hidden did not finish");
        auto *hiddenRenderer = hiddenWidget->interactor()->GetInteractorStyle()->GetDefaultRenderer();
        require(hiddenRenderer->GetNumberOfPropsRendered() == 0, "Initialization displayed hidden batches");
        hideBorder->setChecked(false);
        require(hiddenRenderer->GetNumberOfPropsRendered() == 3, "Hidden batches could not be restored");
        hidden.reset();

        auto cancelled = std::make_unique<Segment3DViewerDialog>(largeScene);
        auto *cancelledWidget = cancelled->findChild<QVTKOpenGLNativeWidget *>();
        cancelledWidget->interactor()->EnableRenderOff();
        bool cancelRequested = false;
        onRender = [&](vtkRenderWindow *window) {
            if (!window->GetSwapBuffers() && !cancelRequested) {
                cancelRequested = true;
                QTimer::singleShot(0, cancelled.get(), [&] {
                    cancelled->findChild<QProgressDialog *>()->findChild<QPushButton *>()->click();
                });
            }
        };
        cancelledWidget->renderWindow()->AddObserver(
            vtkCommand::StartEvent, observeGroups);
        cancelled->show();
        require(waitUntil([&] {
            return cancelRequested && !cancelled->isVisible() && cancelled->findChild<QProgressDialog *>() == nullptr;
        }), "Cancelled initialization did not close and clean up");
        bool reopenedRendered = false;
        onRender = [&](vtkRenderWindow *window) {
            auto *progress = cancelled->findChild<QProgressDialog *>();
            if (window->GetSwapBuffers() && progress && progress->value() == progress->maximum()) {
                reopenedRendered = true;
            }
        };
        cancelled->show();
        require(waitUntil([&] {
            return reopenedRendered && cancelled->findChild<QProgressDialog *>() == nullptr;
        }), "Cancelled viewer could not reopen");
        require(!cancelledWidget->interactor()->GetEnableRender(),
                "Grouped initialization changed the disabled render state");
        require(cancelledWidget->grabFramebuffer().pixelColor(10, 10) == backgroundColor,
                "Reopened viewer did not display the rendered framebuffer");
        cancelled.reset();

        auto destroyed = std::make_unique<Segment3DViewerDialog>(largeScene);
        onRender = [&](vtkRenderWindow *window) {
            if (!window->GetSwapBuffers()) {
                QTimer::singleShot(0, destroyed.get(), [&] { destroyed.reset(); });
            }
        };
        destroyed->findChild<QVTKOpenGLNativeWidget *>()->renderWindow()->AddObserver(
            vtkCommand::StartEvent, observeGroups);
        destroyed->show();
        require(waitUntil([&] { return destroyed == nullptr; }), "Viewer was not destroyed between groups");
        QApplication::processEvents();
        testBatchGeometry();
        testReducedDetail();
        std::cout << "3D render logging tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
