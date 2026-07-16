#include "Segment3DViewerDialog.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QGridLayout>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QSlider>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include "src/qtUtils/TaskRunner.h"
#include "src/utils/AppLogger.h"
#include <QVTKOpenGLNativeWidget.h>

#include <vtkSmartPointer.h>
#include <vtkType.h>
#include <vtkCamera.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkMatrix4x4.h>
#include <vtkRenderer.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkImageImport.h>
#include <vtkImageData.h>
#include <vtkExtractVOI.h>
#include <vtkImageBinaryThreshold.h>
#include <vtkImageConstantPad.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyData.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkCellData.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkProp.h>
#include <vtkPropPicker.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkAxesActor.h>
#include <vtkAlgorithmOutput.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkObjectFactory.h>
#include <vtkSMPTools.h>
#include <vtkSurfaceNets3D.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#ifdef USE_OMP
#include <omp.h>
#endif

#include "src/utils/utils.h"

namespace {

QPointF mouseEventPosition(const QMouseEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

}

class CutStrokeOverlay : public QWidget {
public:
    explicit CutStrokeOverlay(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setMouseTracking(true);
    }

    void setDrawingEnabled(bool enabled) {
        if (m_drawingEnabled == enabled) {
            return;
        }
        m_drawingEnabled = enabled;
        m_dragging = false;
        setAttribute(Qt::WA_TransparentForMouseEvents, !enabled);
        update();
    }

    void clearStroke() {
        if (m_points.empty()) {
            return;
        }
        m_dragging = false;
        m_points.clear();
        update();
        if (onStrokeChanged) {
            onStrokeChanged();
        }
    }

    bool hasValidStroke() const {
        return m_points.size() >= 2;
    }

    std::vector<std::array<double, 2>> strokePixels() const {
        std::vector<std::array<double, 2>> pixels;
        pixels.reserve(m_points.size());
        for (const QPointF &point : m_points) {
            pixels.push_back({point.x(), point.y()});
        }
        return pixels;
    }

    std::function<void()> onStrokeChanged;

protected:
    void paintEvent(QPaintEvent *event) override {
        QWidget::paintEvent(event);
        if (m_points.empty()) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor("#ff6f61"));
        pen.setWidthF(6.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        QPolygonF polyline;
        for (const QPointF &point : m_points) {
            polyline << point;
        }
        painter.drawPolyline(polyline);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (!m_drawingEnabled) {
            event->ignore();
            return;
        }

        if (event->button() != Qt::LeftButton) {
            event->accept();
            return;
        }

        m_dragging = true;
        m_points.clear();
        m_points.push_back(mouseEventPosition(event));
        update();
        if (onStrokeChanged) {
            onStrokeChanged();
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!m_drawingEnabled || !m_dragging) {
            event->accept();
            return;
        }

        const QPointF point = mouseEventPosition(event);
        if (!m_points.empty() && QLineF(m_points.back(), point).length() < 1.0) {
            event->accept();
            return;
        }

        m_points.push_back(point);
        update();
        if (onStrokeChanged) {
            onStrokeChanged();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (m_drawingEnabled && event->button() == Qt::LeftButton) {
            m_dragging = false;
            if (onStrokeChanged) {
                onStrokeChanged();
            }
        }
        event->accept();
    }

    void wheelEvent(QWheelEvent *event) override {
        event->accept();
    }

private:
    bool m_drawingEnabled = false;
    bool m_dragging = false;
    std::vector<QPointF> m_points;
};

namespace {

class SharedPointsPolyData final : public vtkPolyData {
public:
    static SharedPointsPolyData *New();
    vtkTypeMacro(SharedPointsPolyData, vtkPolyData);

