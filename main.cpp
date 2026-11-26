#include <itkMultiThreaderBase.h>
#include "src/utils/WorkerThreads.h"
#include <QApplication>
#include <QTimer>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <itkImage.h>
#include <iostream>
#include <exception>
#include <QFile>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <QTextStream>
#include <QItemSelection>
#include "src/mainWindow.h"
#include <QMetaType>
#include <QtGlobal>
#include <QFileInfo>
#include "src/utils/StartupVolumes.h"
#include <cstdlib>
#include <clocale>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <vtkSMPTools.h>

#include "src/qtUtils/WindowStats.h"
#include "src/qtUtils/GraphicsSettings.h"
#include "src/utils/AppLogger.h"
#include "SegmentPuzzlerBuildInfo.h"

void loadStartupVolumes(SignalControl *control,
                        std::shared_ptr<const std::vector<StartupVolume>> volumes,
                        std::size_t index = 0) {
    while (index < volumes->size() && !QFileInfo::exists((*volumes)[index].path)) {
        SP_LOG_WARNING("app", QStringLiteral("Cannot access startup volume %1, skipping").arg((*volumes)[index].path));
        ++index;
    }
    if (index == volumes->size()) return;
    const auto &volume = (*volumes)[index];
    auto next = [control, volumes, index](SignalControl::LoadResult result) {
        if (!result)
            SP_LOG_WARNING("app", QStringLiteral("Could not load startup volume %1; continuing").arg((*volumes)[index].path));
        loadStartupVolumes(control, volumes, index + 1);
    };
    switch (volume.kind) {
    case StartupVolumeKind::Supervoxels:
        control->addSegmentsGraphAsync(volume.path, next, {}, volume.display, volume.name);
        break;
    case StartupVolumeKind::Image:
        control->addImageAsync(volume.path, volume.name, next, {}, volume.display);
        break;
    case StartupVolumeKind::Segmentation:
        control->loadSegmentationVolume(volume.path, volume.name, next, {}, volume.display);
        break;
    case StartupVolumeKind::Boundary:
        control->loadMembraneProbabilityAsync(volume.path, volume.name, SignalControl::BoundaryLoadMode::BoundaryOnly,
            SignalControl::FloatBoundaryConversionMode::CastValues, next, {}, volume.display);
        break;
    case StartupVolumeKind::Refinement:
        control->loadRefinementAsync(volume.path, volume.name, next, {}, volume.display);
        break;
    }
}

