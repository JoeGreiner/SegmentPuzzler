#ifndef HELLOWORLD_SIGNALCONTROL_H
#define HELLOWORLD_SIGNALCONTROL_H


#include <QString>
#include <type_traits>
#include <functional>
#include <optional>
#include <itkImage.h>
#include <itkDataObject.h>
#include <src/viewers/itkSignal.h>
#include <QAction>
#include <QWidget>
#include <QSplitter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <itkImageIOBase.h>
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/graphBase.h"

#include <src/qtUtils/QImageSelectionRadioButtons.h>
#include <src/qtUtils/QBackgroundIdRadioBox.h>

class OrthoViewer;
class TaskRunner;
class QMenu;
class QLabel;
class QVBoxLayout;

class SignalControl : public QWidget {
Q_OBJECT
public:
    //TODO: Fix file handling. dataype + signal index should be enough as an unique identifier.
    SignalControl(std::shared_ptr<GraphBase> graphBaseIn,
                  OrthoViewer *orthoViewerIn,
                  TaskRunner *taskRunnerIn,
                  QWidget *parent = nullptr,
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

    itkSignalBase *segmentsGraph;

    QTreeWidget *signalTreeWidget;
    QTreeWidget *probabilityTreeWidget;
    QTreeWidget *segmentationTreeWidget;
    QTreeWidget *refinementTreeWidget;

    bool loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                   size_t &signalIndexGlobalOut, bool forceShapeOfSegments = true,
                   bool forceSegmentDataTypeUInt = false,
                   itk::ImageIOBase::IOComponentType forcedDataType = kSegmentLoadIOType);

    void addImageAsync(QString fileName, QString displayedName, LoadCallback then = {});
    void loadSegmentationVolume(QString fileName, QString displayedName="", LoadCallback then = {});
    void loadMembraneProbabilityAsync(QString fileName, QString displayedName, LoadCallback then = {});
    void addSegmentsGraphAsync(QString fileName, LoadCallback then = {});
    void loadRefinementAsync(QString fileName, QString displayedName, LoadCallback then = {});

    bool hasWorkingSegments() const;
    void populateAddDataMenu(QMenu *menu, QAction *loadSampleDataAction);
    void populateBoundariesMenu(QMenu *menu);
    void populateRefinementsMenu(QMenu *menu);
    void populateSegmentationsMenu(QMenu *menu);


public slots:
    void handleDroppedFile(QString fileName);

    void toggleROISelection();

    void loadRefinementPressed();

    void createEmptySegmentation();

    void loadSegmentationVolumePressed();

    void exportSelectedSegmentation();

    void togglePaintMode();

    void loadMembraneProbability(QString fileName, QString displayedName="");

    void addSegmentsPressed();

    void addImagePressed();

    void loadMembraneProbabilityPressed();

    void addSegmentsGraph(QString fileName);

    void addImage(QString fileName, QString displayedName="");

    void loadRefinement(QString fileName);
    void loadRefinement(QString fileName, QString displayedName);

    void mergeSupervoxelsByRefinementLabel();

    void treeDoubleClicked(QTreeWidgetItem *item, int index);

    void treeClicked(QTreeWidgetItem *item, int index);

    void refinementClicked(QTreeWidgetItem *item, int index);

    void segmentationClicked(QTreeWidgetItem *item, int index);
    void boundaryClicked(QTreeWidgetItem *item, int index);

    void transferSegmentsWithVolume();

    void transferAllSegments();

    void transferSupervoxelsByRefinementOverlap();

    void setTransparentLabelIdInRefinement();

    void runWatershed();

    void receiveNewRefinement(itk::Image<dataType::SegmentIdType, 3>::Pointer);

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

    struct SegmentationLoadResultData {
        GraphSegmentImageType::Pointer segmentationImage;
        GraphSegmentImageType::Pointer workingSegmentsImage;
    };

    unsigned int getSignalIndex(QTreeWidgetItem *item);

    bool getIsUChar(QTreeWidgetItem *item);

    bool getIsShort(QTreeWidgetItem *item);

    bool getIsEdge(QTreeWidgetItem *item);

    bool getIsSegments(QTreeWidgetItem *item);

    void getSignalPropsFromItem(QTreeWidgetItem *item, bool &isShort, bool &isUChar, bool &isSegments, bool &isEdge,
                                unsigned int &signalIndex);

    bool verbose;
    bool guiBusy = false;