    void SetPrecomputedCellsBounds(const std::array<double, 6> &bounds) {
        // Explode meshes share the source points and, with OpenGL2, their VBO.
        // Seed the cell-bounds cache so every mapper does not rescan all points.
        // Points and cells must stay immutable after this call.
        std::copy(bounds.begin(), bounds.end(), this->CellsBounds);
        this->CellsBoundsTime.Modified();
    }

private:
    SharedPointsPolyData() = default;
    ~SharedPointsPolyData() override = default;
};

vtkStandardNewMacro(SharedPointsPolyData);

constexpr bool kProfile3DViewExtraction = false;
constexpr dataType::SegmentIdType kDenseLabelLookupLimit = 1'000'000;
constexpr vtkIdType kParallelScanVoxelThreshold = 1'000'000;
constexpr int kMaximumExplodePercent = 1000;

double elapsedMilliseconds(qint64 nanoseconds) {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

QLabel *createHelpBadgeLabel(const QString &tooltipText, QWidget *parent) {
    if (tooltipText.isEmpty()) {
        return nullptr;
    }

    auto *helpLabel = new QLabel("?", parent);
    helpLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    helpLabel->setStyleSheet(
        "QLabel { color: white; background-color: #666; border-radius: 8px; "
        "font-weight: bold; font-size: 11px; min-width: 16px; min-height: 16px; "
        "max-width: 16px; max-height: 16px; padding: 0px; qproperty-alignment: AlignCenter; }");
    helpLabel->setToolTip(tooltipText);
    return helpLabel;
}

bool navigationModifierPressed(Qt::KeyboardModifiers modifiers) {
#ifdef Q_OS_MACOS
    return modifiers.testFlag(Qt::MetaModifier) || modifiers.testFlag(Qt::ControlModifier);
#else
    return modifiers.testFlag(Qt::ControlModifier);
#endif
}

QString threeDViewHelpText(bool showExplodeControls, bool showCutControls) {
#ifdef Q_OS_MACOS
    const QString navigateShortcut = QStringLiteral("Cmd (or Ctrl)");
#else
    const QString navigateShortcut = QStringLiteral("Ctrl");
#endif

    QStringList lines;
    lines << QStringLiteral("Drag to orbit the 3D camera.");
    lines << QStringLiteral("Hold %1 and click a segment to jump the linked orthogonal views to that label.")
                 .arg(navigateShortcut);
    if (showCutControls) {
        lines << QStringLiteral("Use Draw Cut to arm projected cut drawing, Clear to erase the stroke, and Apply to split the segment.");
    }
    if (showExplodeControls) {
        lines << QStringLiteral("Move the Explode slider to add up to %1% of each segment's original distance from the scene center; 0% shows the unshifted view and 100% doubles that distance.")
                     .arg(kMaximumExplodePercent);
        lines << QStringLiteral("Use the left/right arrow keys to step the explode slider.");
    }
    lines << QStringLiteral("Press Q to close the 3D view.");
    return lines.join(QStringLiteral("\n"));
}

double safeCameraDistance(double cameraDistance, double sceneExtent)
{
    const double safeExtent = std::max(sceneExtent, 1.0);
    return std::isfinite(cameraDistance) && cameraDistance > 1e-6
               ? cameraDistance
               : std::max(1.5 * safeExtent, 1.0);
}

#define SP_LOG_3D_TIMER(startedAtMs, label) \
    do { \
        if ((startedAtMs) != 0) { \
            SP_LOG_DEBUG("viewer.three_d", \
                         QStringLiteral("%1 finished in %2 ms") \
                             .arg(label) \
                             .arg(QDateTime::currentMSecsSinceEpoch() - (startedAtMs))); \
        } \
    } while (false)

constexpr int segmentIdVtkDataType() {
#ifdef SEGMENTSHORT
    return VTK_SHORT;
#else
    return VTK_UNSIGNED_INT;
#endif
}

struct LabelMembershipLookup {
    dataType::SegmentIdType maxLabelId = 0;
    bool useDense = false;
    std::vector<unsigned char> denseLookup;
    std::unordered_set<dataType::SegmentIdType> sparseLookup;

    bool contains(dataType::SegmentIdType labelId) const {
        if (labelId == 0) {
            return false;
        }
        if (useDense) {
            return labelId <= maxLabelId && denseLookup[static_cast<std::size_t>(labelId)] != 0;
        }
        return sparseLookup.find(labelId) != sparseLookup.end();
    }
};

LabelMembershipLookup buildRequestedLabelLookup(
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    LabelMembershipLookup lookup;
    for (const auto &[labelId, lutColor] : labels) {
        lookup.maxLabelId = std::max(lookup.maxLabelId, labelId);
    }

    lookup.useDense = lookup.maxLabelId <= kDenseLabelLookupLimit;
    if (lookup.useDense) {
        lookup.denseLookup.assign(static_cast<std::size_t>(lookup.maxLabelId) + 1, 0);
        for (const auto &[labelId, lutColor] : labels) {
            lookup.denseLookup[static_cast<std::size_t>(labelId)] = 1;
        }
    } else {
        lookup.sparseLookup.reserve(labels.size());
        for (const auto &[labelId, lutColor] : labels) {
            lookup.sparseLookup.insert(labelId);
        }
    }

    return lookup;
}

struct BoundsScanResult {
    int minX = 0;
    int maxX = -1;
    int minY = 0;
    int maxY = -1;
    int minZ = 0;
    int maxZ = -1;
};

struct AllLabelsScanResult {
    BoundsScanResult bounds;
    std::vector<dataType::SegmentIdType> labels;
};

void includeInBounds(BoundsScanResult &bounds, int x, int y, int z) {
    bounds.minX = std::min(bounds.minX, x);
    bounds.maxX = std::max(bounds.maxX, x);
    bounds.minY = std::min(bounds.minY, y);
    bounds.maxY = std::max(bounds.maxY, y);
    bounds.minZ = std::min(bounds.minZ, z);
    bounds.maxZ = std::max(bounds.maxZ, z);
}

AllLabelsScanResult scanAllLabelsAndBounds(
    const dataType::SegmentIdType *buf,
    int dimX,
    int dimY,
    int dimZ)
{
    struct LocalResult {
        BoundsScanResult bounds;
        std::unordered_set<dataType::SegmentIdType> labels;
    };

    const BoundsScanResult emptyBounds{dimX, -1, dimY, -1, dimZ, -1};
    std::vector<LocalResult> localResults;

#ifdef USE_OMP
    const auto totalVoxelCount = static_cast<std::size_t>(dimX) * dimY * dimZ;
    const int threadCount = totalVoxelCount >= static_cast<std::size_t>(kParallelScanVoxelThreshold)
                            ? omp_get_max_threads()
                            : 1;
#else
    const int threadCount = 1;
#endif
    localResults.resize(static_cast<std::size_t>(threadCount));
    for (auto &local : localResults) {
        local.bounds = emptyBounds;
    }

#ifdef USE_OMP
#pragma omp parallel if(threadCount > 1) num_threads(threadCount)
#endif
    {
#ifdef USE_OMP
        auto &local = localResults[static_cast<std::size_t>(omp_get_thread_num())];
#else
        auto &local = localResults.front();
#endif
#ifdef USE_OMP
#pragma omp for nowait
#endif
        for (int z = 0; z < dimZ; ++z) {
            for (int y = 0; y < dimY; ++y) {
                const auto rowOffset =
                    static_cast<std::size_t>(z) * dimX * dimY + static_cast<std::size_t>(y) * dimX;
                const auto *row = buf + rowOffset;
                for (int x = 0; x < dimX; ++x) {
                    const auto labelId = row[x];
                    if (labelId == 0) {
                        continue;
                    }
                    local.labels.insert(labelId);
                    includeInBounds(local.bounds, x, y, z);
                }
            }
        }
    }

    AllLabelsScanResult result;
    result.bounds = emptyBounds;
    std::unordered_set<dataType::SegmentIdType> labels;
    for (const auto &local : localResults) {
        if (local.bounds.maxX >= 0) {
            includeInBounds(result.bounds, local.bounds.minX, local.bounds.minY, local.bounds.minZ);
            includeInBounds(result.bounds, local.bounds.maxX, local.bounds.maxY, local.bounds.maxZ);
        }
        labels.insert(local.labels.begin(), local.labels.end());
    }

    result.labels.assign(labels.begin(), labels.end());
    std::sort(result.labels.begin(), result.labels.end());
    return result;
}

BoundsScanResult clampBoundsToImage(const Roi &roi,
                                    int dimX,
                                    int dimY,
                                    int dimZ)
{
    BoundsScanResult result;
    result.minX = std::clamp(roi.minX, 0, std::max(0, dimX - 1));
    result.maxX = std::clamp(roi.maxX, 0, std::max(0, dimX - 1));
    result.minY = std::clamp(roi.minY, 0, std::max(0, dimY - 1));
    result.maxY = std::clamp(roi.maxY, 0, std::max(0, dimY - 1));
    result.minZ = std::clamp(roi.minZ, 0, std::max(0, dimZ - 1));
    result.maxZ = std::clamp(roi.maxZ, 0, std::max(0, dimZ - 1));
    return result;
}

BoundsScanResult scanBoundsForRequestedLabels(
    const dataType::SegmentIdType *buf,
    int dimX,
    int dimY,
    int dimZ,
    const LabelMembershipLookup &lookup)
{
    BoundsScanResult result{dimX, -1, dimY, -1, dimZ, -1};
    const auto totalVoxelCount = static_cast<vtkIdType>(dimX) * dimY * dimZ;

#ifdef USE_OMP
    if (totalVoxelCount >= kParallelScanVoxelThreshold) {
#pragma omp parallel
        {
            BoundsScanResult local{dimX, -1, dimY, -1, dimZ, -1};
#pragma omp for nowait
            for (int z = 0; z < dimZ; ++z) {
                for (int y = 0; y < dimY; ++y) {
                    const auto *row = buf + z * dimX * dimY + y * dimX;
                    for (int x = 0; x < dimX; ++x) {
                        if (!lookup.contains(row[x])) {
                            continue;
                        }
                        if (x < local.minX) local.minX = x;
                        if (x > local.maxX) local.maxX = x;
                        if (y < local.minY) local.minY = y;
                        if (y > local.maxY) local.maxY = y;
                        if (z < local.minZ) local.minZ = z;
                        if (z > local.maxZ) local.maxZ = z;
                    }
                }
            }

            if (local.maxX >= 0) {
#pragma omp critical
                {
                    result.minX = std::min(result.minX, local.minX);
                    result.maxX = std::max(result.maxX, local.maxX);
                    result.minY = std::min(result.minY, local.minY);
                    result.maxY = std::max(result.maxY, local.maxY);
                    result.minZ = std::min(result.minZ, local.minZ);
                    result.maxZ = std::max(result.maxZ, local.maxZ);
                }
            }
        }

        return result;
    }
#endif

    for (int z = 0; z < dimZ; ++z) {
        for (int y = 0; y < dimY; ++y) {
            const auto *row = buf + z * dimX * dimY + y * dimX;
            for (int x = 0; x < dimX; ++x) {
                if (!lookup.contains(row[x])) {
                    continue;
                }
                if (x < result.minX) result.minX = x;
                if (x > result.maxX) result.maxX = x;
                if (y < result.minY) result.minY = y;
                if (y > result.maxY) result.maxY = y;
                if (z < result.minZ) result.minZ = z;
                if (z > result.maxZ) result.maxZ = z;
            }
        }
    }

    return result;
}

vtkSmartPointer<vtkPolyData> detachPolyData(vtkPolyData *source) {
    auto copy = vtkSmartPointer<vtkPolyData>::New();
    if (source != nullptr) {
        copy->ShallowCopy(source);
    }
    return copy;
}

std::array<double, 3> centerFromBounds(vtkPolyData *polyData) {
    std::array<double, 3> center{0.0, 0.0, 0.0};
    if (polyData == nullptr || polyData->GetNumberOfPoints() == 0 || polyData->GetNumberOfCells() == 0) {
        return center;
    }

    double bounds[6];
    polyData->GetBounds(bounds);
    center[0] = 0.5 * (bounds[0] + bounds[1]);
    center[1] = 0.5 * (bounds[2] + bounds[3]);
    center[2] = 0.5 * (bounds[4] + bounds[5]);
    return center;
}

void configureSurfaceNet(vtkSurfaceNets3D *surfaceNet) {
    surfaceNet->SetBackgroundLabel(0);
    surfaceNet->SetOutputMeshTypeToTriangles();
    surfaceNet->SetTriangulationStrategyToGreedy();
    surfaceNet->SmoothingOn();
    surfaceNet->SetNumberOfIterations(3);
}

void setSurfaceNetLabels(vtkSurfaceNets3D *surfaceNet,
                         const std::vector<dataType::SegmentIdType> &labelIds)
{
    surfaceNet->SetNumberOfLabels(static_cast<int>(labelIds.size()));
    for (int i = 0; i < static_cast<int>(labelIds.size()); ++i) {
        surfaceNet->SetLabel(i, labelIds[static_cast<std::size_t>(i)]);
    }
}

std::vector<dataType::SegmentIdType> collectRequestedLabels(
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    std::vector<dataType::SegmentIdType> labelIds;
    labelIds.reserve(labels.size());
    for (const auto &[labelId, lutColor] : labels) {
        labelIds.push_back(labelId);
    }
    return labelIds;
}

std::vector<dataType::SegmentIdType> collectLabelsInExtent(
    const dataType::SegmentIdType *buf,
    int dimX,
    int dimY,
    int minX,
    int maxX,
    int minY,
    int maxY,
    int minZ,
    int maxZ)
{
    std::unordered_set<dataType::SegmentIdType> labelSet;
    const auto totalVoxelCount =
        static_cast<vtkIdType>(maxX - minX + 1) * (maxY - minY + 1) * (maxZ - minZ + 1);

#ifdef USE_OMP
    if (totalVoxelCount >= kParallelScanVoxelThreshold) {
        const int maxThreads = omp_get_max_threads();
        std::vector<std::unordered_set<dataType::SegmentIdType>> localSets(static_cast<std::size_t>(maxThreads));

#pragma omp parallel
        {
            auto &localSet = localSets[static_cast<std::size_t>(omp_get_thread_num())];
#pragma omp for nowait
            for (int z = minZ; z <= maxZ; ++z) {
                for (int y = minY; y <= maxY; ++y) {
                    const auto *row = buf + z * dimX * dimY + y * dimX;
                    for (int x = minX; x <= maxX; ++x) {
                        if (const auto labelId = row[x]; labelId != 0) {
                            localSet.insert(labelId);
                        }
                    }
                }
            }
        }

        std::size_t reserveCount = 0;
        for (const auto &localSet : localSets) {
            reserveCount += localSet.size();
        }
        labelSet.reserve(reserveCount);
        for (auto &localSet : localSets) {
            labelSet.insert(localSet.begin(), localSet.end());
        }
    } else
#endif
    {
        for (int z = minZ; z <= maxZ; ++z) {
            for (int y = minY; y <= maxY; ++y) {
                const auto *row = buf + z * dimX * dimY + y * dimX;
                for (int x = minX; x <= maxX; ++x) {
                    if (const auto labelId = row[x]; labelId != 0) {
                        labelSet.insert(labelId);
                    }
                }
            }
        }
    }

    std::vector<dataType::SegmentIdType> labelIds(labelSet.begin(), labelSet.end());
    std::sort(labelIds.begin(), labelIds.end());
    return labelIds;
}

vtkSmartPointer<vtkSurfaceNets3D> createSurfaceNet(
    vtkImageData *inputData,
    const std::vector<dataType::SegmentIdType> &surfaceNetLabels)
{
    if (inputData == nullptr || surfaceNetLabels.empty()) {
        return nullptr;
    }

    auto surfaceNet = vtkSmartPointer<vtkSurfaceNets3D>::New();
    surfaceNet->SetInputData(inputData);
    configureSurfaceNet(surfaceNet);
    // VTK 9.6 smoothing uses this cache as working storage during the same Update().
    surfaceNet->DataCachingOn();
    setSurfaceNetLabels(surfaceNet, surfaceNetLabels);
    return surfaceNet;
}

vtkSmartPointer<vtkPolyData> extractSelectedSurfaceNetOutput(
    vtkSurfaceNets3D *surfaceNet,
    const std::vector<dataType::SegmentIdType> &selectedLabels)
{
    auto polyData = vtkSmartPointer<vtkPolyData>::New();
    if (surfaceNet == nullptr || selectedLabels.empty()) {
        return polyData;
    }

    surfaceNet->InitializeSelectedLabelsList();
    for (const auto labelId : selectedLabels) {
        surfaceNet->AddSelectedLabel(labelId);
    }
    surfaceNet->Update();

    polyData = detachPolyData(surfaceNet->GetOutput());
    return polyData;
}

Segment3DViewerDialog::PreparedMesh makePreparedMesh(
    dataType::SegmentIdType labelId,
    quint32 lutColor,
    vtkPolyData *polyData)
{
    Segment3DViewerDialog::PreparedMesh mesh;
    if (polyData == nullptr || polyData->GetNumberOfPoints() == 0 || polyData->GetNumberOfCells() == 0) {
        return mesh;
    }

    mesh.labelId = labelId;
    mesh.polyData = polyData;
    mesh.lutColor = lutColor;
    mesh.centerWorld = centerFromBounds(polyData);
    return mesh;
}

Segment3DViewerDialog::PreparedMesh extractSelectedLabelMesh(
    vtkImageData *inputData,
    dataType::SegmentIdType selectedLabel,
    quint32 lutColor,
    const std::vector<dataType::SegmentIdType> &surfaceNetLabels,
    bool logMesh)
{
    Segment3DViewerDialog::PreparedMesh mesh;
    auto surfaceNet = createSurfaceNet(inputData, surfaceNetLabels);
    if (surfaceNet != nullptr) {
        surfaceNet->SetOutputStyleToSelected();
    }
    auto polyData = extractSelectedSurfaceNetOutput(surfaceNet, {selectedLabel});
    if (logMesh) {
        SP_LOG_DEBUG("viewer.three_d",
                     QStringLiteral("[3DView] single-label selected mesh labelsInVOI=%1 points=%2 cells=%3")
                         .arg(surfaceNetLabels.size())
                         .arg(polyData->GetNumberOfPoints())
                         .arg(polyData->GetNumberOfCells()));
    }

    return makePreparedMesh(selectedLabel, lutColor, polyData);
}

using MeshBounds = std::array<double, 6>;

MeshBounds emptyMeshBounds() {
    return {
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()};
}

void includePoint(MeshBounds &bounds, const double point[3]) {
    for (int component = 0; component < 3; ++component) {
        bounds[2 * component] = std::min(bounds[2 * component], point[component]);
        bounds[2 * component + 1] = std::max(bounds[2 * component + 1], point[component]);
    }
}

void mergeBounds(MeshBounds &target, const MeshBounds &source) {
    for (int component = 0; component < 3; ++component) {
        target[2 * component] = std::min(target[2 * component], source[2 * component]);
        target[2 * component + 1] = std::max(target[2 * component + 1], source[2 * component + 1]);
    }
}

std::array<double, 3> centerOfBounds(const MeshBounds &bounds) {
    return {
        0.5 * (bounds[0] + bounds[1]),
        0.5 * (bounds[2] + bounds[3]),
        0.5 * (bounds[4] + bounds[5])};
}

struct ExplodeSourceView {
    vtkPoints *points = nullptr;
    vtkIdType pointCount = 0;
    vtkIdType cellCount = 0;
    const vtkTypeInt32 *connectivity32 = nullptr;
    const vtkTypeInt64 *connectivity64 = nullptr;
    const float *floatPoints = nullptr;
    const double *doublePoints = nullptr;
    const dataType::SegmentIdType *boundaryValues = nullptr;

    vtkIdType pointIdAt(vtkIdType cellId, vtkIdType corner) const {
        const vtkIdType offset = 3 * cellId + corner;
        return connectivity32 != nullptr
                   ? static_cast<vtkIdType>(connectivity32[offset])
                   : static_cast<vtkIdType>(connectivity64[offset]);
    }

    void pointAt(vtkIdType pointId, double point[3]) const {
        const vtkIdType offset = 3 * pointId;
        if (floatPoints != nullptr) {
            point[0] = floatPoints[offset];
            point[1] = floatPoints[offset + 1];
            point[2] = floatPoints[offset + 2];
        } else {
            point[0] = doublePoints[offset];
            point[1] = doublePoints[offset + 1];
            point[2] = doublePoints[offset + 2];
        }
    }

    std::pair<dataType::SegmentIdType, dataType::SegmentIdType> labelsAt(vtkIdType cellId) const {
        const vtkIdType offset = 2 * cellId;
        return {boundaryValues[offset], boundaryValues[offset + 1]};
    }
};

std::optional<ExplodeSourceView> makeExplodeSourceView(
    vtkPolyData *polyData,
    vtkDataArray *boundaryLabels)
{
    if (polyData == nullptr || boundaryLabels == nullptr) {
        return std::nullopt;
    }

    ExplodeSourceView source;
    source.cellCount = polyData->GetNumberOfCells();
    if (source.cellCount <= 0
        || source.cellCount > std::numeric_limits<vtkIdType>::max() / 3
        || boundaryLabels->GetNumberOfComponents() != 2
        || boundaryLabels->GetNumberOfTuples() != source.cellCount
        || boundaryLabels->GetDataType() != segmentIdVtkDataType()
        || !boundaryLabels->HasStandardMemoryLayout()) {
        SP_LOG_WARNING("viewer.three_d",
                       QStringLiteral("[3DView] invalid explode boundary-label array"));
        return std::nullopt;
    }
    source.boundaryValues =
        static_cast<const dataType::SegmentIdType *>(boundaryLabels->GetVoidPointer(0));

    auto *sourcePolys = polyData->GetPolys();
    source.points = polyData->GetPoints();
    if (sourcePolys == nullptr || source.points == nullptr
        || sourcePolys->GetNumberOfCells() != source.cellCount
        || sourcePolys->GetNumberOfConnectivityIds() != 3 * source.cellCount
        || sourcePolys->IsHomogeneous() != 3) {
        SP_LOG_WARNING("viewer.three_d",
                       QStringLiteral("[3DView] explode split expects a triangle-only polygon mesh"));
        return std::nullopt;
    }

    if (auto *connectivity = vtkCellArray::AOSArray32::SafeDownCast(
            sourcePolys->GetConnectivityArray())) {
        source.connectivity32 = connectivity->GetPointer(0);
    } else if (auto *connectivity = vtkCellArray::AOSArray64::SafeDownCast(
                   sourcePolys->GetConnectivityArray())) {
        source.connectivity64 = connectivity->GetPointer(0);
    }
    if (source.connectivity32 == nullptr && source.connectivity64 == nullptr) {
        SP_LOG_WARNING("viewer.three_d",
                       QStringLiteral("[3DView] explode split cannot access polygon connectivity"));
        return std::nullopt;
    }

    source.pointCount = source.points->GetNumberOfPoints();
    auto *pointData = source.points->GetData();
    if (source.pointCount <= 0
        || source.pointCount > std::numeric_limits<vtkIdType>::max() / 3
        || pointData == nullptr
        || pointData->GetNumberOfComponents() != 3
        || !pointData->HasStandardMemoryLayout()) {
        SP_LOG_WARNING("viewer.three_d",
                       QStringLiteral("[3DView] invalid explode point array"));
        return std::nullopt;
    }
    if (pointData->GetDataType() == VTK_FLOAT) {
        source.floatPoints = static_cast<const float *>(pointData->GetVoidPointer(0));
    } else if (pointData->GetDataType() == VTK_DOUBLE) {
        source.doublePoints = static_cast<const double *>(pointData->GetVoidPointer(0));
    } else {
        SP_LOG_WARNING("viewer.three_d",
                       QStringLiteral("[3DView] explode points must use float or double storage"));
        return std::nullopt;
    }
    return source;
}

struct LabelIndexLookup {
    dataType::SegmentIdType maxLabelId = 0;
    bool useDense = false;
    std::vector<int> denseIndices;
    std::unordered_map<dataType::SegmentIdType, int> sparseIndices;

    int find(dataType::SegmentIdType labelId) const {
        if (labelId == 0) {
            return -1;
        }
        if (useDense) {
            return labelId <= maxLabelId
                       ? denseIndices[static_cast<std::size_t>(labelId)]
                       : -1;
        }
        const auto it = sparseIndices.find(labelId);
        return it == sparseIndices.end() ? -1 : it->second;
    }
};

std::optional<LabelIndexLookup> buildLabelIndexLookup(
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    if (labels.empty()) {
        return std::nullopt;
    }

    LabelIndexLookup lookup;
    std::unordered_set<dataType::SegmentIdType> uniqueLabels;
    uniqueLabels.reserve(labels.size());
    for (const auto &[labelId, lutColor] : labels) {
        if (labelId == 0 || !uniqueLabels.insert(labelId).second) {
            SP_LOG_WARNING("viewer.three_d",
                           QStringLiteral("[3DView] explode labels must be nonzero and unique"));
            return std::nullopt;
        }
        lookup.maxLabelId = std::max(lookup.maxLabelId, labelId);
    }

    lookup.useDense = lookup.maxLabelId <= kDenseLabelLookupLimit;
    if (lookup.useDense) {
        lookup.denseIndices.assign(static_cast<std::size_t>(lookup.maxLabelId) + 1, -1);
        for (std::size_t index = 0; index < labels.size(); ++index) {
            lookup.denseIndices[static_cast<std::size_t>(labels[index].first)] =
                static_cast<int>(index);
        }
    } else {
        lookup.sparseIndices.reserve(labels.size());
        for (std::size_t index = 0; index < labels.size(); ++index) {
            lookup.sparseIndices.emplace(labels[index].first, static_cast<int>(index));
        }
    }
    return lookup;
}

vtkIdType chunkBoundary(vtkIdType itemCount, vtkIdType chunkCount, vtkIdType chunkIndex) {
    const vtkIdType baseSize = itemCount / chunkCount;
    const vtkIdType remainder = itemCount % chunkCount;
    return chunkIndex * baseSize + std::min(chunkIndex, remainder);
}

struct ChunkLabelStats {
    std::vector<vtkIdType> cellCounts;
    std::vector<MeshBounds> bounds;
    bool invalidPointId = false;
};

struct ExplodeScanResult {
    vtkIdType chunkCount = 0;
    std::vector<ChunkLabelStats> chunks;
    std::vector<vtkIdType> labelCellCounts;
    std::vector<MeshBounds> labelBounds;
};

std::optional<ExplodeScanResult> scanExplodeTriangles(
    const ExplodeSourceView &source,
    const LabelIndexLookup &labelLookup,
    std::size_t labelCount)
{
    constexpr vtkIdType kChunksPerThread = 4;
    const vtkIdType estimatedThreads =
        std::max<vtkIdType>(1, vtkSMPTools::GetEstimatedNumberOfThreads());

    ExplodeScanResult result;
    result.chunkCount = std::min(source.cellCount, kChunksPerThread * estimatedThreads);
    result.chunks.resize(static_cast<std::size_t>(result.chunkCount));
    for (auto &chunk : result.chunks) {
        chunk.cellCounts.assign(labelCount, 0);
        chunk.bounds.assign(labelCount, emptyMeshBounds());
    }

    vtkSMPTools::For(0, result.chunkCount, 1, [&](vtkIdType chunkBegin, vtkIdType chunkEnd) {
        for (vtkIdType chunkIndex = chunkBegin; chunkIndex < chunkEnd; ++chunkIndex) {
            auto &chunk = result.chunks[static_cast<std::size_t>(chunkIndex)];
            const vtkIdType beginCell = chunkBoundary(source.cellCount, result.chunkCount, chunkIndex);
            const vtkIdType endCell = chunkBoundary(source.cellCount, result.chunkCount, chunkIndex + 1);
            for (vtkIdType cellId = beginCell; cellId < endCell; ++cellId) {
                const auto [labelA, labelB] = source.labelsAt(cellId);
                const int labelAIndex = labelLookup.find(labelA);
                const int labelBIndex = labelB == labelA ? -1 : labelLookup.find(labelB);
                if (labelAIndex < 0 && labelBIndex < 0) {
                    continue;
                }

                MeshBounds triangleBounds = emptyMeshBounds();
                bool validTriangle = true;
                for (vtkIdType corner = 0; corner < 3; ++corner) {
                    const vtkIdType pointId = source.pointIdAt(cellId, corner);
                    if (pointId < 0 || pointId >= source.pointCount) {
                        chunk.invalidPointId = true;
                        validTriangle = false;
                        break;
                    }
                    double point[3];
                    source.pointAt(pointId, point);
                    includePoint(triangleBounds, point);
                }
                if (!validTriangle) {
                    continue;
                }

                const auto includeTriangle = [&](int labelIndex) {
                    if (labelIndex < 0) {
                        return;
                    }
                    const auto index = static_cast<std::size_t>(labelIndex);
                    ++chunk.cellCounts[index];
                    mergeBounds(chunk.bounds[index], triangleBounds);
                };
                // Shared interfaces are part of both adjacent actors.
                includeTriangle(labelAIndex);
                includeTriangle(labelBIndex);
            }
        }
    });

    result.labelCellCounts.assign(labelCount, 0);
    result.labelBounds.assign(labelCount, emptyMeshBounds());
    for (const auto &chunk : result.chunks) {
        if (chunk.invalidPointId) {
            SP_LOG_WARNING("viewer.three_d",
                           QStringLiteral("[3DView] explode mesh contains an invalid point id"));
            return std::nullopt;
        }
        for (std::size_t labelIndex = 0; labelIndex < labelCount; ++labelIndex) {
            if (chunk.cellCounts[labelIndex] == 0) {
                continue;
            }
            result.labelCellCounts[labelIndex] += chunk.cellCounts[labelIndex];
            mergeBounds(result.labelBounds[labelIndex], chunk.bounds[labelIndex]);
        }
    }
    return result;
}

template <typename ConnectivityArray>
typename ConnectivityArray::ValueType *allocateTriangleConnectivity(
    vtkCellArray *polys,
    vtkIdType cellCount)
{
    auto connectivity = vtkSmartPointer<ConnectivityArray>::New();
    connectivity->SetNumberOfValues(3 * cellCount);
    auto *values = connectivity->GetPointer(0);
    return values != nullptr && polys->SetData(3, connectivity) ? values : nullptr;
}

std::optional<std::vector<vtkSmartPointer<vtkCellArray>>> buildExplodePolys(
    const ExplodeSourceView &source,
    const LabelIndexLookup &labelLookup,
    const ExplodeScanResult &scan)
{
    const std::size_t labelCount = scan.labelCellCounts.size();
    std::vector<vtkSmartPointer<vtkCellArray>> polys(labelCount);
    std::vector<vtkTypeInt32 *> connectivity32(labelCount, nullptr);
    std::vector<vtkTypeInt64 *> connectivity64(labelCount, nullptr);
    const bool use32BitConnectivity =
        source.pointCount <= std::numeric_limits<vtkTypeInt32>::max()
        && std::all_of(scan.labelCellCounts.begin(), scan.labelCellCounts.end(), [](vtkIdType count) {
               return count <= std::numeric_limits<vtkTypeInt32>::max() / 3;
           });

    for (std::size_t labelIndex = 0; labelIndex < labelCount; ++labelIndex) {
        const vtkIdType cellCount = scan.labelCellCounts[labelIndex];
        if (cellCount == 0) {
            continue;
        }
        polys[labelIndex] = vtkSmartPointer<vtkCellArray>::New();
        if (use32BitConnectivity) {
            connectivity32[labelIndex] =
                allocateTriangleConnectivity<vtkCellArray::AOSArray32>(
                    polys[labelIndex], cellCount);
            if (connectivity32[labelIndex] == nullptr) {
                return std::nullopt;
            }
        } else {
            connectivity64[labelIndex] =
                allocateTriangleConnectivity<vtkCellArray::AOSArray64>(
                    polys[labelIndex], cellCount);
            if (connectivity64[labelIndex] == nullptr) {
                return std::nullopt;
            }
        }
    }

    std::vector<std::vector<vtkIdType>> chunkStartOffsets(
        static_cast<std::size_t>(scan.chunkCount), std::vector<vtkIdType>(labelCount, 0));
    for (std::size_t labelIndex = 0; labelIndex < labelCount; ++labelIndex) {
        vtkIdType nextOffset = 0;
        for (vtkIdType chunkIndex = 0; chunkIndex < scan.chunkCount; ++chunkIndex) {
            chunkStartOffsets[static_cast<std::size_t>(chunkIndex)][labelIndex] = nextOffset;
            nextOffset += scan.chunks[static_cast<std::size_t>(chunkIndex)].cellCounts[labelIndex];
        }
    }

    vtkSMPTools::For(0, scan.chunkCount, 1, [&](vtkIdType chunkBegin, vtkIdType chunkEnd) {
        for (vtkIdType chunkIndex = chunkBegin; chunkIndex < chunkEnd; ++chunkIndex) {
            auto writeCursors = chunkStartOffsets[static_cast<std::size_t>(chunkIndex)];
            const vtkIdType beginCell = chunkBoundary(source.cellCount, scan.chunkCount, chunkIndex);
            const vtkIdType endCell = chunkBoundary(source.cellCount, scan.chunkCount, chunkIndex + 1);
            for (vtkIdType cellId = beginCell; cellId < endCell; ++cellId) {
                const auto [labelA, labelB] = source.labelsAt(cellId);
                const vtkIdType pointIds[3]{
                    source.pointIdAt(cellId, 0),
                    source.pointIdAt(cellId, 1),
                    source.pointIdAt(cellId, 2)};

                const auto writeTriangle = [&](int labelIndex) {
                    if (labelIndex < 0) {
                        return;
                    }
                    const auto index = static_cast<std::size_t>(labelIndex);
                    const vtkIdType outputOffset = 3 * writeCursors[index]++;
                    if (use32BitConnectivity) {
                        auto *values = connectivity32[index];
                        for (vtkIdType corner = 0; corner < 3; ++corner) {
                            values[outputOffset + corner] = static_cast<vtkTypeInt32>(pointIds[corner]);
                        }
                    } else {
                        auto *values = connectivity64[index];
                        for (vtkIdType corner = 0; corner < 3; ++corner) {
                            values[outputOffset + corner] = static_cast<vtkTypeInt64>(pointIds[corner]);
                        }
                    }
                };
                writeTriangle(labelLookup.find(labelA));
                writeTriangle(labelB == labelA ? -1 : labelLookup.find(labelB));
            }
        }
    });

    for (std::size_t labelIndex = 0; labelIndex < labelCount; ++labelIndex) {
        if (scan.labelCellCounts[labelIndex] == 0) {
            continue;
        }
        auto *connectivity = polys[labelIndex]->GetConnectivityArray();
        connectivity->DataChanged();
        connectivity->Modified();
    }
    return polys;
}

std::vector<Segment3DViewerDialog::PreparedMesh> makeExplodeMeshes(
    vtkPoints *sharedPoints,
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels,
    const ExplodeScanResult &scan,
    std::vector<vtkSmartPointer<vtkCellArray>> polys)
{
    std::vector<Segment3DViewerDialog::PreparedMesh> meshes;
    meshes.reserve(labels.size());
    for (std::size_t labelIndex = 0; labelIndex < labels.size(); ++labelIndex) {
        if (scan.labelCellCounts[labelIndex] == 0) {
            continue;
        }

        auto polyData = vtkSmartPointer<SharedPointsPolyData>::New();
        polyData->SetPoints(sharedPoints);
        polyData->SetPolys(polys[labelIndex]);
        polyData->SetPrecomputedCellsBounds(scan.labelBounds[labelIndex]);

        const auto &[labelId, lutColor] = labels[labelIndex];
        Segment3DViewerDialog::PreparedMesh mesh;
        mesh.labelId = labelId;
        mesh.polyData = polyData;
        mesh.lutColor = lutColor;
        mesh.centerWorld = centerOfBounds(scan.labelBounds[labelIndex]);
        meshes.push_back(std::move(mesh));
    }
    return meshes;
}

std::vector<Segment3DViewerDialog::PreparedMesh> splitMultiLabelMesh(
    vtkPolyData *combinedPolyData,
    vtkDataArray *boundaryLabels,
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    const qint64 startedAt =
        kProfile3DViewExtraction ? QDateTime::currentMSecsSinceEpoch() : 0;
    const auto source = makeExplodeSourceView(combinedPolyData, boundaryLabels);
    const auto labelLookup = buildLabelIndexLookup(labels);
    if (!source || !labelLookup) {
        return {};
    }

    const auto scan = scanExplodeTriangles(*source, *labelLookup, labels.size());
    if (!scan) {
        return {};
    }
    auto polys = buildExplodePolys(*source, *labelLookup, *scan);
    if (!polys) {
        SP_LOG_WARNING("viewer.three_d",
                       QStringLiteral("[3DView] failed to allocate explode connectivity"));
        return {};
    }

    auto meshes = makeExplodeMeshes(source->points, labels, *scan, std::move(*polys));
    SP_LOG_3D_TIMER(startedAt, QStringLiteral("[3DView] [segmentpuzzler] split multi-label mesh"));
    return meshes;
}

std::vector<Segment3DViewerDialog::PreparedMesh> extractSingleLabelMeshes(
    vtkImageData *inputData,
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels,
    bool logPerLabel)
{
    std::vector<Segment3DViewerDialog::PreparedMesh> preparedMeshes;
    preparedMeshes.reserve(labels.size());

    for (const auto &[labelId, lutColor] : labels) {
        auto threshold = vtkSmartPointer<vtkImageBinaryThreshold>::New();
        threshold->SetThresholdFunction(vtkImageBinaryThreshold::THRESHOLD_BETWEEN);
        threshold->SetInputData(inputData);
        threshold->SetLowerThreshold(labelId);
        threshold->SetUpperThreshold(labelId);
        threshold->SetInValue(1.0);
        threshold->SetOutValue(0.0);
        threshold->ReplaceInOn();
        threshold->ReplaceOutOn();
        threshold->SetOutputScalarTypeToFloat();

        auto surfaceNet = vtkSmartPointer<vtkSurfaceNets3D>::New();
        surfaceNet->SetInputConnection(threshold->GetOutputPort());
        configureSurfaceNet(surfaceNet);
        surfaceNet->SetNumberOfLabels(1);
        surfaceNet->SetLabel(0, 1.0);
        surfaceNet->Update();
        auto polyData = detachPolyData(surfaceNet->GetOutput());

        if (logPerLabel) {
            SP_LOG_DEBUG("viewer.three_d",
                         QStringLiteral("[3DView] label %1 points=%2 cells=%3")
                             .arg(labelId)
                             .arg(polyData->GetNumberOfPoints())
                             .arg(polyData->GetNumberOfCells()));
        }

        if (polyData->GetNumberOfPoints() == 0 || polyData->GetNumberOfCells() == 0) {
            continue;
        }

        Segment3DViewerDialog::PreparedMesh mesh;
        mesh.labelId = labelId;
        mesh.polyData = polyData;
        mesh.lutColor = lutColor;
        mesh.centerWorld = centerFromBounds(polyData);
        preparedMeshes.push_back(std::move(mesh));
    }

    return preparedMeshes;
}

vtkSmartPointer<vtkActor> createMeshActor(
    const Segment3DViewerDialog::PreparedMesh &mesh)
{
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(mesh.polyData);
    mapper->StaticOn();
    mapper->ScalarVisibilityOff();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    const double red = ((mesh.lutColor >> 16) & 0xFF) / 255.0;
    const double green = ((mesh.lutColor >> 8) & 0xFF) / 255.0;
    const double blue = (mesh.lutColor & 0xFF) / 255.0;
    actor->GetProperty()->SetColor(red, green, blue);
    actor->GetProperty()->SetAmbient(0.1);
    actor->GetProperty()->SetDiffuse(0.7);
    actor->GetProperty()->SetSpecular(0.3);
    actor->GetProperty()->SetSpecularPower(20.0);
    return actor;
}

void updateSceneBoundsFromMeshes(Segment3DViewerDialog::PreparedScene &preparedScene) {
    if (preparedScene.meshes.empty()) {
        return;
    }

    MeshBounds sceneBounds = emptyMeshBounds();
    bool haveBounds = false;
    for (const auto &mesh : preparedScene.meshes) {
        if (mesh.polyData == nullptr || mesh.polyData->GetNumberOfPoints() == 0 || mesh.polyData->GetNumberOfCells() == 0) {
            continue;
        }

        MeshBounds bounds;
        mesh.polyData->GetCellsBounds(bounds.data());
        mergeBounds(sceneBounds, bounds);
        haveBounds = true;
    }
    if (!haveBounds) {
        return;
    }

    preparedScene.sceneCenterWorld = centerOfBounds(sceneBounds);
    preparedScene.sceneExtent = std::max({sceneBounds[1] - sceneBounds[0],
                                          sceneBounds[3] - sceneBounds[2],
                                          sceneBounds[5] - sceneBounds[4],
                                          1.0});
}

}

std::vector<Segment3DViewerDialog::PreparedMesh> Segment3DViewerDialog::prepareExplodedMeshes(
    vtkPolyData *combinedPolyData,
    const std::vector<LabelWithColor> &labels)
{
    if (combinedPolyData == nullptr) {
        return {};
    }
    return splitMultiLabelMesh(
        combinedPolyData,
        combinedPolyData->GetCellData()->GetArray("BoundaryLabels"),
        labels);
}

Segment3DViewerDialog::PreparedScene Segment3DViewerDialog::prepareScene(
    dataType::SegmentsImageType::Pointer segImage,
    std::vector<LabelWithColor> labels)
{
    return prepareScene(segImage, std::move(labels), Roi(), false);
}

Segment3DViewerDialog::PreparedScene Segment3DViewerDialog::prepareScene(
    dataType::SegmentsImageType::Pointer segImage,
    std::vector<LabelWithColor> labels,
    Roi requestedBounds)
{
    return prepareScene(segImage, std::move(labels), requestedBounds, false);
}

Segment3DViewerDialog::PreparedScene Segment3DViewerDialog::prepareAllLabelsScene(
    dataType::SegmentsImageType::Pointer segImage,
    std::vector<quint32> labelColors)
{
    if (segImage == nullptr) {
        return {};
    }

    const auto &size = segImage->GetLargestPossibleRegion().GetSize();
    const auto scanStartedAt = QDateTime::currentMSecsSinceEpoch();
    const auto scan = scanAllLabelsAndBounds(
        segImage->GetBufferPointer(),
        static_cast<int>(size[0]),
        static_cast<int>(size[1]),
        static_cast<int>(size[2]));
    SP_LOG_3D_TIMER(scanStartedAt, QStringLiteral("[3DView] [segmentpuzzler] all-label scan"));

    if (scan.labels.empty()) {
        return {};
    }

    std::vector<LabelWithColor> labels;
    labels.reserve(scan.labels.size());
    for (const auto labelId : scan.labels) {
        const auto colorIndex = static_cast<std::size_t>(labelId);
        const quint32 color = colorIndex < labelColors.size() ? labelColors[colorIndex] : 0xFFAAAAAA;
        labels.emplace_back(labelId, color);
    }

    Roi bounds;
    bounds.minX = scan.bounds.minX;
    bounds.maxX = scan.bounds.maxX;
    bounds.minY = scan.bounds.minY;
    bounds.maxY = scan.bounds.maxY;
    bounds.minZ = scan.bounds.minZ;
    bounds.maxZ = scan.bounds.maxZ;
    return prepareScene(segImage, std::move(labels), bounds, true);
}

Segment3DViewerDialog::PreparedScene Segment3DViewerDialog::prepareScene(
    dataType::SegmentsImageType::Pointer segImage,
    std::vector<LabelWithColor> labels,
    Roi requestedBounds,
    bool allLabelsInImage)
{
    PreparedScene preparedScene;
    if (!allLabelsInImage && labels.size() == 1) {
        preparedScene.targetLabelId = labels[0].first;
        preparedScene.windowTitle = QString("Segment %1 (press q to quit)").arg(labels[0].first);
    } else {
        preparedScene.windowTitle = QString("All Segments (press q to quit)");
    }

    if (segImage == nullptr || labels.empty()) {
        return preparedScene;
    }

    const auto *buf = segImage->GetBufferPointer();
    const auto &sz = segImage->GetLargestPossibleRegion().GetSize();
    const int dimX = static_cast<int>(sz[0]);
    const int dimY = static_cast<int>(sz[1]);
    const int dimZ = static_cast<int>(sz[2]);
    const auto spacing = segImage->GetSpacing();
    const auto origin  = segImage->GetOrigin();

    SP_LOG_INFO("viewer.three_d",
                QStringLiteral("[3DView] image dims=%1x%2x%3 spacing=%4,%5,%6 origin=%7,%8,%9 labels=%10")
                    .arg(dimX)
                    .arg(dimY)
                    .arg(dimZ)
                    .arg(spacing[0], 0, 'g', 6)
                    .arg(spacing[1], 0, 'g', 6)
                    .arg(spacing[2], 0, 'g', 6)
                    .arg(origin[0], 0, 'g', 6)
                    .arg(origin[1], 0, 'g', 6)
                    .arg(origin[2], 0, 'g', 6)
                    .arg(labels.size()));

    const qint64 t_total = QDateTime::currentMSecsSinceEpoch();
    BoundsScanResult bounds;
    const bool haveRequestedBounds = requestedBounds.maxX >= requestedBounds.minX &&
                                     requestedBounds.maxY >= requestedBounds.minY &&
                                     requestedBounds.maxZ >= requestedBounds.minZ;
    const bool useRequestedBounds = haveRequestedBounds && (allLabelsInImage || labels.size() == 1);
    if (useRequestedBounds) {
        const qint64 t_bbox = QDateTime::currentMSecsSinceEpoch();
        bounds = clampBoundsToImage(requestedBounds, dimX, dimY, dimZ);
        SP_LOG_3D_TIMER(t_bbox, QStringLiteral("[3DView] [segmentpuzzler] bbox from requested ROI"));
    } else {
        const auto requestedLabelLookup = buildRequestedLabelLookup(labels);
        const qint64 t_bbox = QDateTime::currentMSecsSinceEpoch();
        bounds = scanBoundsForRequestedLabels(buf, dimX, dimY, dimZ, requestedLabelLookup);
        SP_LOG_3D_TIMER(t_bbox, QStringLiteral("[3DView] [segmentpuzzler] bbox scan"));
    }
    const int minX = bounds.minX;
    const int maxX = bounds.maxX;
    const int minY = bounds.minY;
    const int maxY = bounds.maxY;
    const int minZ = bounds.minZ;
    const int maxZ = bounds.maxZ;

    if (maxX < 0) {
        SP_LOG_WARNING("viewer.three_d", QStringLiteral("[3DView] no requested labels found in image, no mesh created"));
        SP_LOG_3D_TIMER(t_total, QStringLiteral("[3DView] total"));
        return preparedScene;
    }

    const int padX0 = std::max(0, minX - 1), padX1 = std::min(dimX - 1, maxX + 1);
    const int padY0 = std::max(0, minY - 1), padY1 = std::min(dimY - 1, maxY + 1);
    const int padZ0 = std::max(0, minZ - 1), padZ1 = std::min(dimZ - 1, maxZ + 1);

    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DView] union bbox x=[%1,%2] y=[%3,%4] z=[%5,%6]")
                     .arg(minX)
                     .arg(maxX)
                     .arg(minY)
                     .arg(maxY)
                     .arg(minZ)
                     .arg(maxZ));

