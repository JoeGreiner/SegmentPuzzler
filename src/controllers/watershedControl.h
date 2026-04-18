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
#include <QVBoxLayout>
#include <itkImageIOBase.h>
#include "src/file_definitions/dataTypes.h"
#include "src/segment_handling/graphBase.h"
#include "SignalControl.h"


#include <QTreeWidget>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QScrollArea>
#include <QGroupBox>
#include <QLabel>
#include <QListView>
#include <QTimer>
#include <unordered_map>

#include "src/itkImageFilters/itkWatershedHelpers.h"
#include "src/utils/DistanceMapSeedExtractors.h"

class OrthoViewer;
class TaskRunner;

class WatershedControl : public QWidget {
Q_OBJECT
public:
    enum class OutputMode {
        Refinement,
        Segments
    };

    enum class ThresholdAlgorithm {
        BinaryThreshold
    };

    //TODO: Fix file handling. dataype + signal index should be enough as an unique identifier.
    WatershedControl(std::shared_ptr<GraphBase> graphBaseIn,
                     OrthoViewer *orthoViewerIn,
                     TaskRunner *taskRunnerIn,
                     OutputMode outputModeIn = OutputMode::Refinement,
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
    itkSignalThresholdPreview<dataType::BoundaryVoxelType> *pThresholdPreviewSignal;
    itk::Image<unsigned char, 3>::Pointer pThresholdedMembrane;
    itk::Image<float, 3>::Pointer pDistanceMap;
    itk::Image<unsigned int, 3>::Pointer pSeeds;
    itk::Image<unsigned int, 3>::Pointer pWatershed;

    bool loadImage(QString fileName, itk::ImageIOBase::IOComponentType &dataTypeOut,
               size_t &signalIndexGlobalOut, bool forceShapeOfSegments = true,
               bool forceSegmentDataType = false);

signals:
    void sendClosingSignal();

public slots:
    void togglePaintBoundaryMode();

    void forwardValueChangedSignal(int value);
    void addImagePressed();

    void addBoundariesFromFile(QString fileName);
    void addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn);
    void addBoundaries(dataType::BoundaryImageType::Pointer pBoundariesIn,
            int fxIn, int txIn, int fyIn, int tyIn, int fzIn, int tzIn);

    void setThreadCount(int n);
    void setWatershedAlgorithm(WatershedAlgorithm algorithm);

    void thresholdBoundariesAsync(std::function<void()> then = {});
    void calculateDistanceMapAsync(std::function<void()> then = {});
    void extractSeedsAsync(std::function<void()> then = {});
    void watershedAsync(std::function<void()> then = {});
    void createRefinementAsync(std::function<void()> then = {});

    void thresholdBoundariesPressed();

    void calculateDistanceMapPressed();

    void extractSeedsPressed();


    void watershedPressed();

    void finalizeOutputPressed();

    void addImage(QString fileName);

    void treeDoubleClicked(QTreeWidgetItem *item, int index);

    void treeClicked(QTreeWidgetItem *item, int index);

private:
    enum class SignalStage {
        None,
        Threshold,
        DistanceMap,
        Seeds,
        Watershed,
    };

    QTreeWidgetItem *topLevelItem(QTreeWidgetItem *item) const;
    size_t signalIndexForItem(QTreeWidgetItem *item) const;
    itkSignalBase *signalForItem(QTreeWidgetItem *item) const;
    bool isSegmentsItem(QTreeWidgetItem *item) const;

    bool verbose;
    OutputMode outputMode;

    bool useROI;
    size_t fx, fy, fz, tx, ty, tz;

    QVBoxLayout *signalControlLayout;
    QScrollArea *workflowScrollArea;
    QWidget *workflowContentWidget;
    QVBoxLayout *workflowLayout;

    // overlay tree widget
    QTreeWidget *signalTreeWidget;
    QPushButton *thresholdBoundariesButton;
    QComboBox *boundaryInputComboBox;
    QComboBox *thresholdAlgorithmComboBox;
    QSlider* thresholdValueSlider;
    QSpinBox* thresholdValueSpinBox;

    QPushButton *calculateDistanceMapButton;
    QComboBox *thresholdInputComboBox;
    QComboBox *distanceMapAlgorithmComboBox;
    QPushButton *togglePaintBoundaryModeButton;

    QPushButton *calculateSeedsButton;
    QComboBox *distanceMapInputComboBox;
    QComboBox *seedAlgorithmComboBox;

    QCheckBox *checkBoxFiltering;
    QSpinBox *sizeFilteringInput;
    QPushButton *runWatershedButton;
    QComboBox *watershedDistanceMapInputComboBox;
    QComboBox *watershedSeedsInputComboBox;
    QComboBox *watershedThresholdInputComboBox;
    QComboBox *watershedAlgorithmComboBox;


