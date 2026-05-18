#include <QApplication>
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
#include <mainWindow.h>
#include <QMetaType>
#include <sys/stat.h>
#include <cstdlib>
#include <clocale>
#include <optional>

#include "src/utils/AppLogger.h"

bool return_string_if_valid_option(int argc, char *argv[], int requested_index){
    if (requested_index >= argc){
        return false;
    }
    std::string parameterToParse = std::string(argv[requested_index]);
    std::string firstTwoCharacters = parameterToParse.substr(0, 2);
    return firstTwoCharacters != "--";
}

bool checkIfPathExists(const QString &s)
{
    struct stat buffer{};
    return (stat (s.toStdString().c_str(), &buffer) == 0);
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


int main(int argc, char *argv[]) {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication a(argc, argv);

    std::setlocale(LC_NUMERIC, "C");

    QApplication::setApplicationName("SegmentPuzzler");
    QApplication::setOrganizationName("JoeGreiner");
    QApplication::setOrganizationDomain("joegreiner.de");

    std::optional<segment_puzzler::app_logging::LogLevel> logLevelOverride;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--log-level") {
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for --log-level. Expected one of: trace, debug, info, warning, error.\n";
            return 1;
        }

        const QString rawLogLevel = QString::fromUtf8(argv[i + 1]);
        logLevelOverride = parseLogLevelOverride(rawLogLevel);
        if (!logLevelOverride.has_value()) {
            std::cerr << "Invalid value for --log-level: " << rawLogLevel.toStdString()
                      << ". Expected one of: trace, debug, info, warning, error.\n";
            return 1;
        }
        ++i;
    }

    segment_puzzler::app_logging::AppLogger::initialize();
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
    SP_LOG_INFO("app", QStringLiteral("OpenMP enabled, maxThreads=%1").arg(omp_get_max_threads()));
#else
    SP_LOG_INFO("app", QStringLiteral("OpenMP disabled"));
