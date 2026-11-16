#include "src/viewers/Segment3DViewerDialog.h"
#include "src/utils/AppLogger.h"

#include <QApplication>
#include <QCheckBox>
#include <QEventLoop>
#include <QImage>
#include <QProgressDialog>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRendererCollection.h>
#include <vtkSphereSource.h>

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

} // namespace

// frameSwapped requires a native Qt display.
int main(int argc, char **argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("SegmentPuzzlerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RenderLogging"));
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
            require(simpleDialog->findChild<QProgressDialog *>() == nullptr, "Single batch retained its loading dialog");
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
        std::cout << "3D render logging tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