    QSplitter *sectionSplitter = nullptr;
    QPushButton *togglePaintBrushButton = nullptr;
    QPushButton *setPaintIdButton = nullptr;
    QPushButton *toggleROISelectionButton = nullptr;
    QPushButton *runWatershedButton = nullptr;
    QPushButton *exportSegmentationButton = nullptr;
    QLabel *selectedBoundaryLabel = nullptr;
    QLabel *selectedRefinementLabel = nullptr;
    QLabel *selectedSegmentationLabel = nullptr;
    QTreeWidgetItem *lastAutoSelectedBoundaryItem = nullptr;
    QTreeWidgetItem *lastAutoSelectedRefinementItem = nullptr;
    QTreeWidgetItem *lastAutoSelectedSegmentationItem = nullptr;

    QAction *addImageAction = nullptr;
    QAction *addSegmentsAction = nullptr;
    QAction *addBoundariesAction = nullptr;
    QAction *loadRefinementAction = nullptr;

    QAction *createEmptySegmentationAction = nullptr;
    QAction *loadSegmentationAction = nullptr;
    QAction *exportSegmentationAction = nullptr;

    QAction *runWatershedAction = nullptr;
    QAction *mergeWithRefinementAction = nullptr;
    QAction *setIdTransparentAction = nullptr;

    QAction *toggleROISelectionAction = nullptr;
    QAction *togglePaintModeAction = nullptr;
    QAction *setPaintIdAction = nullptr;

    QAction *transferWithVolumeAction = nullptr;
    QAction *transferAllAction = nullptr;
    QAction *transferWithRefinementAction = nullptr;

    bool roiSelectionActive = false;
    bool paintModeActive = false;

    QString DEFAULT_SAVE_DIR;

    void askForBackgroundStrategy();
    void loadDroppedFileAs(QString fileName, ImageLoadChoice loadChoice);

    void setupSignalTreeWidget();

    void setupRefinementTreeWidget();

    void setupSegmentationTreeWidget();

    void setupProbabilityTreeWidget();
    QVBoxLayout *createSectionLayout(const QString &title);
    void createMenuActions();
    void updateModeActionTexts();
    void setROISelectionActive(bool active);
    void setPaintModeActive(bool active);
    void refreshUiState();
    void updateSelectionLabel(QTreeWidget *tree, QLabel *label);
    void selectLoadedItemIfAppropriate(QTreeWidget *tree, QTreeWidgetItem *newItem, QTreeWidgetItem *&lastAutoSelectedItem);
    void selectBoundaryItem(QTreeWidgetItem *item);
    void selectRefinementItem(QTreeWidgetItem *item);
    void selectSegmentationItem(QTreeWidgetItem *item);
    void showInfoMessage(const QString &message) const;
    bool hasSelectedSegmentation() const;
    bool hasSelectedRefinement() const;
    bool hasSelectedBoundary() const;






//    typedef  enum { UNKNOWNCOMPONENTTYPE, UCHAR, CHAR, USHORT, SHORT, UINT, INT,
//        ULONG, LONG, ULONGLONG, LONGLONG, FLOAT, DOUBLE } IOComponentType;



    bool getDimensionMatchWithSegmentImage();
    void setGuiBusy(bool busy);
    void refreshViewers();
    QString resolvedDisplayName(const QString &fileName, const QString &displayedName) const;
    void invokeLoadCallbackLater(LoadCallback then, LoadResult result);
    GraphSegmentImageType::Pointer duplicateSegmentationAndBuildWorkingSegments(const GraphSegmentImageType::Pointer &segmentationImage);

    LoadedImageData loadImageData(QString fileName,
                                  bool forceSegmentDataTypeUInt = false,
                                  itk::ImageIOBase::IOComponentType forcedDataType = kSegmentLoadIOType);
    bool insertLoadedImage(const LoadedImageData &loadedImage,
                           size_t &signalIndexGlobalOut,
                           bool forceShapeOfSegments);
    void loadSegmentationVolumeAsync(QString fileName,
                                     QString displayedName,
                                     bool createWorkingSegments,
                                     LoadCallback then);

    void registerImageSignal(size_t signalIndexGlobal, const QString &name);
    void registerSegmentationSignal(size_t signalIndexGlobal, const QString &name);
    void registerBoundarySignal(size_t signalIndexGlobal, const QString &name);
    void registerRefinementSignal(size_t signalIndexGlobal, const QString &name);
    void registerSegmentsGraphSignal(size_t signalIndexGlobal, bool createSegmentationVolume = true);

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
