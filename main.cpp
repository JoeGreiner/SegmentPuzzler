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

// ideally, this would be allowed for user-defined fileio plugins, however, user-written plugins
// sometimes caused crashes when starting the program
    #ifdef _WIN32
        _putenv("ITK_AUTOLOAD_PATH=");
        std::cout << "ITK_AUTOLOAD_PATH: " << getEnvVar("ITK_AUTOLOAD_PATH") << std::endl;
    #else 
        setenv("ITK_AUTOLOAD_PATH", "", 1);
        std::cout << "ITK_AUTOLOAD_PATH: " << getenv("ITK_AUTOLOAD_PATH") << std::endl;
    #endif

#ifdef USE_OMP
    omp_set_max_active_levels(1);
    std::cout << "OpenMP enabled, max threads: " << omp_get_max_threads() << std::endl;
#else
    std::cout << "OpenMP disabled" << std::endl;
#endif
    qRegisterMetaType<QVector<int> >("QVector<int>");
    qRegisterMetaType<QItemSelection>("QItemSelection");

    QFile f(":qdarkstyle/dark/style.qss");
    if (!f.exists()) {
        printf("Unable to set stylesheet, file not found\n");
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

    if(argc >= 2) {
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--help") {
                std::cout << "SegmentPuzzler\n"
                             "\t\t [--segments $path_to_segments [$display_name_segments]]\n";
                std::cout << "\t\t [--image $path_to_image [$display_name_image]]\n";
                std::cout << "\t\t [--segmentation $path_to_segmentation [$display_name_segmentation]]\n";
                std::cout << "\t\t [--boundary $path_to_boundary [$display_name_boundary]]\n";
                std::cout << "\t\t [--refinement $path_to_refinement [$display_name_refinement]]\n";
            }
        }
    }

    if(argc >= 2){
        for (int i = 1; i < argc; ++i) {
            if ((std::string(argv[i]) == "--segments") & (i+1 < argc)) {
                loadSegment=true;
                pathToSegment = QString(argv[i+1]);
                std::cout << "Path to segments: " << pathToSegment.toStdString() << "\n";
                segmentNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                if (segmentNameIsGiven){
                    segmentName = argv[i+2];
                    std::cout << "Segments name: " << segmentName.toStdString() << "\n";
                }
                if (!checkIfPathExists(pathToSegment)){
                    std::cout << "Can't access " << pathToSegment.toStdString() << " , skipping.\n";
                    pathToSegment.clear();
                }
            }
        }
    }

    if (loadSegment && !pathToSegment.isEmpty()) {
        if(argc >= 2){
            for (int i = 1; i < argc; ++i) {
                if ((std::string(argv[i]) == "--image") & (i+1 < argc)) {
                    pathToImage = QString(argv[i+1]);
                    std::cout << "Path to image: " << pathToImage.toStdString() << "\n";
                    imageNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                    if (imageNameIsGiven){
                        imageName = argv[i+2];
                        std::cout << "Image name: " << imageName.toStdString() << "\n";
                    }
                    if (!checkIfPathExists(pathToImage)){
                        std::cout << "Can't access " << pathToImage.toStdString() << " , skipping.\n";
                        pathToImage.clear();
                    }
                }
                if ((std::string(argv[i]) == "--segmentation") & (i+1 < argc)) {
                    pathToSegmentation = QString(argv[i+1]);
                    std::cout << "Path to segmentation: " << pathToSegmentation.toStdString() << "\n";
                    segmentationNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                    if (segmentationNameIsGiven){
                        segmentationName = argv[i+2];
                        std::cout << "Segmentation name: " << segmentationName.toStdString() << "\n";
                    }
                    if (!checkIfPathExists(pathToSegmentation)) {
                        std::cout << "Can't access " << pathToSegmentation.toStdString() << " , skipping.\n";
                        pathToSegmentation.clear();
                    }
                }
                if ((std::string(argv[i]) == "--boundary") & (i+1 < argc)) {
                    pathToBoundary = QString(argv[i+1]);
                    std::cout << "Path to boundary: " << pathToBoundary.toStdString() << "\n";
                    boundaryNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                    if (boundaryNameIsGiven){
                        boundaryName = argv[i+2];
                        std::cout << "boundary name: " << boundaryName.toStdString() << "\n";
                    }
                    if (!checkIfPathExists(pathToBoundary)) {
                        std::cout << "Can't access " << pathToBoundary.toStdString() << " , skipping.\n";
                        pathToBoundary.clear();
                    }
                }
                if ((std::string(argv[i]) == "--refinement") & (i+1 < argc)) {
                    pathToRefinement = QString(argv[i+1]);
                    std::cout << "Path to refinement: " << pathToRefinement.toStdString() << "\n";
                    refinementNameIsGiven = return_string_if_valid_option(argc, argv, i+2);
                    if (refinementNameIsGiven){
                        refinementName = argv[i+2];
                        std::cout << "Refinement name: " << refinementName.toStdString() << "\n";
                    }
                    if (!checkIfPathExists(pathToRefinement)) {
                        std::cout << "Can't access " << pathToRefinement.toStdString() << " , skipping.\n";
                        pathToRefinement.clear();
                    }
                }
            }
        }

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
    }

    int val = 0;
    try {
        val = QApplication::exec();
    } catch (const std::exception& e){
        std::cout << e.what() << std::endl;
    }
    return val;
}