    const qint64 t_voi = QDateTime::currentMSecsSinceEpoch();
    auto importer = vtkSmartPointer<vtkImageImport>::New();
    importer->SetImportVoidPointer(const_cast<dataType::SegmentIdType *>(buf));
    importer->SetDataScalarType(segmentIdVtkDataType());
    importer->SetNumberOfScalarComponents(1);
    importer->SetWholeExtent(0, dimX - 1, 0, dimY - 1, 0, dimZ - 1);
    importer->SetDataExtentToWholeExtent();
    importer->SetDataSpacing(spacing[0], spacing[1], spacing[2]);
    importer->SetDataOrigin(origin[0], origin[1], origin[2]);

    auto extractor = vtkSmartPointer<vtkExtractVOI>::New();
    extractor->SetInputConnection(importer->GetOutputPort());
    extractor->SetVOI(padX0, padX1, padY0, padY1, padZ0, padZ1);

    auto padder = vtkSmartPointer<vtkImageConstantPad>::New();
    padder->SetInputConnection(extractor->GetOutputPort());
    padder->SetOutputWholeExtent(padX0 - 1, padX1 + 1,
                                 padY0 - 1, padY1 + 1,
                                 padZ0 - 1, padZ1 + 1);
    padder->SetConstant(0);
    padder->Update();
    vtkImageData *paddedImage = padder->GetOutput();
    SP_LOG_3D_TIMER(t_voi, QStringLiteral("[3DView] [segmentpuzzler] VOI extract + pad"));