    QPushButton *createRefinementButton;
    QComboBox *finalOutputInputComboBox;
    bool paintBoundaryModeActive = false;
    int registeredEdgeSignalIndex = -1;
    int workerThreadCount = 1;
    int boundarySignalIndex = -1;
    std::vector<size_t> thresholdOutputSignalIndices;
    std::vector<size_t> distanceMapOutputSignalIndices;
    std::vector<size_t> seedOutputSignalIndices;
    std::vector<size_t> watershedOutputSignalIndices;

    void transferWatershedToGraph();
    void setupWorkflowUi();
    void setupSignalTreeWidget();
    void setupThresholdWidget();
    void setupDistanceMapWidget();
    void setupSeedWidget();
    void setupWatershedWidget();
    void setupFinalizeWidget();
    QGroupBox *createStepGroup(const QString &title) const;
    QWidget *createLabeledInputRow(const QString &labelText, QComboBox *comboBox) const;
    QWidget *createLabeledInputRow(const QString &labelText, QWidget *widget) const;
    QWidget *createSliderWithSpinBox(QSlider *&slider, QSpinBox *&spinBox);
    void addStepSection(QGroupBox *groupBox, QWidget *controlsWidget, QWidget *actionWidget = nullptr);
    void setupTreeWidget(QTreeWidget *treeWidget);
    void updatePaintBoundaryModeButtonText();
    void setupAlgorithmComboBoxes();
    void configureInputCombo(QComboBox *comboBox) const;
    void configureAlgorithmCombo(QComboBox *comboBox) const;
    void setTreeVisibleRows(QTreeWidget *treeWidget, int rows) const;
    void refreshInputSelectors();
    void refreshComboSelection(QComboBox *comboBox, const std::vector<size_t> &signalIndices);
    void updateStepEnablement();
    bool comboHasValidSelection(const QComboBox *comboBox) const;
    size_t selectedSignalIndex(const QComboBox *comboBox) const;
    dataType::BoundaryImageType::Pointer selectedBoundaryInput() const;
    itk::Image<unsigned char, 3>::Pointer selectedThresholdInput() const;
    itk::Image<float, 3>::Pointer selectedDistanceMapInput() const;
    itk::Image<unsigned int, 3>::Pointer selectedSeedsInput() const;
    itk::Image<float, 3>::Pointer selectedWatershedDistanceMapInput() const;
    itk::Image<unsigned int, 3>::Pointer selectedWatershedSeedsInput() const;
    itk::Image<unsigned char, 3>::Pointer selectedWatershedThresholdInput() const;
    dataType::SegmentsImageType::Pointer selectedFinalOutputInput() const;

    ThresholdAlgorithm selectedThresholdAlgorithm() const;
    DistanceMapAlgorithm selectedDistanceMapAlgorithm() const;
    distance_map_benchmark::SeedExtractorKind selectedSeedAlgorithm() const;
    WatershedAlgorithm selectedWatershedAlgorithm() const;

    QString thresholdAlgorithmLabel(ThresholdAlgorithm algorithm) const;
    QString distanceMapAlgorithmLabel(DistanceMapAlgorithm algorithm) const;
    QString seedAlgorithmLabel(distance_map_benchmark::SeedExtractorKind algorithm) const;
    QString watershedAlgorithmLabel(WatershedAlgorithm algorithm) const;

    bool getDimensionMatchWithSegmentImage();
    void setGuiBusy(bool busy);
    void refreshViewers();

private:
    void removeRegisteredEdgeSignal();
    void setSignalActive(size_t signalIdx, bool active);
    void deactivateSignalsByIndices(const std::vector<size_t> &signalIndices);
    void rebuildGraphFromSegmentsImage(dataType::SegmentsImageType::Pointer segmentsImage);
    void attachSegmentsSignalToGraph(itkSignal<GraphSegmentType> *segmentsSignal);

    // Registers a signal: takes ownership, appends to ownedSignals/allSignalList,
    // sets name/LUT/tree widget, adds to viewer. Returns the global signal index.
    size_t registerSignal(std::unique_ptr<itkSignalBase> sig, SignalStage stage,
                          const QString &name, bool categorical = false, bool transparentZero = false, bool active = true);

    template<typename T>
    bool insertTypedImage(
        typename itk::Image<T, 3>::Pointer  pImage,
        size_t                             &signalIndexGlobalOut,
        bool                                forceShapeOfSegments)
    {
        std::unique_ptr<itkSignal<T>> pSignal(new itkSignal<T>(pImage));
        const bool shapeCheckPassed =
            !forceShapeOfSegments || itkSignalSegmentsGraph == nullptr || pSignal->isShapeMatched(itkSignalSegmentsGraph);
        if (shapeCheckPassed) {
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