std::optional<segment_puzzler::app_logging::LogLevel> parseLogLevelOverride(const QString &rawValue) {
    using segment_puzzler::app_logging::LogLevel;

    if (rawValue.compare(QStringLiteral("trace"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Trace;
    }
    if (rawValue.compare(QStringLiteral("debug"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Debug;
    }
    if (rawValue.compare(QStringLiteral("info"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Info;
    }
    if (rawValue.compare(QStringLiteral("warning"), Qt::CaseInsensitive) == 0 ||
        rawValue.compare(QStringLiteral("warn"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Warning;
    }
    if (rawValue.compare(QStringLiteral("error"), Qt::CaseInsensitive) == 0) {
        return LogLevel::Error;
    }
    return std::nullopt;
}

#ifdef _WIN32
std::string getEnvVar(const char* name) {
    char* value = nullptr;
    size_t len = 0;
    std::string result;

    if (_dupenv_s(&value, &len, name) == 0 && value != nullptr) {
        result = value;
        free(value);
    }

    return result;
}
#endif

void configureVtkSmpBackend() {
    const bool backendChosenByUser = qEnvironmentVariableIsSet("VTK_SMP_BACKEND_IN_USE");
    const QString initialBackend = QString::fromLatin1(vtkSMPTools::GetBackend());

    if (!backendChosenByUser && initialBackend == QStringLiteral("Sequential")) {
        if (!vtkSMPTools::SetBackend("STDThread")) {
            SP_LOG_WARNING("app", QStringLiteral("VTK STDThread backend unavailable; continuing sequentially"));
        }
    }

    vtkSMPTools::Initialize(segment_puzzler::workerThreadCount(segment_puzzler::ThreadBackend::VTK));

    SP_LOG_INFO("app",
                QStringLiteral("VTK SMP backend=%1, estimatedThreads=%2")
                    .arg(QString::fromLatin1(vtkSMPTools::GetBackend()))
                    .arg(vtkSMPTools::GetEstimatedNumberOfThreads()));
}

int main(int argc, char *argv[]) {
    QCoreApplication::setApplicationName("SegmentPuzzler");
    QCoreApplication::setApplicationVersion(QString::fromUtf8(segment_puzzler::version));
    QCoreApplication::setOrganizationName("JoeGreiner");
    QCoreApplication::setOrganizationDomain("joegreiner.de");
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && QByteArray(argv[i]) == "--") {
            argc = i;
            argv[i] = nullptr;
            break;
        }
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }
    if (arguments.contains("--help")) {
        std::cout << "SegmentPuzzler [--segments FILE [NAME]] [--image FILE [NAME]] ...\n"
                     "              [--segmentation FILE [NAME]] ... [--boundary FILE [NAME]] [--refinement FILE [NAME]]\n"
                     "After each file and optional name: [--visible | --hidden] [--opacity 0..100]\n"
                     "Image and boundary layers: [--blend additive|over] [--range MIN MAX]\n"
                     "Scalar images and boundaries: [--color '#RRGGBB' | --color R G B] (0..255)\n"
                     "Label layers: [--view outline|filled] [--outline-width 1..10] (image pixels)\n"
                     "Global options: --log-level trace|debug|info|warning|error, --graphics=auto|system,\n"
                     "                -platform xcb|wayland, --reset-graphics\n";
        return 0;
    }
    const auto graphics = graphicsSettings::readStartupOptions(arguments);
    if (!graphics.error.isEmpty()) {
        std::cerr << graphics.error.toStdString() << '\n';
        return 1;
    }
    if (graphics.reset) {
        const QString error = graphicsSettings::resetSettings();
        if (!error.isEmpty()) {
            std::cerr << error.toStdString() << '\n';
        }
        return error.isEmpty() ? 0 : 1;
    }
    if (!graphics.warning.isEmpty()) {
        std::cerr << graphics.warning.toStdString() << '\n';
    }
    if (graphics.printSettings) {
        std::cout << graphics.mode.toStdString() << ' '
                  << graphics.platform.toStdString() << '\n';
        return 0;
    }
    graphicsSettings::applyStartupOptions(graphics);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication a(argc, argv);

    std::setlocale(LC_NUMERIC, "C");

    if (graphics.test) {
        return graphicsSettings::runRenderTest();
    }

    std::optional<segment_puzzler::app_logging::LogLevel> logLevelOverride;
    // Qt has consumed its own switches. Forward only layer arguments to the volume parser.
    const auto remaining = graphicsSettings::parseCommandLine(a.arguments()).remainingArguments;
    QStringList layerArguments;
    for (int i = 0; i < remaining.size(); ++i) {
        if (remaining[i] != "--log-level") {
            layerArguments.append(remaining[i]);
            continue;
        }
        if (i + 1 >= remaining.size()) {
            std::cerr << "Missing value for --log-level. Expected one of: trace, debug, info, warning, error.\n";
            return 1;
        }

        const QString &rawLogLevel = remaining[++i];
        logLevelOverride = parseLogLevelOverride(rawLogLevel);
        if (!logLevelOverride.has_value()) {
            std::cerr << "Invalid value for --log-level: " << rawLogLevel.toStdString()
                      << ". Expected one of: trace, debug, info, warning, error.\n";
            return 1;
        }
    }

    std::shared_ptr<const std::vector<StartupVolume>> startupVolumes;
    try {
        startupVolumes = std::make_shared<const std::vector<StartupVolume>>(
            parseStartupVolumes(layerArguments));
    } catch (const std::invalid_argument &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    segment_puzzler::app_logging::AppLogger::initialize();
    SP_LOG_INFO("app", QStringLiteral("Graphics mode=%1, source=%2, Qt platform=%3%4")
                           .arg(graphics.mode, graphics.source, QGuiApplication::platformName(),
                                graphics.warning.isEmpty() ? QString() : QStringLiteral("; ") + graphics.warning));
    QObject::connect(&a, &QGuiApplication::lastWindowClosed, &a, []() {
        SP_LOG_INFO("app",
                    QStringLiteral("QGuiApplication::lastWindowClosed emitted; %1")
                        .arg(windowStats::describeTopLevelWindows()));
    });
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &a, []() {
        SP_LOG_INFO("app",
                    QStringLiteral("QCoreApplication::aboutToQuit emitted; %1")
                        .arg(windowStats::describeTopLevelWindows()));
    });
    SP_LOG_INFO("app",
                QStringLiteral("Qt window lifecycle logging enabled; %1")
                    .arg(windowStats::describeTopLevelWindows()));
    if (logLevelOverride.has_value()) {
        auto logSettings = segment_puzzler::app_logging::AppLogger::settings();
        logSettings.minimumLevel = *logLevelOverride;
        segment_puzzler::app_logging::AppLogger::setSettings(logSettings, false);
        SP_LOG_INFO("app",
                    QStringLiteral("Overriding log level from CLI: %1")
                        .arg(segment_puzzler::app_logging::AppLogger::levelName(*logLevelOverride)));
    }

// ideally, this would be allowed for user-defined fileio plugins, however, user-written plugins
// sometimes caused crashes when starting the program
    #ifdef _WIN32
        _putenv("ITK_AUTOLOAD_PATH=");
        SP_LOG_INFO("app", QStringLiteral("ITK_AUTOLOAD_PATH=%1").arg(QString::fromStdString(getEnvVar("ITK_AUTOLOAD_PATH"))));
    #else 
        setenv("ITK_AUTOLOAD_PATH", "", 1);
        SP_LOG_INFO("app", QStringLiteral("ITK_AUTOLOAD_PATH=%1").arg(QString::fromUtf8(getenv("ITK_AUTOLOAD_PATH"))));
    #endif

#ifdef USE_OMP
#if defined(_OPENMP) && _OPENMP >= 200805
    omp_set_max_active_levels(1);
#endif
    SP_LOG_INFO("app", QStringLiteral("OpenMP enabled, defaultThreads=%1").arg(segment_puzzler::workerThreadCount()));
#else
    SP_LOG_INFO("app", QStringLiteral("OpenMP disabled"));
#endif
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(
        segment_puzzler::workerThreadCount(segment_puzzler::ThreadBackend::ITK));
    configureVtkSmpBackend();
    qRegisterMetaType<QVector<int> >("QVector<int>");
    qRegisterMetaType<QItemSelection>("QItemSelection");

    QFile f(":qdarkstyle/dark/style.qss");
    if (!f.exists()) {
        SP_LOG_WARNING("app", QStringLiteral("Unable to set stylesheet because :qdarkstyle/dark/style.qss was not found"));
    } else if (!f.open(QFile::ReadOnly | QFile::Text)) {
        SP_LOG_WARNING("app",
                       QStringLiteral("Unable to open stylesheet :qdarkstyle/dark/style.qss: %1")
                               .arg(f.errorString()));
    } else {
        QTextStream ts(&f);
        a.setStyleSheet(ts.readAll());
    }

    auto *myMainWindow = new MainWindow();
    auto window_icon = QIcon(":images/icon.png");
    myMainWindow->setWindowIcon(window_icon);
    myMainWindow->show();
    QTimer::singleShot(0, myMainWindow, [myMainWindow]() {
        SP_LOG_INFO("app",
                    QStringLiteral("Qt runtime=%1 backend=%2 windowDpr=%3 logicalDpi=%4x%5")
                        .arg(QString::fromLatin1(qVersion()), QGuiApplication::platformName())
                        .arg(myMainWindow->devicePixelRatioF(), 0, 'f', 3)
                        .arg(myMainWindow->logicalDpiX())
                        .arg(myMainWindow->logicalDpiY()));
    });

    loadStartupVolumes(myMainWindow->mySignalControl, startupVolumes);

    int val = 0;
    try {
        val = QApplication::exec();
    } catch (const std::exception& e){
        SP_LOG_ERROR("app", QStringLiteral("Unhandled application exception: %1").arg(QString::fromUtf8(e.what())));
    }
    return val;
}
