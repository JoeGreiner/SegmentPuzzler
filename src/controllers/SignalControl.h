#ifndef HELLOWORLD_SIGNALCONTROL_H
#define HELLOWORLD_SIGNALCONTROL_H


#include <QString>
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include <QWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <itkImageIOBase.h>
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/graphBase.h"

#include "src/qtUtils/QTreeWidgetWithDragAndDrop.h"
#include <src/qtUtils/QImageSelectionRadioButtons.h>


class SignalControl : public QTabWidget {
Q_OBJECT
public:
    //TODO: Fix file handling. dataype + signal index should be enough as an unique identifier.
    SignalControl(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent = 0, bool verboseIn = true);

    ~SignalControl();

    std::shared_ptr<GraphBase> graphBase;

    using GraphSegmentType = dataType::SegmentIdType;
    using GraphSegmentImageType = dataType::SegmentsImageType;

    void addEmptySegmentsFromBoundary();
    void addSegmentsGraph(QString &fileName);
    void initializeGraph(size_t signalIndexLocal, size_t signalIndexGlobal);


//    void ITKLoaderUnknownFileType(QString &fileName);
    void setUserColor(QTreeWidgetItem *item);

    void setUserNorm(QTreeWidgetItem *item);

    void setIsActive(QTreeWidgetItem *item, bool isActiveIn);

    void setUserAlpha(QTreeWidgetItem *item);

    void setDescription(QTreeWidgetItem *item);


    // ATTENTION: if you link the pointer to uCharImageList, and not to uCharImageList.at(0),
    // the pointer gets invalidated if you resize the vector!!!!
//    std::vector<itkSignal<unsigned char>> uCharSignalList;
//    std::vector<itkSignal<short>> shortSignalList;
    //TODO: isn't there a better way to do this? virtual baseclass?
    std::vector<itk::Image<unsigned char, 3>::Pointer> uCharImageList;
    std::vector<itk::Image<char, 3>::Pointer> charImageList;
    std::vector<itk::Image<unsigned short, 3>::Pointer> uShortImageList;
    std::vector<itk::Image<short, 3>::Pointer> shortImageList;
    std::vector<itk::Image<unsigned int, 3>::Pointer> uIntImageList;
    std::vector<itk::Image<int, 3>::Pointer> intImageList;
    std::vector<itk::Image<unsigned long, 3>::Pointer> uLongImageList;
    std::vector<itk::Image<long, 3>::Pointer> longImageList;
    std::vector<itk::Image<unsigned long long, 3>::Pointer> uLongLongImageList;
    std::vector<itk::Image<long long, 3>::Pointer> longLongImageList;
    std::vector<itk::Image<float, 3>::Pointer> floatImageList;
    std::vector<itk::Image<double, 3>::Pointer> doubleImageList;

    // do we actually need to save the signals? or can we just use the abstract class instead?
    std::vector<std::unique_ptr<itkSignal<unsigned char>>> uCharSignalList;
    std::vector<std::unique_ptr<itkSignal<char>>> charSignalList;
    std::vector<std::unique_ptr<itkSignal<unsigned short>>> uShortSignalList;
    std::vector<std::unique_ptr<itkSignal<short>>> shortSignalList;
    std::vector<std::unique_ptr<itkSignal<unsigned int>>> uIntSignalList;
    std::vector<std::unique_ptr<itkSignal<int>>> intSignalList;
    std::vector<std::unique_ptr<itkSignal<unsigned long>>> uLongSignalList;
    std::vector<std::unique_ptr<itkSignal<long>>> longSignalList;
    std::vector<std::unique_ptr<itkSignal<unsigned long long>>> uLongLongSignalList;
    std::vector<std::unique_ptr<itkSignal<long long>>> longLongSignalList;
    std::vector<std::unique_ptr<itkSignal<float>>> floatSignalList;
    std::vector<std::unique_ptr<itkSignal<double>>> doubleSignalList;

    std::vector<std::unique_ptr<itkSignal<dataType::SegmentIdType>>> *pSegmentTypeSignalList;
    std::vector<itk::Image<dataType::SegmentIdType, 3>::Pointer> *pSegmentTypeImageList;

