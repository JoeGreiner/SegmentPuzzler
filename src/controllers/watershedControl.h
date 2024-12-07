#ifndef SEGMENTCOUPLER_WATERSHEDCONTROL_H
#define SEGMENTCOUPLER_WATERSHEDCONTROL_H
#include <QString>
#include <itkImage.h>
#include <src/viewers/itkSignal.h>
#include <src/viewers/itkSignalThresholdPreview.h>
#include <QWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <itkImageIOBase.h>
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/graphBase.h"
#include "SignalControl.h"


#include <QTreeWidget>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QCheckBox>
#include <QSpinBox>


class QTreeWidgetWithDragAndDrop2 : public QTreeWidget {
Q_OBJECT
public:
    explicit QTreeWidgetWithDragAndDrop2(QTreeWidget *parent = 0) : QTreeWidget(parent) {
        setAcceptDrops(true);
    };


    void dragEnterEvent(QDragEnterEvent *e) override {
        if (e->mimeData()->hasUrls()) {
            e->acceptProposedAction();
        }
    }

    // seems to be needed that dropevent is fired
    void dragMoveEvent(QDragMoveEvent *e) override {
        if (e->mimeData()->hasUrls()) {
            e->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent *e) override {
        for (int i = 0; i < e->mimeData()->urls().size(); i++) {
            QUrl url = e->mimeData()->urls().at(i);
            QString fileName = url.toLocalFile();
            emit urlDropped(fileName);
        }
    }

signals:

    void urlDropped(QString url);

};





class WatershedControl : public QTabWidget {
Q_OBJECT
public:
    //TODO: Fix file handling. dataype + signal index should be enough as an unique identifier.
    WatershedControl(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent = 0, bool verboseIn = true);

    ~WatershedControl();

    SignalControl* linkedSignalControl;


    std::shared_ptr<GraphBase> graphBase;

    using GraphSegmentType = dataType::SegmentIdType;
    using GraphSegmentImageType = dataType::SegmentsImageType;

    void setUserColor(QTreeWidgetItem *item);

    void setUserNorm(QTreeWidgetItem *item);

    void setIsActive(QTreeWidgetItem *item, bool isActiveIn);

    void setUserAlpha(QTreeWidgetItem *item);

    void setDescription(QTreeWidgetItem *item);


    // ATTENTION: if you link the pointer to uCharImageList, and not to uCharImageList.at(0),
    // the pointer gets invalidated if you resize the vector!!!! This is a big FIXME!
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

    itkSignal<GraphSegmentType> *itkSignalSegmentsGraph;

    //TODO: make union
    itk::Image<unsigned short, 3>::Pointer pBoundaries;
    std::unique_ptr<itkSignalThresholdPreview<dataType::BoundaryVoxelType >> pBoundariesSignal;
    itk::Image<unsigned char, 3>::Pointer pThresholdedMembrane;
    itk::Image<float, 3>::Pointer pDistanceMap;
    itk::Image<unsigned int, 3>::Pointer pSeeds;
    itk::Image<unsigned int, 3>::Pointer pWatershed;

    void transferWatershedToGraph();

    bool loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
               size_t &signalIndexLocalOut, size_t &signalIndexGlobalOut, bool forceShapeOfSegments = true,
               bool forceSegmentDataType = false);

signals:
    void sendClosingSignal();

public slots:

    void togglePaintMode();
    void togglePaintBoundaryMode();

    void forwardValueChangedSignal(int value);
    void addImagePressed();

    void addBoundariesFromFile(QString fileName);
    void addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn);
    void addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn,
            int fxIn, int txIn, int fyIn, int tyIn, int fzIn, int tzIn);

    void thresholdBoundaries();

    void thresholdBoundariesPressed();

    void calculateDistanceMap();

    void calculateDistanceMapPressed();

    void extractSeeds();

    void extractSeedsPressed();

    void watershed();

    void watershedPressed();

    void exportSegments();

    void exportSegmentsPressed();

    void addImage(QString fileName);

    void treeDoubleClicked(QTreeWidgetItem *item, int index);

    void treeClicked(QTreeWidgetItem *item, int index);


private:
    unsigned int getSignalIndex(QTreeWidgetItem *item);

    bool getIsUChar(QTreeWidgetItem *item);

    bool getIsShort(QTreeWidgetItem *item);

    bool getIsEdge(QTreeWidgetItem *item);

    bool getIsSegments(QTreeWidgetItem *item);

    void getSignalPropsFromItem(QTreeWidgetItem *item, bool &isShort, bool &isUChar, bool &isSegments, bool &isEdge,
                                unsigned int &signalIndex);

    bool verbose;

    bool useROI;
    size_t fx, fy, fz, tx, ty, tz;

    QVBoxLayout *signalControlLayout;

    // overlay tree widget
    QTreeWidget *signalTreeWidget;
    QWidget *signalInputButtonsWidget;
    QGridLayout *signalInputButtonsLayout;
    QPushButton *thresholdBoundariesButton;
    QSlider* thresholdValueSlider;

    QTreeWidget *thresholdTreeWidget;
    QWidget *thresholdButtonsWidget;
    QGridLayout *thresholdButtonsLayout;
    QPushButton *calculateDistanceMapButton;
    QPushButton *togglePaintBoundaryModeButton;


    QTreeWidget *distanceMapTreeWidget;
    QWidget *distanceMapButtonsWidget;
    QGridLayout *distanceMapButtonsLayout;
    QPushButton *calculateSeedsButton;

    QTreeWidget *seedsTreeWidget;
    QWidget *seedsButtonsWidget;
    QGridLayout *seedsButtonsLayout;
    QCheckBox *checkBoxFiltering;
    QSpinBox *sizeFilteringInput;
    QPushButton *runWatershedButton;

    QTreeWidget *watershedTreeWidget;
    QWidget *watershedButtonsWidget;
    QGridLayout *watershedButtonsLayout;
    QPushButton *exportSegmentButton;

    std::map<size_t, size_t> globalToLocalMapping; // mapping: local index of <dtype>Array -> globalIndex of allSignalLists

    void setupSignalTreeWidget();
    void setupThresholdWidget();
    void setupDistanceMapWidget();
    void setupSeedWidget();
    void setupWatershedWidget();
    void setupWidget(QTreeWidget *treeWidget, QWidget *buttonsWidget, QGridLayout *buttonsLayout, QPushButton *button, QString tabName);

    bool getDimensionMatchWithSegmentImage();
};



#endif //SEGMENTCOUPLER_WATERSHEDCONTROL_H
