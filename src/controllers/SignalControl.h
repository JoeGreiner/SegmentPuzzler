#ifndef HELLOWORLD_SIGNALCONTROL_H
#define HELLOWORLD_SIGNALCONTROL_H


#include <QString>
#include <type_traits>
#include <functional>
#include <optional>
#include <itkImage.h>
#include <itkDataObject.h>
#include <src/viewers/itkSignal.h>
#include <QWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <itkImageIOBase.h>
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/graphBase.h"

#include "src/qtUtils/QTreeWidgetWithDragAndDrop.h"
#include <src/qtUtils/QImageSelectionRadioButtons.h>
#include <src/qtUtils/QBackgroundIdRadioBox.h>

class OrthoViewer;
class TaskRunner;

class SignalControl : public QTabWidget {
Q_OBJECT
public:
    //TODO: Fix file handling. dataype + signal index should be enough as an unique identifier.
    SignalControl(std::shared_ptr<GraphBase> graphBaseIn,
                  OrthoViewer *orthoViewerIn,
                  TaskRunner *taskRunnerIn,
                  QWidget *parent = 0,
                  bool verboseIn = true);

    ~SignalControl();

    std::shared_ptr<GraphBase> graphBase;
    OrthoViewer *orthoViewer;
    TaskRunner *taskRunner;

    using GraphSegmentType = dataType::SegmentIdType;
    using GraphSegmentImageType = dataType::SegmentsImageType;
    using LoadResult = std::optional<size_t>;
    using LoadCallback = std::function<void(LoadResult)>;

    // Maps the compiled SegmentIdType to its ITK IOComponentType.
    // Ensures segment files are always forced to load with the pixel type
    // that matches the current build (SEGMENTUINT → UINT, SEGMENTSHORT → SHORT).
    static constexpr itk::ImageIOBase::IOComponentType kSegmentLoadIOType =
        std::is_same_v<dataType::SegmentIdType, unsigned int>
            ? itk::ImageIOBase::IOComponentType::UINT
            : std::is_same_v<dataType::SegmentIdType, short>
                ? itk::ImageIOBase::IOComponentType::SHORT
                : itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE;
    static_assert(kSegmentLoadIOType != itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE,
                  "SegmentIdType has no corresponding IOComponentType — add it to kSegmentLoadIOType");

    void addEmptySegmentsFromBoundary();
    void initializeGraph(size_t signalIndexGlobal);


//    void ITKLoaderUnknownFileType(QString &fileName);
    void setUserColor(QTreeWidgetItem *item);

    void setUserNorm(QTreeWidgetItem *item);

    void setIsActive(QTreeWidgetItem *item, bool isActiveIn);

    void setUserAlpha(QTreeWidgetItem *item);

    void setDescription(QTreeWidgetItem *item);

    // Owns all signals created during image loading.
    // allSignalList is a non-owning view used for indexed access by tree-widget callbacks.
    std::vector<std::unique_ptr<itkSignalBase>> ownedSignals;
    std::vector<itkSignalBase *> allSignalList;

    std::vector<itkSignal<dataType::SegmentIdType>> refinementWatershedList;

    itkSignalBase *segmentsGraph;

    QTreeWidgetWithDragAndDrop *signalTreeWidget;
    QTreeWidget *probabilityTreeWidget;
    QTreeWidget *segmentationTreeWidget;
    QTreeWidgetWithDragAndDrop *refinementWatershedTreeWidget;



    bool loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                   size_t &signalIndexGlobalOut, bool forceShapeOfSegments = true,
                   bool forceSegmentDataTypeUInt = false,
                   itk::ImageIOBase::IOComponentType forcedDataType = kSegmentLoadIOType);

    void addImageAsync(QString fileName, QString displayedName, LoadCallback then = {});
    void loadSegmentationVolumeAsync(QString fileName, QString displayedName, LoadCallback then = {});
    void loadMembraneProbabilityAsync(QString fileName, QString displayedName, LoadCallback then = {});
    void addSegmentsGraphAsync(QString fileName, LoadCallback then = {});
    void addRefinementWatershedAsync(QString fileName, QString displayedName, LoadCallback then = {});


public slots:
    void loadFileFromDragAndDropTriggered(QString fileName);
    void loadFileFromDragAndDrop(QString fileName, QString choiceOfImage);

    void selectROIRefinementPressed();

    void addRefinementWatershedPressed();

    void createNewSegmentationVolume();

    void loadSegmentationVolume(QString fileName, QString displayedName="");

    void loadSegmentationVolumePressed();

    void exportSelectedSegmentation();

    void togglePaintMode();

    void loadMembraneProbability(QString fileName, QString displayedName="");

    void addSegmentsPressed();

    void addImagePressed();

    void loadMembraneProbabilityPressed();

    void addSegmentsGraph(QString fileName);

    void addImage(QString fileName, QString displayedName="");

    void addRefinementWatershed(QString fileName);
    void addRefinementWatershed(QString fileName, QString displayedName);

    void mergeSegmentsWithRefinementWatershedClicked();

    void treeDoubleClicked(QTreeWidgetItem *item, int index);

    void treeClicked(QTreeWidgetItem *item, int index);

    void watershedClicked(QTreeWidgetItem *item, int index);

    void segmentationClicked(QTreeWidgetItem *item, int index);

    void transferSegmentsWithVolume();

    void transferAllSegments();

    void transferSegmentsWithRefinementWS();

    void setIdToTransparentInRefinementWS();

    void runWatershed();

    void receiveNewRefinementWatershed(itk::Image<dataType::SegmentIdType, 3>::Pointer);

    bool insertImageSegmenttype(itk::Image<dataType::SegmentIdType, 3>::Pointer pImage,
    size_t &signalIndexGlobalOut,
    bool forceShapeOfSegments = true);

    void setPaintId();