    const qint64 t_surfaces = QDateTime::currentMSecsSinceEpoch();
    if (labels.size() == 1) {
        SP_LOG_INFO("viewer.three_d", QStringLiteral("[3DView] using vtkSurfaceNets3D selected-label extraction"));

        auto surfaceNetLabels = collectLabelsInExtent(
            buf, dimX, dimY, padX0, padX1, padY0, padY1, padZ0, padZ1);
        if (surfaceNetLabels.empty()) {
            surfaceNetLabels.push_back(labels.front().first);
        }

        auto mesh = extractSelectedLabelMesh(
            paddedImage, labels.front().first, labels.front().second, surfaceNetLabels, true);
        if (mesh.polyData != nullptr && mesh.polyData->GetNumberOfPoints() > 0 && mesh.polyData->GetNumberOfCells() > 0) {
            preparedScene.meshes.push_back(std::move(mesh));
        }
    } else {
        SP_LOG_INFO(
            "viewer.three_d",
            allLabelsInImage
                ? QStringLiteral("[3DView] using vtkSurfaceNets3D default all-label extraction")
                : QStringLiteral("[3DView] using vtkSurfaceNets3D selected multi-label extraction"));

        auto requestedLabels = collectRequestedLabels(labels);
        std::vector<dataType::SegmentIdType> surfaceNetLabels;
        if (allLabelsInImage) {
            surfaceNetLabels = requestedLabels;
        } else {
            const qint64 t_collectLabels =
                kProfile3DViewExtraction ? QDateTime::currentMSecsSinceEpoch() : 0;
            surfaceNetLabels = collectLabelsInExtent(
                buf, dimX, dimY, padX0, padX1, padY0, padY1, padZ0, padZ1);
            if (kProfile3DViewExtraction) {
                SP_LOG_3D_TIMER(t_collectLabels, QStringLiteral("[3DView] [segmentpuzzler] collect labels in VOI"));
            }
        }

        const qint64 t_extraction = kProfile3DViewExtraction ? QDateTime::currentMSecsSinceEpoch() : 0;
        auto surfaceNet = createSurfaceNet(paddedImage, surfaceNetLabels);
        vtkSmartPointer<vtkPolyData> combinedPolyData;
        if (allLabelsInImage) {
            surfaceNet->SetOutputStyleToDefault();
            surfaceNet->Update();
            combinedPolyData = detachPolyData(surfaceNet->GetOutput());
        } else {
            surfaceNet->SetOutputStyleToSelected();
            combinedPolyData = extractSelectedSurfaceNetOutput(surfaceNet, requestedLabels);
        }
        if (kProfile3DViewExtraction) {
            SP_LOG_3D_TIMER(
                t_extraction,
                allLabelsInImage
                    ? QStringLiteral("[3DView] [vtksurfacenets] default all-label extraction")
                    : QStringLiteral("[3DView] [vtksurfacenets] selected extraction"));
        }
        SP_LOG_DEBUG("viewer.three_d",
                     QStringLiteral("[3DView] SurfaceNets source mesh labelsInVOI=%1 requested=%2 points=%3 cells=%4")
                         .arg(surfaceNetLabels.size())
                         .arg(requestedLabels.size())
                         .arg(combinedPolyData->GetNumberOfPoints())
                         .arg(combinedPolyData->GetNumberOfCells()));

        preparedScene.meshes = prepareExplodedMeshes(combinedPolyData, labels);
        if (preparedScene.meshes.size() != labels.size()) {
            SP_LOG_WARNING("viewer.three_d",
                           QStringLiteral("[3DView] incomplete segment mesh split: requested=%1 produced=%2")
                               .arg(labels.size())
                               .arg(preparedScene.meshes.size()));
            preparedScene.meshes.clear();
        }

        if (preparedScene.meshes.empty() && !allLabelsInImage) {
            SP_LOG_WARNING("viewer.three_d",
                           QStringLiteral("[3DView] selected mesh split failed, falling back to default-output extraction"));

            combinedPolyData = nullptr;
            surfaceNet = nullptr;

            auto fallbackSurfaceNet = vtkSmartPointer<vtkSurfaceNets3D>::New();
            fallbackSurfaceNet->SetInputData(paddedImage);
            configureSurfaceNet(fallbackSurfaceNet);
            fallbackSurfaceNet->DataCachingOn();
            fallbackSurfaceNet->SetOutputStyleToDefault();
            setSurfaceNetLabels(fallbackSurfaceNet, requestedLabels);
            const qint64 t_legacyExtraction = kProfile3DViewExtraction ? QDateTime::currentMSecsSinceEpoch() : 0;
            fallbackSurfaceNet->Update();
            auto polyData = detachPolyData(fallbackSurfaceNet->GetOutput());
            if (kProfile3DViewExtraction) {
                SP_LOG_3D_TIMER(t_legacyExtraction, QStringLiteral("[3DView] [vtksurfacenets] legacy extraction"));
            }
            SP_LOG_DEBUG("viewer.three_d",
                         QStringLiteral("[3DView] legacy multi-label mesh points=%1 cells=%2")
                             .arg(polyData->GetNumberOfPoints())
                             .arg(polyData->GetNumberOfCells()));

            auto *boundaryLabels = polyData->GetCellData()->GetArray("BoundaryLabels");
            const bool canSplitLegacy =
                boundaryLabels != nullptr && boundaryLabels->GetNumberOfComponents() >= 2;
            if (!canSplitLegacy) {
                SP_LOG_WARNING("viewer.three_d", QStringLiteral("[3DView] legacy multi-label split unavailable"));
            }
            if (canSplitLegacy) {
                preparedScene.meshes = prepareExplodedMeshes(polyData, labels);
                if (preparedScene.meshes.size() != labels.size()) {
                    SP_LOG_WARNING("viewer.three_d",
                                   QStringLiteral("[3DView] incomplete fallback segment mesh split: requested=%1 produced=%2")
                                       .arg(labels.size())
                                       .arg(preparedScene.meshes.size()));
                    preparedScene.meshes.clear();
                }
            }
        }

        if (preparedScene.meshes.empty() && !allLabelsInImage) {
            SP_LOG_WARNING("viewer.three_d",
                           QStringLiteral("[3DView] mesh split failed, falling back to per-label extraction"));
            preparedScene.meshes = extractSingleLabelMeshes(paddedImage, labels, false);
            if (preparedScene.meshes.size() != labels.size()) {
                SP_LOG_WARNING("viewer.three_d",
                               QStringLiteral("[3DView] incomplete per-label fallback: requested=%1 produced=%2")
                                   .arg(labels.size())
                                   .arg(preparedScene.meshes.size()));
                preparedScene.meshes.clear();
            }
        } else if (preparedScene.meshes.empty()) {
            SP_LOG_WARNING("viewer.three_d",
                           QStringLiteral("[3DView] all-label mesh split failed; no 3D scene was created"));
        }
    }
    if (!preparedScene.meshes.empty()) {
        updateSceneBoundsFromMeshes(preparedScene);

        auto *sharedPoints = preparedScene.meshes.front().polyData->GetPoints();
        vtkIdType segmentCellCount = 0;
        bool allMeshesSharePoints = true;
        for (const auto &mesh : preparedScene.meshes) {
            segmentCellCount += mesh.polyData->GetNumberOfCells();
            allMeshesSharePoints &= mesh.polyData->GetPoints() == sharedPoints;
        }
        SP_LOG_DEBUG("viewer.three_d",
                     QStringLiteral("[3DView] segment meshes=%1 sharedPoints=%2 sharedPointCount=%3 cells=%4")
                         .arg(preparedScene.meshes.size())
                         .arg(allMeshesSharePoints)
                         .arg(allMeshesSharePoints && sharedPoints != nullptr
                                  ? sharedPoints->GetNumberOfPoints()
                                  : 0)
                         .arg(segmentCellCount));
    }
    SP_LOG_3D_TIMER(t_surfaces, QStringLiteral("[3DView] [total] surface extraction"));
    SP_LOG_3D_TIMER(t_total, QStringLiteral("[3DView] [total] total"));