    std::vector<itkSignalBase *> allSignalList;


    std::vector<itkSignal<dataType::SegmentIdType>> refinementWatershedList;

    itkSignal<GraphSegmentType> *segmentsGraph;

    bool loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
                   size_t &signalIndexLocalOut, size_t &signalIndexGlobalOut, bool forceShapeOfSegments = true,
                   bool forceSegmentDataTypeUInt = false,
                   itk::ImageIOBase::IOComponentType forcedDataType = itk::ImageIOBase::IOComponentType::UINT);


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

    void addImage(QString fileName, QString displayedName="");

    void addRefinementWatershed(QString fileName, QString displayedName="");

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
    size_t &signalIndexLocalOut, size_t &signalIndexGlobalOut,
    bool forceShapeOfSegments = true);



private:
    unsigned int getSignalIndex(QTreeWidgetItem *item);

    bool getIsUChar(QTreeWidgetItem *item);

    bool getIsShort(QTreeWidgetItem *item);

    bool getIsEdge(QTreeWidgetItem *item);

    bool getIsSegments(QTreeWidgetItem *item);

    void exportSelectedSegmentationSurface();

    void getSignalPropsFromItem(QTreeWidgetItem *item, bool &isShort, bool &isUChar, bool &isSegments, bool &isEdge,
                                unsigned int &signalIndex);

    bool verbose;

    QVBoxLayout *signalControlLayout;

    // overlay tree widget
    QTreeWidgetWithDragAndDrop *signalTreeWidget;
    QWidget *signalInputButtonsWidget;
    QGridLayout *signalInputButtonsLayout;
    QPushButton *addSignalButton;
    QPushButton *addSegmentsButton;

    // refinement watershed tree
    QTreeWidgetWithDragAndDrop *refinementWatershedTreeWidget;
    QWidget *refinementWatershedInputButtonsWidget;
    QGridLayout *refinementWatershedButtonLayout;
    QPushButton *addRefinementWatershedButton;
    QPushButton *mergeWithRefinementWatershedButton;
    QPushButton *replaceSegmentByPositionButton;
    QPushButton *replaceSegmentBySegmentButton;
    QPushButton *setIdToTransparentInRefinementWSButton;

    // segmentation tree widget
    QTreeWidget *segmentationTreeWidget;
    QWidget *segmentationButtonWidget;
    QGridLayout *segmentationButtonWidgetGridLayout;
    QPushButton *addSegmentationButton;
    QPushButton *exportSegmentationButton;
    QPushButton *loadSegmentationButton;
    QPushButton *exportSegmentationSurfaceButton;
    QPushButton *togglePaintBrushButton;
    QPushButton *transferSegmentsWithVolumeButton;
    QPushButton *transferSegmentsWithRefinementButton;
    QPushButton *transferAllSegmentsButton;


    // probabilities tree widget
    QTreeWidget *probabilityTreeWidget;
    QWidget *probabilityButtonWidget;
    QGridLayout *probabilityButtonWidgetLayout;
    QPushButton *addMembraneProbabilityButton;
    QPushButton *runWatershedButton;
    QPushButton *selectROIRefinementButton;

    QImageSelectionRadioButtons* imageSelectionButtonWidget;

    std::map<size_t, size_t> globalToLocalMapping; // mapping: local index of <dtype>Array -> globalIndex of allSignalLists


    QString DEFAULT_SAVE_DIR;

    void setupSignalTreeWidget();

    void setupRefinementWatershedTreeWidget();

    void setupSegmentationTreeWidget();

    void setupProbabilityTreeWidget();




//    typedef  enum { UNKNOWNCOMPONENTTYPE, UCHAR, CHAR, USHORT, SHORT, UINT, INT,
//        ULONG, LONG, ULONGLONG, LONGLONG, FLOAT, DOUBLE } IOComponentType;



    bool getDimensionMatchWithSegmentImage();
};


#endif //HELLOWORLD_SIGNALCONTROL_H
