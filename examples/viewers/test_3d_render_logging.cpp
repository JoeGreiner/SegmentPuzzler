#include "src/viewers/Segment3DViewerDialog.h"
#include "src/utils/AppLogger.h"
#include "src/viewers/itkSignalBase.h"

#include <QApplication>
#include <QCheckBox>
#include <QEventLoop>
#include <QImage>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QSettings>
#include <QTemporaryDir>
#include <QSurfaceFormat>
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
#include <vtkProperty.h>
#include <vtkPropPicker.h>
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
    std::vector<vtkSmartPointer<vtkActor>> actors;
    auto *collection = renderer->GetActors();
    collection->InitTraversal();
    while (auto *actor = collection->GetNextActor()) actors.push_back(actor);
    require(actors.size() == 3, "Unexpected LOD actor count");
    for (std::size_t i = 0; i < actors.size(); ++i)
        require(vtkPolyDataMapper::SafeDownCast(actors[i]->GetMapper())->GetInput() == scene.meshes[i].polyData,
                "Viewer did not use the prepared mesh");
    int swaps = 0;
    QObject::connect(widget, &QOpenGLWidget::frameSwapped, dialog.get(), [&] { ++swaps; });
    require(widget->renderWindow()->GetNeverRendered(), "LOD rendered before initialization");
    dialog->show();
    require(waitUntil([&] { return swaps > 0; }), "LOD initial frame was not presented");
    dialog->findChild<QSlider *>("explodeSlider")->setValue(20);
    dialog->findChild<QCheckBox *>("hideImageBoundarySegmentsCheckBox")->setChecked(true);
    require(!actors[0]->GetVisibility() && actors[1]->GetVisibility(), "LOD filtering failed");
    const auto *position = actors[1]->GetPosition();
    renderer->SetWorldPoint(scene.meshes[1].centerWorld[0] + position[0],
                           scene.meshes[1].centerWorld[1] + position[1],
                           scene.meshes[1].centerWorld[2] + position[2], 1.0);
    renderer->WorldToDisplay();
    const auto *display = renderer->GetDisplayPoint();
    vtkNew<vtkPropPicker> picker;
    require(picker->Pick(display[0], display[1], 0, renderer) && picker->GetViewProp() == actors[1],
            "Picking an exploded LOD segment failed");
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
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    try {
        using segment_puzzler::app_logging::AppLogger;
        auto settings = AppLogger::defaultSettings();
        settings.consoleEnabled = false;
        settings.fileEnabled = false;
        AppLogger::setSettings(settings, false);

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
        vtkSmartPointer<vtkRenderWindowInteractor> interactor = widget->interactor();
        auto *style = vtkInteractorStyleTrackballCamera::SafeDownCast(interactor->GetInteractorStyle());
        require(style != nullptr, "Missing trackball style");
        style->SetCurrentRenderer(style->GetDefaultRenderer());

        interactor->SetEventPosition(window->GetSize()[0] / 2, window->GetSize()[1] / 2);
        style->FindPokedRenderer(interactor->GetEventPosition()[0], interactor->GetEventPosition()[1]);
        auto *camera = style->GetCurrentRenderer()->GetActiveCamera();
        const double initialDistance = camera->GetDistance();
        for (const bool forward : {true, false}) {
            const int previousSwaps = swaps;
            const int previousRenders = renders;
            if (forward) {
                style->OnMouseWheelForward();
            } else {
                style->OnMouseWheelBackward();
            }
            require(waitUntil([&] { return swaps > previousSwaps; }), "Wheel frame was not presented");
            require(renders == previousRenders + 1,
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
        interactor->EnableRenderOn();

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
        require(visibleActors == 512, "Grouped initialization changed the segment filter");
        require(groupedWidget->interactor()->GetEnableRender(), "Grouped initialization left rendering disabled");
        require(renderer->GetNumberOfPropsRendered() == 512, "Final render did not contain the complete selection");
        const auto groupedFrame = groupedWidget->grabFramebuffer();
        require(groupedFrame.pixelColor(10, 10) == backgroundColor,
                "Grouped initialization did not display the rendered framebuffer");
        require(groupedFrame.pixelColor(groupedFrame.width() / 2, groupedFrame.height() / 2) != backgroundColor,
                "Grouped initialization displayed only the background");
        grouped.reset();

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
        testReducedDetail();
        std::cout << "3D render logging tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