    return preparedScene;
}

Segment3DViewerDialog::Segment3DViewerDialog(PreparedScene preparedScene,
                                             QWidget *parent,
                                             int launchSliceAxis)
    : Segment3DViewerDialog(std::move(preparedScene), CutSessionConfig{}, parent, launchSliceAxis)
{
}

Segment3DViewerDialog::Segment3DViewerDialog(PreparedScene preparedScene,
                                             CutSessionConfig cutSession,
                                             QWidget *parent,
                                             int launchSliceAxis)
    : QDialog(parent)
    , m_targetLabelId(preparedScene.targetLabelId)
    , m_cutSession(std::move(cutSession))
    , m_launchSliceAxis(launchSliceAxis)
{
    setWindowTitle(preparedScene.windowTitle);
    resize(600, 600);

    auto renWin = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    m_renderer = vtkSmartPointer<vtkRenderer>::New();
    m_renderer->SetBackground(0.1, 0.1, 0.1);
    renWin->AddRenderer(m_renderer);

    m_segmentActors.reserve(preparedScene.meshes.size());
    for (const auto &mesh : preparedScene.meshes) {
        if (mesh.polyData == nullptr || mesh.polyData->GetNumberOfPoints() == 0 || mesh.polyData->GetNumberOfCells() == 0) {
            continue;
        }

        auto actor = createMeshActor(mesh);
        m_renderer->AddActor(actor);
        m_segmentActors.push_back({actor, mesh.labelId, mesh.centerWorld});
    }

    m_sceneCenterWorld = preparedScene.sceneCenterWorld;
    m_sceneExtent = preparedScene.sceneExtent;
    const bool showExplodeControls = m_segmentActors.size() > 1;
    const bool showCutControls = static_cast<bool>(m_cutSession.applyCut) && m_targetLabelId != 0;

    m_vtkWidget = new QVTKOpenGLNativeWidget(this);
    m_vtkWidget->setRenderWindow(renWin);
    m_vtkWidget->setEnableTouchEventProcessing(false);
    m_vtkWidget->setMouseTracking(false);
    m_vtkWidget->setAttribute(Qt::WA_Hover, false);
    QSizePolicy vtkSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    vtkSizePolicy.setRetainSizeWhenHidden(true);
    m_vtkWidget->setSizePolicy(vtkSizePolicy);
    m_vtkWidget->setFocusPolicy(Qt::StrongFocus);
    m_vtkWidget->installEventFilter(this);
    m_vtkWidget->hide();

    auto style = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
    style->SetDefaultRenderer(m_renderer);
    if (m_vtkWidget->interactor() != nullptr) {
        m_vtkWidget->interactor()->SetInteractorStyle(style);
    }

    auto axes = vtkSmartPointer<vtkAxesActor>::New();
    m_orientationWidget = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    m_orientationWidget->SetOrientationMarker(axes);
    m_orientationWidget->SetInteractor(m_vtkWidget->interactor());
    m_orientationWidget->SetViewport(0.0, 0.0, 0.16, 0.16);
    m_orientationWidget->SetEnabled(1);
    m_orientationWidget->InteractiveOff();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *vtkContainer = new QWidget(this);
    auto *vtkStackLayout = new QGridLayout(vtkContainer);
    vtkStackLayout->setContentsMargins(0, 0, 0, 0);
    vtkStackLayout->setSpacing(0);
    vtkStackLayout->addWidget(m_vtkWidget, 0, 0);
    if (showCutControls) {
        m_cutOverlay = new CutStrokeOverlay(vtkContainer);
        m_cutOverlay->onStrokeChanged = [this]() { updateCutUiState(); };
        vtkStackLayout->addWidget(m_cutOverlay, 0, 0);
    }
    layout->addWidget(vtkContainer, 1);

    if (showExplodeControls || showCutControls) {

        m_controlsWidget = new QWidget(this);
        m_controlsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *controlsRow = new QHBoxLayout(m_controlsWidget);
        controlsRow->setContentsMargins(10, 6, 10, 10);
        controlsRow->setSpacing(8);

        if (showExplodeControls) {
            m_explodeSlider = new QSlider(Qt::Horizontal, m_controlsWidget);
            m_explodeSlider->setObjectName(QStringLiteral("explodeSlider"));
            m_explodeSlider->setRange(0, kMaximumExplodePercent);
            m_explodeSlider->setSingleStep(2);
            m_explodeSlider->setPageStep(10);
            m_explodeSlider->setValue(0);
            m_explodeSlider->setFocusPolicy(Qt::StrongFocus);
            m_explodeSlider->setToolTip(
                QStringLiteral("Explode distance: 0% shows the original view; 100% doubles each segment's distance from the scene center; maximum %1%.")
                    .arg(kMaximumExplodePercent));
            m_explodeSlider->setMinimumHeight(28);

            connect(m_explodeSlider, &QSlider::valueChanged, this, [this](int value) {
                if (m_segmentActors.empty()) {
                    return;
                }
                const double explodeFactor = static_cast<double>(value) / 100.0;
                for (const auto &actorInfo : m_segmentActors) {
                    if (actorInfo.actor == nullptr) {
                        continue;
                    }
                    const double dx = actorInfo.centerWorld[0] - m_sceneCenterWorld[0];
                    const double dy = actorInfo.centerWorld[1] - m_sceneCenterWorld[1];
                    const double dz = actorInfo.centerWorld[2] - m_sceneCenterWorld[2];
                    actorInfo.actor->SetPosition(explodeFactor * dx,
                                                 explodeFactor * dy,
                                                 explodeFactor * dz);
                }
                m_renderer->ResetCameraClippingRange();
                if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
                    m_vtkWidget->renderWindow()->Render();
                }
            });
            controlsRow->addWidget(new QLabel(QStringLiteral("Explode:"), m_controlsWidget));
            controlsRow->addWidget(m_explodeSlider, 1);
        }

        if (showCutControls) {
            if (showExplodeControls) {
                controlsRow->addSpacing(8);
            }
            m_drawCutButton = new QPushButton(QStringLiteral("Draw Cut"), m_controlsWidget);
            m_clearCutButton = new QPushButton(QStringLiteral("Clear"), m_controlsWidget);
            m_applyCutButton = new QPushButton(QStringLiteral("Apply"), m_controlsWidget);
            connect(m_drawCutButton, &QPushButton::clicked, this, &Segment3DViewerDialog::beginCutDrawing);
            connect(m_clearCutButton, &QPushButton::clicked, this, &Segment3DViewerDialog::clearCutStroke);
            connect(m_applyCutButton, &QPushButton::clicked, this, &Segment3DViewerDialog::applyProjectedCut);
            controlsRow->addWidget(m_drawCutButton);
            controlsRow->addWidget(m_clearCutButton);
            controlsRow->addWidget(m_applyCutButton);
        }

        if (!showExplodeControls) {
            controlsRow->addStretch(1);
        }

        if (QLabel *helpLabel = createHelpBadgeLabel(
                threeDViewHelpText(showExplodeControls, showCutControls), m_controlsWidget)) {
            controlsRow->addWidget(helpLabel);
        }

        layout->addWidget(m_controlsWidget, 0);
    }