private:
    struct LoadedImageData {
        itk::ImageIOBase::IOComponentType dataType = itk::ImageIOBase::IOComponentType::UNKNOWNCOMPONENTTYPE;
        itk::DataObject::Pointer image;
    };

    struct BoundaryLoadResult {
        dataType::BoundaryImageType::Pointer boundaryImage;
        dataType::SegmentsImageType::Pointer emptySegmentsImage;
        bool createdEmptySegments = false;
    };

    unsigned int getSignalIndex(QTreeWidgetItem *item);

    bool getIsUChar(QTreeWidgetItem *item);

    bool getIsShort(QTreeWidgetItem *item);

    bool getIsEdge(QTreeWidgetItem *item);

    bool getIsSegments(QTreeWidgetItem *item);

    void getSignalPropsFromItem(QTreeWidgetItem *item, bool &isShort, bool &isUChar, bool &isSegments, bool &isEdge,
                                unsigned int &signalIndex);

    bool verbose;

    QVBoxLayout *signalControlLayout;

    // overlay tree widget
    QWidget *signalInputButtonsWidget;
    QGridLayout *signalInputButtonsLayout;
    QPushButton *addSignalButton;
    QPushButton *addSegmentsButton;

    // refinement watershed tree
    QWidget *refinementWatershedInputButtonsWidget;
    QGridLayout *refinementWatershedButtonLayout;
    QPushButton *addRefinementWatershedButton;
    QPushButton *mergeWithRefinementWatershedButton;
    QPushButton *setIdToTransparentInRefinementWSButton;

    // segmentation tree widget
    QWidget *segmentationButtonWidget;
    QGridLayout *segmentationButtonWidgetGridLayout;
    QPushButton *addSegmentationButton;
    QPushButton *exportSegmentationButton;
    QPushButton *loadSegmentationButton;
    QPushButton *togglePaintBrushButton;
    QPushButton *setPaintIdButton;
    QPushButton *transferSegmentsWithVolumeButton;
    QPushButton *transferSegmentsWithRefinementButton;
    QPushButton *transferAllSegmentsButton;


    // probabilities tree widget
    QWidget *probabilityButtonWidget;
    QGridLayout *probabilityButtonWidgetLayout;
    QPushButton *addMembraneProbabilityButton;
    QPushButton *runWatershedButton;
    QPushButton *selectROIRefinementButton;

    QImageSelectionRadioButtons* imageSelectionButtonWidget;



    QString DEFAULT_SAVE_DIR;

    void askForBackgroundStrategy();

    void setupSignalTreeWidget();

    void setupRefinementWatershedTreeWidget();

    void setupSegmentationTreeWidget();

    void setupProbabilityTreeWidget();






//    typedef  enum { UNKNOWNCOMPONENTTYPE, UCHAR, CHAR, USHORT, SHORT, UINT, INT,
//        ULONG, LONG, ULONGLONG, LONGLONG, FLOAT, DOUBLE } IOComponentType;



    bool getDimensionMatchWithSegmentImage();
    void setGuiBusy(bool busy);
    void refreshViewers();
    QString resolvedDisplayName(const QString &fileName, const QString &displayedName) const;
    void invokeLoadCallbackLater(LoadCallback then, LoadResult result);

    LoadedImageData loadImageData(QString fileName,
                                  bool forceSegmentDataTypeUInt = false,
                                  itk::ImageIOBase::IOComponentType forcedDataType = kSegmentLoadIOType);
    bool insertLoadedImage(const LoadedImageData &loadedImage,
                           size_t &signalIndexGlobalOut,
                           bool forceShapeOfSegments);

    void registerImageSignal(size_t signalIndexGlobal, const QString &name);
    void registerSegmentationSignal(size_t signalIndexGlobal, const QString &name);
    void registerBoundarySignal(size_t signalIndexGlobal, const QString &name);
    void registerRefinementSignal(size_t signalIndexGlobal, const QString &name);
    void registerSegmentsGraphSignal(size_t signalIndexGlobal);

    template<typename T>
    bool insertTypedImage(
        typename itk::Image<T, 3>::Pointer  pImage,
        size_t                             &signalIndexGlobalOut,
        bool                                forceShapeOfSegments)
    {
        std::unique_ptr<itkSignal<T>> pSignal(new itkSignal<T>(pImage));
        if (pSignal->isShapeMatched(segmentsGraph) | !forceShapeOfSegments) {
            signalIndexGlobalOut = allSignalList.size();
            itkSignalBase *pRaw = pSignal.get();
            ownedSignals.push_back(std::move(pSignal));
            allSignalList.push_back(pRaw);
            return true;
        } else {
            std::cout << "Segments: [" << segmentsGraph->getDimX() << " "
                      << segmentsGraph->getDimY() << " " << segmentsGraph->getDimZ() << "]\n";
            std::cout << "Image:    [" << pSignal->getDimX() << " "
                      << pSignal->getDimY() << " " << pSignal->getDimZ() << "]\n";
            std::cout << "Dimension mismatch! Image is not added.\n";
            return false;
        }
    }
};


#endif //HELLOWORLD_SIGNALCONTROL_H
