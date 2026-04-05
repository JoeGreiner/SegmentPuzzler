#ifndef SEGMENTCOUPLER_WATERSHEDCONTROL_H
#define SEGMENTCOUPLER_WATERSHEDCONTROL_H
#include <QString>
#include <functional>
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

class OrthoViewer;
class TaskRunner;

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
    WatershedControl(std::shared_ptr<GraphBase> graphBaseIn,
                     OrthoViewer *orthoViewerIn,
                     TaskRunner *taskRunnerIn,
                     QWidget *parent = 0,
                     bool verboseIn = true);

    ~WatershedControl();

    SignalControl* linkedSignalControl;


    std::shared_ptr<GraphBase> graphBase;
    OrthoViewer *orthoViewer;
    TaskRunner *taskRunner;

    using GraphSegmentType = dataType::SegmentIdType;
    using GraphSegmentImageType = dataType::SegmentsImageType;

    void setUserColor(QTreeWidgetItem *item);

    void setUserNorm(QTreeWidgetItem *item);

    void setIsActive(QTreeWidgetItem *item, bool isActiveIn);

    void setUserAlpha(QTreeWidgetItem *item);

    void setDescription(QTreeWidgetItem *item);

    // Owns all signals created during image loading.
    // allSignalList is a non-owning view used for indexed access by tree-widget callbacks.
    std::vector<std::unique_ptr<itkSignalBase>> ownedSignals;
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
               size_t &signalIndexGlobalOut, bool forceShapeOfSegments = true,
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

    void setupSignalTreeWidget();
    void setupThresholdWidget();
    void setupDistanceMapWidget();
    void setupSeedWidget();
    void setupWatershedWidget();
    void setupWidget(QTreeWidget *treeWidget, QWidget *buttonsWidget, QGridLayout *buttonsLayout, QPushButton *button, QString tabName);

    bool getDimensionMatchWithSegmentImage();
    void setGuiBusy(bool busy);
    void refreshViewers();
    void thresholdBoundariesAsync(std::function<void()> then = {});
    void calculateDistanceMapAsync(std::function<void()> then = {});
    void extractSeedsAsync(std::function<void()> then = {});
    void watershedAsync(std::function<void()> then = {});
    void exportSegmentsAsync(std::function<void()> then = {});

    // Registers a signal: takes ownership, appends to ownedSignals/allSignalList,
    // sets name/LUT/tree widget, adds to viewer. Returns the global signal index.
    size_t registerSignal(std::unique_ptr<itkSignalBase> sig, QTreeWidget *tree,
                          const QString &name, bool categorical = false, bool transparentZero = false);

    template<typename T>
    bool insertTypedImage(
        typename itk::Image<T, 3>::Pointer  pImage,
        size_t                             &signalIndexGlobalOut,
        bool                                forceShapeOfSegments)
    {
        std::unique_ptr<itkSignal<T>> pSignal(new itkSignal<T>(pImage));
        if (pSignal->isShapeMatched(itkSignalSegmentsGraph) | !forceShapeOfSegments) {
            signalIndexGlobalOut = allSignalList.size();
            itkSignalBase *pRaw = pSignal.get();
            ownedSignals.push_back(std::move(pSignal));
            allSignalList.push_back(pRaw);
            return true;
        } else {
            std::cout << "Segments: [" << itkSignalSegmentsGraph->getDimX() << " "
                      << itkSignalSegmentsGraph->getDimY() << " " << itkSignalSegmentsGraph->getDimZ() << "]\n";
            std::cout << "Image:    [" << pSignal->getDimX() << " "
                      << pSignal->getDimY() << " " << pSignal->getDimZ() << "]\n";
            std::cout << "Dimension mismatch! Image is not added.\n";
            return false;
        }
    }
};



#endif //SEGMENTCOUPLER_WATERSHEDCONTROL_H