    if (m_vtkWidget != nullptr && m_vtkWidget->interactor() != nullptr) {
        auto *interactor = m_vtkWidget->interactor();
        auto navigationObserver = vtkSmartPointer<vtkCallbackCommand>::New();
        navigationObserver->SetClientData(this);
        navigationObserver->SetCallback([](vtkObject *, unsigned long eventId, void *clientData, void *) {
            if (eventId != vtkCommand::LeftButtonPressEvent || clientData == nullptr) {
                return;
            }

            auto *dialog = static_cast<Segment3DViewerDialog *>(clientData);
            dialog->handleInteractorLeftButtonPress();
        });
        interactor->AddObserver(vtkCommand::LeftButtonPressEvent, navigationObserver);
    }

    auto *closeShortcut = new QShortcut(QKeySequence(Qt::Key_Q), this);
    closeShortcut->setContext(Qt::WindowShortcut);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::close);

    if (showCutControls) {
        auto *cutHelpShortcut = new QShortcut(QKeySequence(Qt::Key_Question), this);
        cutHelpShortcut->setContext(Qt::WindowShortcut);
        connect(cutHelpShortcut, &QShortcut::activated, this, &Segment3DViewerDialog::showCutHelp);

        auto *cutHelpF1Shortcut = new QShortcut(QKeySequence(Qt::Key_F1), this);
        cutHelpF1Shortcut->setContext(Qt::WindowShortcut);
        connect(cutHelpF1Shortcut, &QShortcut::activated, this, &Segment3DViewerDialog::showCutHelp);
    }

    if (showExplodeControls) {
        auto *stepLeftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
        stepLeftShortcut->setContext(Qt::WindowShortcut);
        connect(stepLeftShortcut, &QShortcut::activated, this, [this]() { stepExplodeSlider(-1); });

        auto *stepRightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
        stepRightShortcut->setContext(Qt::WindowShortcut);
        connect(stepRightShortcut, &QShortcut::activated, this, [this]() { stepExplodeSlider(1); });
    }

    updateCutUiState();
}