#endif
    qRegisterMetaType<QVector<int> >("QVector<int>");
    qRegisterMetaType<QItemSelection>("QItemSelection");

    QFile f(":qdarkstyle/dark/style.qss");
    if (!f.exists()) {
        SP_LOG_WARNING("app", QStringLiteral("Unable to set stylesheet because :qdarkstyle/dark/style.qss was not found"));
    } else {
        f.open(QFile::ReadOnly | QFile::Text);
        QTextStream ts(&f);
        a.setStyleSheet(ts.readAll());
    }

    auto *myMainWindow = new MainWindow();
    auto window_icon = QIcon(":images/icon.png");
    myMainWindow->setWindowIcon(window_icon);
    myMainWindow->show();

    bool loadSegment = false, segmentNameIsGiven = false;
    bool imageNameIsGiven = false;
    bool segmentationNameIsGiven = false;
    bool boundaryNameIsGiven = false;
    bool refinementNameIsGiven = false;

    QString pathToSegment, segmentName;
    QString pathToImage, imageName;
    QString pathToSegmentation, segmentationName;
    QString pathToBoundary, boundaryName;
    QString pathToRefinement, refinementName;

    if (argc >= 2) {
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--help") {
                std::cout << "SegmentPuzzler\n"
                             "\t\t [--segments $path_to_segments [$display_name_segments]]\n";
                std::cout << "\t\t [--image $path_to_image [$display_name_image]]\n";
                std::cout << "\t\t [--segmentation $path_to_segmentation [$display_name_segmentation]]\n";
                std::cout << "\t\t [--boundary $path_to_boundary [$display_name_boundary]]\n";
                std::cout << "\t\t [--refinement $path_to_refinement [$display_name_refinement]]\n";
                std::cout << "\t\t [--log-level trace|debug|info|warning|error]\n";
            }
        }
    }

    if (argc >= 2) {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = std::string(argv[i]);
            if ((argument == "--segments") && (i + 1 < argc)) {
                loadSegment = true;
                pathToSegment = QString(argv[i+1]);
                SP_LOG_INFO("app", QStringLiteral("Startup segments path=%1").arg(pathToSegment));
                segmentNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                if (segmentNameIsGiven) {
                    segmentName = argv[i+2];
                    SP_LOG_INFO("app", QStringLiteral("Startup segments displayName=%1").arg(segmentName));
                }
                if (!checkIfPathExists(pathToSegment)) {
                    SP_LOG_WARNING("app", QStringLiteral("Cannot access startup segments path=%1, skipping").arg(pathToSegment));
                    pathToSegment.clear();
                }
            } else if ((argument == "--image") && (i + 1 < argc)) {
                pathToImage = QString(argv[i+1]);
                SP_LOG_INFO("app", QStringLiteral("Startup image path=%1").arg(pathToImage));
                imageNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                if (imageNameIsGiven) {
                    imageName = argv[i+2];
                    SP_LOG_INFO("app", QStringLiteral("Startup image displayName=%1").arg(imageName));
                }
                if (!checkIfPathExists(pathToImage)) {
                    SP_LOG_WARNING("app", QStringLiteral("Cannot access startup image path=%1, skipping").arg(pathToImage));
                    pathToImage.clear();
                }
            } else if ((argument == "--segmentation") && (i + 1 < argc)) {
                pathToSegmentation = QString(argv[i+1]);
                SP_LOG_INFO("app", QStringLiteral("Startup segmentation path=%1").arg(pathToSegmentation));
                segmentationNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                if (segmentationNameIsGiven) {
                    segmentationName = argv[i+2];
                    SP_LOG_INFO("app", QStringLiteral("Startup segmentation displayName=%1").arg(segmentationName));
                }
                if (!checkIfPathExists(pathToSegmentation)) {
                    SP_LOG_WARNING("app", QStringLiteral("Cannot access startup segmentation path=%1, skipping").arg(pathToSegmentation));
                    pathToSegmentation.clear();
                }
            } else if ((argument == "--boundary") && (i + 1 < argc)) {
                pathToBoundary = QString(argv[i+1]);
                SP_LOG_INFO("app", QStringLiteral("Startup boundary path=%1").arg(pathToBoundary));
                boundaryNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                if (boundaryNameIsGiven) {
                    boundaryName = argv[i+2];
                    SP_LOG_INFO("app", QStringLiteral("Startup boundary displayName=%1").arg(boundaryName));
                }
                if (!checkIfPathExists(pathToBoundary)) {
                    SP_LOG_WARNING("app", QStringLiteral("Cannot access startup boundary path=%1, skipping").arg(pathToBoundary));
                    pathToBoundary.clear();
                }
            } else if ((argument == "--refinement") && (i + 1 < argc)) {
                pathToRefinement = QString(argv[i+1]);
                SP_LOG_INFO("app", QStringLiteral("Startup refinement path=%1").arg(pathToRefinement));
                refinementNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                if (refinementNameIsGiven) {
                    refinementName = argv[i+2];
                    SP_LOG_INFO("app", QStringLiteral("Startup refinement displayName=%1").arg(refinementName));
                }
                if (!checkIfPathExists(pathToRefinement)) {
                    SP_LOG_WARNING("app", QStringLiteral("Cannot access startup refinement path=%1, skipping").arg(pathToRefinement));
                    pathToRefinement.clear();
                }
            }
        }
    }

    if (loadSegment && !pathToSegment.isEmpty()) {
        myMainWindow->mySignalControl->addSegmentsGraphAsync(
            pathToSegment,
            [myMainWindow,
             pathToImage,
             imageName,
             pathToSegmentation,
             segmentationName,
             pathToBoundary,
             boundaryName,
             pathToRefinement,
             refinementName](SignalControl::LoadResult segmentsIndex) {
                if (!segmentsIndex) {
                    return;
                }

                auto loadRefinement = [myMainWindow, pathToRefinement, refinementName]() {
                    if (!pathToRefinement.isEmpty()) {
                        myMainWindow->mySignalControl->loadRefinement(pathToRefinement, refinementName);
                    }
                };

                auto loadBoundary = [myMainWindow, pathToBoundary, boundaryName, loadRefinement]() {
                    if (!pathToBoundary.isEmpty()) {
                        myMainWindow->mySignalControl->loadMembraneProbabilityAsync(
                            pathToBoundary,
                            boundaryName,
                            SignalControl::BoundaryLoadMode::BoundaryOnly,
                            SignalControl::FloatBoundaryConversionMode::CastValues,
                            [loadRefinement](SignalControl::LoadResult boundaryIndex) {
                                if (!boundaryIndex) {
                                    return;
                                }
                                loadRefinement();
                            });
                    } else {
                        loadRefinement();
                    }
                };

                auto loadSegmentation = [myMainWindow, pathToSegmentation, segmentationName, loadBoundary]() {
                    if (!pathToSegmentation.isEmpty()) {
                        myMainWindow->mySignalControl->loadSegmentationVolume(
                            pathToSegmentation,
                            segmentationName,
                            [loadBoundary](SignalControl::LoadResult segmentationIndex) {
                                if (!segmentationIndex) {
                                    return;
                                }
                                loadBoundary();
                            });
                    } else {
                        loadBoundary();
                    }
                };

                if (!pathToImage.isEmpty()) {
                    myMainWindow->mySignalControl->addImageAsync(
                        pathToImage,
                        imageName,
                        [loadSegmentation](SignalControl::LoadResult imageIndex) {
                            if (!imageIndex) {
                                return;
                            }
                            loadSegmentation();
                        });
                } else {
                    loadSegmentation();
                }
            });
    } else if (!pathToImage.isEmpty()) {
        myMainWindow->mySignalControl->addImageAsync(pathToImage, imageName);
    }

    int val = 0;
    try {
        val = QApplication::exec();
    } catch (const std::exception& e){
        SP_LOG_ERROR("app", QStringLiteral("Unhandled application exception: %1").arg(QString::fromUtf8(e.what())));
    }
    return val;
}