std::optional<Segment3DViewerDialog::CameraOrientation>
Segment3DViewerDialog::cameraOrientationForSliceAxis(int sliceAxis)
{
    switch (sliceAxis) {
        case 0:
            return CameraOrientation{{-1.0, 0.0, 0.0}, {0.0, -1.0, 0.0}};
        case 1:
            return CameraOrientation{{0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}};
        case 2:
            return CameraOrientation{{0.0, 0.0, 1.0}, {0.0, -1.0, 0.0}};
        default:
            return std::nullopt;
    }
}

void Segment3DViewerDialog::setNavigateToLabelHandler(NavigateToLabelHandler handler) {
    m_navigateToLabelHandler = std::move(handler);
}

void Segment3DViewerDialog::presentInFront() {
    show();
    raiseAndRequestActivation();
    QTimer::singleShot(0, this, [this]() { raiseAndRequestActivation(); });
    QTimer::singleShot(100, this, [this]() { raiseAndRequestActivation(); });
}

void Segment3DViewerDialog::raiseAndRequestActivation() {
    if (!isVisible()) {
        return;
    }

    raise();
    activateWindow();
    if (windowHandle() != nullptr) {
        windowHandle()->requestActivate();
    }
}

void Segment3DViewerDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);

    if (m_initialFrameScheduled || m_initialFrameRendered) {
        return;
    }

    m_initialFrameScheduled = true;
    QTimer::singleShot(0, this, [this]() { finishInitialRender(); });
}

void Segment3DViewerDialog::applyInitialCameraOrientation(int launchSliceAxis) {
    if (m_renderer == nullptr) {
        return;
    }

    vtkCamera *camera = m_renderer->GetActiveCamera();
    if (camera == nullptr) {
        return;
    }

    const auto orientation = cameraOrientationForSliceAxis(launchSliceAxis);
    if (!orientation.has_value()) {
        return;
    }

    const double distance = safeCameraDistance(camera->GetDistance(), m_sceneExtent);
    camera->SetFocalPoint(m_sceneCenterWorld[0], m_sceneCenterWorld[1], m_sceneCenterWorld[2]);
    camera->SetPosition(m_sceneCenterWorld[0] - orientation->lookDirection[0] * distance,
                        m_sceneCenterWorld[1] - orientation->lookDirection[1] * distance,
                        m_sceneCenterWorld[2] - orientation->lookDirection[2] * distance);
    camera->SetViewUp(orientation->viewUp[0], orientation->viewUp[1], orientation->viewUp[2]);
    camera->OrthogonalizeViewUp();

    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DView] applied launch axis=%1 center=%2,%3,%4 distance=%5")
                     .arg(launchSliceAxis)
                     .arg(m_sceneCenterWorld[0], 0, 'g', 6)
                     .arg(m_sceneCenterWorld[1], 0, 'g', 6)
                     .arg(m_sceneCenterWorld[2], 0, 'g', 6)
                     .arg(distance, 0, 'g', 6));
}

void Segment3DViewerDialog::finishInitialRender() {
    if (m_initialFrameRendered) {
        return;
    }

    if (!isVisible()) {
        m_initialFrameScheduled = false;
        return;
    }

    m_initialFrameRendered = true;

    if (m_vtkWidget != nullptr && m_vtkWidget->interactor() != nullptr) {
        m_vtkWidget->interactor()->Initialize();
        m_vtkWidget->interactor()->Enable();
    }

    if (m_renderer != nullptr && !m_segmentActors.empty()) {
        m_renderer->ResetCamera();
        applyInitialCameraOrientation(m_launchSliceAxis);
        m_renderer->ResetCameraClippingRange();
    }

    if (m_vtkWidget != nullptr) {
        m_vtkWidget->show();
    }

    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }

    if (m_controlsWidget != nullptr) {
        m_controlsWidget->raise();
    }
    if (m_cutOverlay != nullptr) {
        m_cutOverlay->raise();
    }

    const bool showExplodeControls = m_explodeSlider != nullptr;
    const bool showCutControls = m_cutOverlay != nullptr;
    SP_LOG_DEBUG(
        "viewer.three_d",
        QStringLiteral("[3DInputDebug] ready targetLabel=%1 segmentActorCount=%2 "
                       "showExplodeControls=%3 showCutControls=%4 interactorEnabled=%5")
            .arg(m_targetLabelId)
            .arg(m_segmentActors.size())
            .arg(showExplodeControls)
            .arg(showCutControls)
            .arg(m_vtkWidget != nullptr
                 && m_vtkWidget->interactor() != nullptr
                 && m_vtkWidget->interactor()->GetEnabled()));

    m_initialFrameScheduled = false;
}

bool Segment3DViewerDialog::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_vtkWidget && event != nullptr) {
        switch (event->type()) {
            case QEvent::HoverEnter:
            case QEvent::HoverMove:
            case QEvent::HoverLeave:
            case QEvent::TouchBegin:
            case QEvent::TouchUpdate:
            case QEvent::TouchEnd:
            case QEvent::TouchCancel:
                event->accept();
                return true;

            case QEvent::MouseMove: {
                auto *mouseEvent = static_cast<QMouseEvent *>(event);
                if (mouseEvent->buttons() == Qt::NoButton) {
                    event->accept();
                    return true;
                }
                break;
            }

            case QEvent::MouseButtonPress:
                if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
                    auto *mouseEvent = static_cast<QMouseEvent *>(event);
                    if (mouseEvent->button() == Qt::LeftButton) {
                        const double devicePixelRatio = m_vtkWidget->devicePixelRatioF();
                        const int pickX = static_cast<int>(std::lround(mouseEvent->pos().x() * devicePixelRatio));
                        const int pickY = static_cast<int>(std::lround(
                            (m_vtkWidget->height() - mouseEvent->pos().y() - 1) * devicePixelRatio));
                        const Qt::KeyboardModifiers effectiveModifiers =
                            mouseEvent->modifiers() | QApplication::keyboardModifiers();
                        if (tryNavigateToPickedLabel(pickX, pickY, effectiveModifiers, "qt")) {
                            event->accept();
                            return true;
                        }
                    }
                }
                break;

            default:
                break;
        }
    }

    return QDialog::eventFilter(watched, event);
}

void Segment3DViewerDialog::stepExplodeSlider(int direction) {
    if (m_explodeSlider == nullptr) {
        return;
    }

    m_explodeSlider->triggerAction(direction < 0
                                       ? QAbstractSlider::SliderSingleStepSub
                                       : QAbstractSlider::SliderSingleStepAdd);
}

void Segment3DViewerDialog::beginCutDrawing() {
    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DCutDebug] beginCutDrawing targetLabel=%1 hasApplyCallback=%2")
                     .arg(m_targetLabelId)
                     .arg(static_cast<bool>(m_cutSession.applyCut)));

    if (m_cutOverlay == nullptr || m_cutApplyInFlight) {
        return;
    }

    m_cutDrawModeActive = true;
    m_cutOverlay->setDrawingEnabled(true);
    m_cutOverlay->raise();
    updateCutUiState();
}

void Segment3DViewerDialog::clearCutStroke() {
    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DCutDebug] clearCutStroke targetLabel=%1").arg(m_targetLabelId));

    if (m_cutOverlay == nullptr || m_cutApplyInFlight) {
        return;
    }
    m_cutOverlay->clearStroke();
    updateCutUiState();
}

Projected3DCutRequest Segment3DViewerDialog::buildProjected3DCutRequest() const {
    Projected3DCutRequest request;
    request.targetWorkingLabel = m_targetLabelId;
    request.strokeWidthPixels = 6.0;

    if (m_cutOverlay != nullptr) {
        request.viewportSize = {m_cutOverlay->width(), m_cutOverlay->height()};
        request.strokePixels = m_cutOverlay->strokePixels();
    } else if (m_vtkWidget != nullptr) {
        request.viewportSize = {m_vtkWidget->width(), m_vtkWidget->height()};
    }

    if (m_renderer != nullptr && m_renderer->GetActiveCamera() != nullptr) {
        vtkCamera *camera = m_renderer->GetActiveCamera();
        vtkMatrix4x4 *matrix = camera->GetCompositeProjectionTransformMatrix(
            m_renderer->GetTiledAspectRatio(), 0.0, 1.0);
        if (matrix != nullptr) {
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    request.worldToNdcMatrix[static_cast<std::size_t>(row * 4 + col)] =
                        matrix->GetElement(row, col);
                }
            }
        }
    }

    return request;
}

void Segment3DViewerDialog::applyProjectedCut() {
    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DCutDebug] applyProjectedCut targetLabel=%1 hasApplyCallback=%2")
                     .arg(m_targetLabelId)
                     .arg(static_cast<bool>(m_cutSession.applyCut)));

    if (!m_cutSession.applyCut) {
        return;
    }
    if (m_cutOverlay == nullptr || !m_cutOverlay->hasValidStroke()) {
        QMessageBox::information(this, tr("3D Cut"), tr("Draw a cut stroke before applying the split."));
        return;
    }

    const Projected3DCutRequest request = buildProjected3DCutRequest();
    if (request.targetWorkingLabel == 0 || request.strokePixels.size() < 2) {
        QMessageBox::information(this, tr("3D Cut"), tr("The cut request is incomplete."));
        return;
    }

    if (m_cutSession.preflightWarning) {
        const QString warningMessage = m_cutSession.preflightWarning(request);
        if (!warningMessage.isEmpty()) {
            QMessageBox::warning(this, tr("3D Cut"), warningMessage);
        }
    }

    m_cutApplyInFlight = true;
    updateCutUiState();

    const auto finishApply = [this](CutApplyResult result) {
        if (result.mutated) {
            if (!result.message.isEmpty()) {
                QMessageBox::warning(this, tr("3D Cut"), result.message);
            }
            accept();
            return;
        }

        m_cutApplyInFlight = false;
        updateCutUiState();
        QMessageBox::information(this,
                                 tr("3D Cut"),
                                 result.message.isEmpty()
                                     ? tr("The painted cut did not split the working segment.")
                                     : result.message);
    };

    const auto applyCut = m_cutSession.applyCut;
    if (m_cutSession.taskRunner != nullptr) {
        m_cutSession.taskRunner->runWithLabel(
            m_cutSession.progressText.isEmpty()
                ? QStringLiteral("Applying 3D cut...")
                : m_cutSession.progressText,
            [applyCut, request]() {
                return applyCut(request);
            },
            [finishApply](CutApplyResult result) mutable {
                finishApply(std::move(result));
            });
        return;
    }

    finishApply(applyCut(request));
}

bool Segment3DViewerDialog::tryNavigateToPickedLabel(int pickX,
                                                     int pickY,
                                                     Qt::KeyboardModifiers modifiers,
                                                     const char *sourceTag) {
    QElapsedTimer totalTimer;
    totalTimer.start();

    const Qt::KeyboardModifiers effectiveModifiers = modifiers | QApplication::keyboardModifiers();
    const bool hasNavigateHandler = static_cast<bool>(m_navigateToLabelHandler);
    const bool modifierActive = navigationModifierPressed(effectiveModifiers);
    const char *result = "skipped";

    if (!hasNavigateHandler) {
        result = "skipped_no_handler";
    } else if (!modifierActive) {
        result = "skipped_no_modifier";
    } else if (m_renderer == nullptr) {
        result = "skipped_no_renderer";
    } else {
        QElapsedTimer pickTimer;
        pickTimer.start();

        auto picker = vtkSmartPointer<vtkPropPicker>::New();
        vtkProp *pickedProp = nullptr;
        if (picker->Pick(pickX, pickY, 0.0, m_renderer) != 0) {
            pickedProp = picker->GetViewProp();
        }
        const qint64 pickNanoseconds = pickTimer.nsecsElapsed();

        dataType::SegmentIdType pickedLabelId = 0;
        if (pickedProp != nullptr) {
            for (const auto &actorInfo : m_segmentActors) {
                if (actorInfo.actor == nullptr || actorInfo.labelId == 0) {
                    continue;
                }
                if (pickedProp == actorInfo.actor.GetPointer()) {
                    pickedLabelId = actorInfo.labelId;
                    break;
                }
            }
        }

        qint64 dispatchNanoseconds = 0;
        bool navigated = false;
        if (pickedLabelId != 0) {
            QElapsedTimer dispatchTimer;
            dispatchTimer.start();
            m_navigateToLabelHandler(pickedLabelId);
            dispatchNanoseconds = dispatchTimer.nsecsElapsed();
            navigated = true;
            result = "navigated";
        } else {
            result = "pick_miss";
        }

        SP_LOG_DEBUG(
            "viewer.three_d",
            QStringLiteral("[3DInputProfile] syncClick source=%1 pickPos=%2,%3 modifiers=%4 targetLabel=%5 "
                           "pickedLabelId=%6 pickedPropPresent=%7 pickMs=%8 dispatchMs=%9 totalMs=%10 result=%11")
                .arg(QString::fromUtf8(sourceTag != nullptr ? sourceTag : "unknown"))
                .arg(pickX)
                .arg(pickY)
                .arg(static_cast<int>(effectiveModifiers))
                .arg(m_targetLabelId)
                .arg(pickedLabelId)
                .arg(pickedProp != nullptr)
                .arg(elapsedMilliseconds(pickNanoseconds), 0, 'f', 3)
                .arg(elapsedMilliseconds(dispatchNanoseconds), 0, 'f', 3)
                .arg(elapsedMilliseconds(totalTimer.nsecsElapsed()), 0, 'f', 3)
                .arg(QString::fromUtf8(result)));

        return navigated;
    }

    SP_LOG_DEBUG(
        "viewer.three_d",
        QStringLiteral("[3DInputProfile] syncClick source=%1 pickPos=%2,%3 modifiers=%4 targetLabel=%5 "
                       "pickedLabelId=0 pickedPropPresent=0 pickMs=0.000 dispatchMs=0.000 totalMs=%6 result=%7")
            .arg(QString::fromUtf8(sourceTag != nullptr ? sourceTag : "unknown"))
            .arg(pickX)
            .arg(pickY)
            .arg(static_cast<int>(effectiveModifiers))
            .arg(m_targetLabelId)
            .arg(elapsedMilliseconds(totalTimer.nsecsElapsed()), 0, 'f', 3)
            .arg(QString::fromUtf8(result)));
    return false;
}

void Segment3DViewerDialog::handleInteractorLeftButtonPress() {
    if (m_vtkWidget == nullptr || m_vtkWidget->interactor() == nullptr) {
        return;
    }

    int eventPosition[2] = {0, 0};
    m_vtkWidget->interactor()->GetEventPosition(eventPosition);
    tryNavigateToPickedLabel(eventPosition[0],
                            eventPosition[1],
                            QApplication::keyboardModifiers(),
                            "vtk");
}

void Segment3DViewerDialog::showCutHelp() {
    QMessageBox::information(
        this,
        tr("3D Cut Help"),
        tr("1. Rotate, pan, and zoom the segment until the intended cut is visible.\n"
           "2. Press Draw Cut to freeze navigation and arm stroke drawing.\n"
           "3. Hold the left mouse button and paint the cut line across the 3D view.\n"
           "4. Use Clear if you want to redraw the stroke.\n"
           "5. Press Apply to project the painted stroke through the segment and split it.\n"
           "6. Cancel or close the dialog to leave the graph unchanged.\n\n"
           "Shortcuts: ? or F1 opens this helper, Q closes the dialog."));
}

void Segment3DViewerDialog::updateCutUiState() {
    const bool cutEnabled = m_cutOverlay != nullptr && static_cast<bool>(m_cutSession.applyCut);
    if (!cutEnabled) {
        return;
    }

    const bool hasStroke = m_cutOverlay->hasValidStroke();
    m_cutOverlay->setDrawingEnabled(m_cutDrawModeActive && !m_cutApplyInFlight);

    if (m_drawCutButton != nullptr) {
        m_drawCutButton->setEnabled(!m_cutApplyInFlight && !m_cutDrawModeActive);
        m_drawCutButton->setText(m_cutDrawModeActive ? QStringLiteral("Cut Drawing Active")
                                                     : QStringLiteral("Draw Cut"));
    }
    if (m_clearCutButton != nullptr) {
        m_clearCutButton->setEnabled(!m_cutApplyInFlight && hasStroke);
    }
    if (m_applyCutButton != nullptr) {
        m_applyCutButton->setEnabled(!m_cutApplyInFlight && m_cutDrawModeActive && hasStroke);
    }
}
