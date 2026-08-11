#include "Segment3DViewerDialog.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QGridLayout>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineF>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QRandomGenerator>
#include <QShortcut>
#include <QShowEvent>
#include <QSaveFile>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSpinBox>
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
#include <vtkCellPicker.h>
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
#include <vtkSphereSource.h>
#include <vtkSurfaceNets3D.h>

#include <itkImageFileWriter.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#ifdef USE_OMP
#include <omp.h>
#endif

#include "src/utils/utils.h"

namespace {

constexpr int kDefaultSeedDistancePixels = 10;
constexpr int kMaximumSeedDistancePixels = 80;
constexpr int kDefaultLineSamplingPixels = 30;
constexpr int kMinimumLineSamplingPixels = 5;
constexpr int kMaximumLineSamplingPixels = 100;
constexpr int kMaximumSplitLineSeedPairs = 64;
constexpr int kSmoothingSliderStepsPerPixel = 10;
constexpr int kMaximumSmoothingSliderValue = 30;

class SurfaceOnlyTrackballCameraStyle : public vtkInteractorStyleTrackballCamera {
public:
    static SurfaceOnlyTrackballCameraStyle *New();
    vtkTypeMacro(SurfaceOnlyTrackballCameraStyle, vtkInteractorStyleTrackballCamera);

    void OnChar() override {
        const char key = Interactor != nullptr ? Interactor->GetKeyCode() : '\0';
        if (key == 'w' || key == 'W') {
            return;
        }
        Superclass::OnChar();
    }
};

vtkStandardNewMacro(SurfaceOnlyTrackballCameraStyle);

QPointF mouseEventPosition(const QMouseEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

QPolygonF offsetPolyline(const QPolygonF &points, double distance) {
    QPolygonF offsetPoints;
    offsetPoints.reserve(points.size());
    for (int index = 0; index < points.size(); ++index) {
        const QPointF &before = points[index == 0 ? 0 : index - 1];
        const QPointF &after = points[index + 1 < points.size() ? index + 1 : index];
        const double dx = after.x() - before.x();
        const double dy = after.y() - before.y();
        const double length = std::hypot(dx, dy);
        if (length <= 1e-9) {
            offsetPoints << points[index];
            continue;
        }
        offsetPoints << QPointF(
            points[index].x() - distance * dy / length,
            points[index].y() + distance * dx / length);
    }
    return offsetPoints;
}

double polylineLength(const QPolygonF &points) {
    double length = 0.0;
    for (int index = 1; index < points.size(); ++index) {
        length += QLineF(points[index - 1], points[index]).length();
    }
    return length;
}

std::vector<QPointF> samplePolyline(const QPolygonF &points, int sampleCount) {
    std::vector<QPointF> samples;
    if (points.size() < 2 || sampleCount <= 0) {
        return samples;
    }
    std::vector<double> accumulatedLength(static_cast<std::size_t>(points.size()), 0.0);
    for (int index = 1; index < points.size(); ++index) {
        accumulatedLength[static_cast<std::size_t>(index)] =
            accumulatedLength[static_cast<std::size_t>(index - 1)]
            + QLineF(points[index - 1], points[index]).length();
    }
    const double totalLength = accumulatedLength.back();
    if (totalLength <= 1e-9) {
        return samples;
    }
    samples.reserve(static_cast<std::size_t>(sampleCount));
    int segment = 1;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const double targetLength =
            (static_cast<double>(sample) + 0.5) * totalLength / sampleCount;
        while (segment + 1 < points.size()
               && accumulatedLength[static_cast<std::size_t>(segment)] < targetLength) {
            ++segment;
        }
        const double segmentStart = accumulatedLength[static_cast<std::size_t>(segment - 1)];
        const double segmentLength =
            accumulatedLength[static_cast<std::size_t>(segment)] - segmentStart;
        const double fraction = segmentLength > 1e-9
                                    ? (targetLength - segmentStart) / segmentLength
                                    : 0.0;
        samples.push_back(points[segment - 1]
                          + fraction * (points[segment] - points[segment - 1]));
    }
    return samples;
}

template<typename Values>
QString formatTriple(const Values &values) {
    return QStringLiteral("[%1,%2,%3]")
        .arg(QString::number(static_cast<double>(values[0]), 'g', 17))
        .arg(QString::number(static_cast<double>(values[1]), 'g', 17))
        .arg(QString::number(static_cast<double>(values[2]), 'g', 17));
}

QString formatHash(std::uint64_t hash) {
    return QStringLiteral("0x%1").arg(
        QString::number(static_cast<qulonglong>(hash), 16).rightJustified(16, QLatin1Char('0')));
}

QString formatSeedIndices(
    const std::vector<segment_puzzler::SeededWatershedSplitSession::IndexType> &seeds,
    const segment_puzzler::SeededWatershedSplitSession::IndexType *offset = nullptr)
{
    QStringList values;
    values.reserve(static_cast<int>(seeds.size()));
    for (auto seed : seeds) {
        if (offset != nullptr) {
            for (unsigned int axis = 0; axis < 3; ++axis) {
                seed[axis] += (*offset)[axis];
            }
        }
        values << formatTriple(seed);
    }
    return QStringLiteral("[%1]").arg(values.join(QLatin1Char(',')));
}

QString formatSeedDistances(
    const std::vector<segment_puzzler::SeededWatershedSplitSession::IndexType> &seeds,
    const segment_puzzler::SeededSplitDistanceImage *distance)
{
    QStringList values;
    values.reserve(static_cast<int>(seeds.size()));
    for (const auto &seed : seeds) {
        values << QString::number(distance->GetPixel(seed), 'g', 9);
    }
    return QStringLiteral("[%1]").arg(values.join(QLatin1Char(',')));
}

template<typename Values>
QJsonArray jsonTriple(const Values &values) {
    return {static_cast<double>(values[0]),
            static_cast<double>(values[1]),
            static_cast<double>(values[2])};
}

template<typename TImage>
void writeDebugImage(const TImage *image, const QString &filePath) {
    using Writer = itk::ImageFileWriter<TImage>;
    auto writer = Writer::New();
    writer->SetFileName(filePath.toStdString());
    writer->SetInput(image);
    writer->Update();
}

}

class StrokeOverlay : public QWidget {
public:
    explicit StrokeOverlay(QWidget *parent = nullptr)
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

    QPolygonF offsetStroke(double distancePixels) const {
        QPolygonF points;
        points.reserve(static_cast<int>(m_points.size()));
        for (const QPointF &point : m_points) {
            points << point;
        }
        return offsetPolyline(points, distancePixels);
    }

    void setSeedDistancePixels(double distancePixels) {
        if (std::abs(m_seedDistancePixels - distancePixels) <= 1e-9) {
            return;
        }
        m_seedDistancePixels = distancePixels;
        update();
    }

    std::function<void()> onStrokeChanged;
    std::function<void()> onStrokeFinished;

protected:
    void paintEvent(QPaintEvent *event) override {
        QWidget::paintEvent(event);
        if (m_points.empty()) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor("#ffb05c"));
        pen.setWidthF(m_seedDistancePixels > 0.0 ? 3.0 : 6.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        QPolygonF polyline;
        for (const QPointF &point : m_points) {
            polyline << point;
        }
        painter.drawPolyline(polyline);

        if (m_seedDistancePixels <= 0.0 || m_points.size() < 2) {
            return;
        }
        pen.setWidthF(4.0);
        pen.setColor(QColor("#f2483d"));
        painter.setPen(pen);
        painter.drawPolyline(offsetStroke(-m_seedDistancePixels));
        pen.setColor(QColor("#26a6ff"));
        painter.setPen(pen);
        painter.drawPolyline(offsetStroke(m_seedDistancePixels));
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
            const bool finishedStroke = m_dragging && hasValidStroke();
            m_dragging = false;
            if (onStrokeChanged) {
                onStrokeChanged();
            }
            if (finishedStroke && onStrokeFinished) {
                onStrokeFinished();
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
    double m_seedDistancePixels = 0.0;
    std::vector<QPointF> m_points;
};

class ContentWidthDoubleSpinBox final : public QDoubleSpinBox {
public:
    explicit ContentWidthDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        connect(this, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this]() { updateGeometry(); });
    }

    QSize sizeHint() const override {
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        const QSize textSize = fontMetrics().size(Qt::TextSingleLine, text());
        return style()->sizeFromContents(QStyle::CT_SpinBox, &option, textSize, this);
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }
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

quint32 randomCycleColor(dataType::SegmentIdType labelId, quint32 seed) {
    std::uint64_t mixed = static_cast<std::uint64_t>(labelId)
                          ^ (static_cast<std::uint64_t>(seed) << 32U);
    mixed += 0x9e3779b97f4a7c15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31U;

    const int hue = static_cast<int>(mixed % 360U);
    const int saturation = 180 + static_cast<int>((mixed >> 9U) % 60U);
    const int value = 210 + static_cast<int>((mixed >> 17U) % 46U);
    return QColor::fromHsv(hue, saturation, value).rgb();
}

void setActorColor(vtkActor *actor, quint32 color) {
    if (actor == nullptr) {
        return;
    }
    actor->GetProperty()->SetColor(
        qRed(color) / 255.0,
        qGreen(color) / 255.0,
        qBlue(color) / 255.0);
}

double elapsedMilliseconds(qint64 nanoseconds) {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

QLabel *createHelpBadgeLabel(const QString &tooltipText, QWidget *parent) {
    if (tooltipText.isEmpty()) {
        return nullptr;
    }

    auto *helpLabel = new QLabel("?", parent);
    helpLabel->setObjectName(QStringLiteral("threeDViewHelpBadge"));
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

QString threeDViewHelpText(bool showExplodeControls,
                           bool showCutControls,
                           bool showSeededSplitControls) {
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
    if (showSeededSplitControls) {
        lines << QStringLiteral("Press 1 or 2 before a click to add a red or blue seed. Split Line (L) previews seeds on both sides of a stroke.");
        lines << QStringLiteral("Adjust Line Offset and Line Sampling, then use Confirm Seeds. Auto Preview runs once both confirmed seed classes contain a marker.");
        lines << QStringLiteral("Connect Seeds joins markers of each color through the segment interior before watershed.");
        lines << QStringLiteral("Compact Watershed adds seed-distance regularization. Allow disconnected parts accepts such results but keeps a warning visible.");
        lines << QStringLiteral("Distance-map Smoothing controls Gaussian smoothing in pixels.");
        lines << QStringLiteral("Press E to export the latest split input and result for debugging.");
    }
    if (showExplodeControls) {
        lines << QStringLiteral("Move the Explode slider to add up to %1% of each segment's original distance from the scene center; 0% shows the unshifted view and 100% doubles that distance.")
                     .arg(kMaximumExplodePercent);
        lines << QStringLiteral("Use the left/right arrow keys to step the explode slider.");
    }
    if (!showSeededSplitControls) {
        lines << QStringLiteral("Press R to cycle the segment colors in this 3D view.");
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
    std::unordered_map<dataType::SegmentIdType, BoundsScanResult> boundsByLabel;
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
        std::unordered_map<dataType::SegmentIdType, BoundsScanResult> boundsByLabel;
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
                    includeInBounds(local.bounds, x, y, z);
                    const auto [labelBounds, inserted] = local.boundsByLabel.try_emplace(
                        labelId, BoundsScanResult{x, x, y, y, z, z});
                    if (!inserted) {
                        includeInBounds(labelBounds->second, x, y, z);
                    }
                }
            }
        }
    }

    AllLabelsScanResult result;
    result.bounds = emptyBounds;
    for (const auto &local : localResults) {
        if (local.bounds.maxX >= 0) {
            includeInBounds(result.bounds, local.bounds.minX, local.bounds.minY, local.bounds.minZ);
            includeInBounds(result.bounds, local.bounds.maxX, local.bounds.maxY, local.bounds.maxZ);
        }
        for (const auto &[labelId, localBounds] : local.boundsByLabel) {
            const auto [labelBounds, inserted] = result.boundsByLabel.try_emplace(
                labelId, localBounds);
            if (!inserted) {
                includeInBounds(labelBounds->second,
                                localBounds.minX,
                                localBounds.minY,
                                localBounds.minZ);
                includeInBounds(labelBounds->second,
                                localBounds.maxX,
                                localBounds.maxY,
                                localBounds.maxZ);
            }
        }
    }

    result.labels.reserve(result.boundsByLabel.size());
    for (const auto &[labelId, bounds] : result.boundsByLabel) {
        result.labels.push_back(labelId);
    }
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

Roi roiFromBounds(const BoundsScanResult &bounds) {
    Roi roi;
    roi.minX = bounds.minX;
    roi.maxX = bounds.maxX;
    roi.minY = bounds.minY;
    roi.maxY = bounds.maxY;
    roi.minZ = bounds.minZ;
    roi.maxZ = bounds.maxZ;
    return roi;
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
    setActorColor(actor, mesh.lutColor);
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

Segment3DViewerDialog::PreparedScene
Segment3DViewerDialog::prepareSingleLabelSlideshowScene(
    dataType::SegmentsImageType::Pointer segImage,
    LabelWithColor label)
{
    if (segImage == nullptr || label.first == 0) {
        return {};
    }

    const auto &size = segImage->GetLargestPossibleRegion().GetSize();
    const std::vector<LabelWithColor> requestedLabels{label};
    const auto scanStartedAt = QDateTime::currentMSecsSinceEpoch();
    auto scan = scanAllLabelsAndBounds(
        segImage->GetBufferPointer(),
        static_cast<int>(size[0]),
        static_cast<int>(size[1]),
        static_cast<int>(size[2]));
    SP_LOG_3D_TIMER(
        scanStartedAt,
        QStringLiteral("[3DView] [segmentpuzzler] slideshow label and bbox scan"));

    PreparedScene preparedScene;
    const auto requestedBounds = scan.boundsByLabel.find(label.first);
    if (requestedBounds != scan.boundsByLabel.end()) {
        const Roi bounds = roiFromBounds(requestedBounds->second);
        preparedScene = prepareScene(segImage, requestedLabels, bounds, false);
    } else {
        preparedScene.targetLabelId = label.first;
        preparedScene.windowTitle =
            QString("Segment %1 (press q to quit)").arg(label.first);
    }

    preparedScene.navigationCatalogComplete = true;
    preparedScene.navigationLabels = std::move(scan.labels);
    for (const auto &[labelId, labelBounds] : scan.boundsByLabel) {
        preparedScene.navigationBounds.emplace(labelId, roiFromBounds(labelBounds));
    }
    return preparedScene;
}

Segment3DViewerDialog::PreparedScene
Segment3DViewerDialog::prepareSingleLabelSlideshowScene(
    dataType::SegmentsImageType::Pointer segImage,
    LabelWithColor label,
    const Roi &cachedBounds)
{
    auto preparedScene = prepareScene(segImage, {label}, cachedBounds);
    if (!preparedScene.meshes.empty()) {
        return preparedScene;
    }

    SP_LOG_INFO(
        "viewer.three_d",
        QStringLiteral(
            "operation=3d_slideshow phase=cache_rebuild reason=empty_surface "
            "target_label=%1")
            .arg(label.first));
    return prepareSingleLabelSlideshowScene(segImage, label);
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
    : Segment3DViewerDialog(std::move(preparedScene),
                            CutSessionConfig{},
                            SeededSplitSessionConfig{},
                            parent,
                            launchSliceAxis)
{
}

Segment3DViewerDialog::Segment3DViewerDialog(PreparedScene preparedScene,
                                             CutSessionConfig cutSession,
                                             QWidget *parent,
                                             int launchSliceAxis)
    : Segment3DViewerDialog(std::move(preparedScene),
                            std::move(cutSession),
                            SeededSplitSessionConfig{},
                            parent,
                            launchSliceAxis)
{
}

Segment3DViewerDialog::Segment3DViewerDialog(
    PreparedScene preparedScene,
    SeededSplitSessionConfig seededSplitSession,
    QWidget *parent,
    int launchSliceAxis)
    : Segment3DViewerDialog(std::move(preparedScene),
                            CutSessionConfig{},
                            std::move(seededSplitSession),
                            parent,
                            launchSliceAxis)
{
}

Segment3DViewerDialog::Segment3DViewerDialog(
    PreparedScene preparedScene,
    CutSessionConfig cutSession,
    SeededSplitSessionConfig seededSplitSession,
    QWidget *parent,
    int launchSliceAxis)
    : QDialog(parent)
    , m_targetLabelId(preparedScene.targetLabelId)
    , m_cutSession(std::move(cutSession))
    , m_seededSplitSession(std::move(seededSplitSession))
    , m_originalSeededSplitScene(
          m_seededSplitSession.session != nullptr ? preparedScene : PreparedScene{})
    , m_navigationBounds(std::move(preparedScene.navigationBounds))
    , m_launchSliceAxis(launchSliceAxis)
{
    setWindowTitle(preparedScene.windowTitle);
    resize(600, 600);
    qApp->installEventFilter(this);

    if (preparedScene.targetLabelId != 0 && preparedScene.meshes.size() == 1) {
        PreparedScene cachedScene = preparedScene;
        cachedScene.navigationLabels.clear();
        cachedScene.navigationBounds.clear();
        cachedScene.navigationCatalogComplete = false;
        m_preparedSceneCache.emplace(preparedScene.targetLabelId, std::move(cachedScene));
    }

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
    const bool showSeededSplitControls =
        m_seededSplitSession.session != nullptr
        && static_cast<bool>(m_seededSplitSession.applySplit)
        && m_targetLabelId != 0;
    const bool showExplodeControls = m_segmentActors.size() > 1 && !showSeededSplitControls;
    const bool showCutControls = static_cast<bool>(m_cutSession.applyCut) && m_targetLabelId != 0;
    if (showSeededSplitControls) {
        renWin->SetNumberOfLayers(2);
        m_seedRenderer = vtkSmartPointer<vtkRenderer>::New();
        m_seedRenderer->SetLayer(1);
        m_seedRenderer->PreserveDepthBufferOff();
        m_seedRenderer->SetActiveCamera(m_renderer->GetActiveCamera());
        m_seedRenderer->InteractiveOff();
        renWin->AddRenderer(m_seedRenderer);
    }

    m_vtkWidget = new QVTKOpenGLNativeWidget(this);
    m_vtkWidget->setRenderWindow(renWin);
    m_vtkWidget->setEnableTouchEventProcessing(false);
    m_vtkWidget->setMouseTracking(false);
    m_vtkWidget->setAttribute(Qt::WA_Hover, false);
    QSizePolicy vtkSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    vtkSizePolicy.setRetainSizeWhenHidden(true);
    m_vtkWidget->setSizePolicy(vtkSizePolicy);
    m_vtkWidget->setFocusPolicy(Qt::StrongFocus);
    m_vtkWidget->hide();

    auto style = vtkSmartPointer<SurfaceOnlyTrackballCameraStyle>::New();
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
    if (showCutControls || showSeededSplitControls) {
        m_strokeOverlay = new StrokeOverlay(vtkContainer);
        m_strokeOverlay->onStrokeChanged = [this]() {
            updateCutUiState();
            updateSeededSplitUiState();
        };
        if (showSeededSplitControls) {
            m_strokeOverlay->onStrokeFinished =
                [this]() { updateSplitLineSeedPreview(); };
        }
        vtkStackLayout->addWidget(m_strokeOverlay, 0, 0);
    }
    layout->addWidget(vtkContainer, 1);

    if (showExplodeControls || showCutControls || showSeededSplitControls) {
        m_controlsRow = ensureControlsRow();

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
            m_controlsRow->addWidget(new QLabel(QStringLiteral("Explode:"), m_controlsWidget));
            m_controlsRow->addWidget(m_explodeSlider, 1);
            m_controlsRow->addSpacing(8);
            addOrbitControls(m_controlsRow);
        }

        if (showCutControls) {
            if (showExplodeControls) {
                m_controlsRow->addSpacing(8);
            }
            m_drawCutButton = new QPushButton(QStringLiteral("Draw Cut"), m_controlsWidget);
            m_clearCutButton = new QPushButton(QStringLiteral("Clear"), m_controlsWidget);
            m_applyCutButton = new QPushButton(QStringLiteral("Apply"), m_controlsWidget);
            connect(m_drawCutButton, &QPushButton::clicked, this, &Segment3DViewerDialog::beginCutDrawing);
            connect(m_clearCutButton, &QPushButton::clicked, this, &Segment3DViewerDialog::clearCutStroke);
            connect(m_applyCutButton, &QPushButton::clicked, this, &Segment3DViewerDialog::applyProjectedCut);
            m_controlsRow->addWidget(m_drawCutButton);
            m_controlsRow->addWidget(m_clearCutButton);
            m_controlsRow->addWidget(m_applyCutButton);
        }

        if (showSeededSplitControls) {
            m_seedButtons[0] = new QPushButton(QStringLiteral("Red Seed"), m_controlsWidget);
            m_seedButtons[1] = new QPushButton(QStringLiteral("Blue Seed"), m_controlsWidget);
            m_seedButtons[0]->setCheckable(true);
            m_seedButtons[1]->setCheckable(true);
            m_seedButtons[0]->setToolTip(
                QStringLiteral("Add a red seed (1)."));
            m_seedButtons[1]->setToolTip(
                QStringLiteral("Add a blue seed (2)."));
            m_splitLineButton = new QPushButton(QStringLiteral("Redraw Line"), m_controlsWidget);
            m_splitLineButton->setCheckable(true);
            m_splitLineButton->setMinimumWidth(m_splitLineButton->sizeHint().width());
            m_splitLineButton->setText(QStringLiteral("Split Line"));
            m_splitLineButton->setToolTip(
                QStringLiteral("Draw a line that generates red and blue seeds on either side (L)."));
            m_seedDistanceSlider = new QSlider(Qt::Horizontal, m_controlsWidget);
            m_seedDistanceSlider->setRange(2, kMaximumSeedDistancePixels);
            m_seedDistanceSlider->setValue(kDefaultSeedDistancePixels);
            m_seedDistanceSlider->setSingleStep(1);
            m_seedDistanceSlider->setPageStep(5);
            m_seedDistanceSlider->setFixedWidth(90);
            m_seedDistanceSlider->setToolTip(
                QStringLiteral("Screen-space offset between the split line and generated seeds."));
            m_seedDistanceLabel = new QLabel(
                QStringLiteral("Line Offset: %1 px").arg(kDefaultSeedDistancePixels),
                m_controlsWidget);
            m_lineSamplingSlider = new QSlider(Qt::Horizontal, m_controlsWidget);
            m_lineSamplingSlider->setRange(
                kMinimumLineSamplingPixels, kMaximumLineSamplingPixels);
            m_lineSamplingSlider->setValue(kDefaultLineSamplingPixels);
            m_lineSamplingSlider->setSingleStep(1);
            m_lineSamplingSlider->setPageStep(5);
            m_lineSamplingSlider->setFixedWidth(90);
            m_lineSamplingSlider->setToolTip(
                QStringLiteral(
                    "Screen-space distance between generated seed pairs. "
                    "Smaller values create more seeds."));
            m_lineSamplingLabel = new QLabel(
                QStringLiteral("Line Sampling: %1 px")
                    .arg(kDefaultLineSamplingPixels),
                m_controlsWidget);
            m_confirmLineSeedsButton = new QPushButton(
                QStringLiteral("Confirm Seeds"), m_controlsWidget);
            m_confirmLineSeedsButton->setToolTip(
                QStringLiteral(
                    "Use the currently displayed line seeds for the watershed."));
            m_clearSeedsButton = new QPushButton(QStringLiteral("Reset"), m_controlsWidget);
            m_previewSplitButton = new QPushButton(QStringLiteral("Preview"), m_controlsWidget);
            m_applySplitButton = new QPushButton(QStringLiteral("Apply Split"), m_controlsWidget);
            m_applySplitButton->setDefault(true);
            m_seedStatusLabel = new QLabel(
                QStringLiteral("Add at least one seed to each class."), m_controlsWidget);
            m_seedStatusLabel->setSizePolicy(
                QSizePolicy::Ignored, QSizePolicy::Preferred);
            auto *optionsWidget = new QWidget(this);
            m_autoPreviewCheckBox = new QCheckBox(
                QStringLiteral("Auto Preview"), optionsWidget);
            m_autoPreviewCheckBox->setObjectName(QStringLiteral("autoPreviewCheckBox"));
            m_autoPreviewCheckBox->setChecked(true);
            m_autoPreviewCheckBox->setToolTip(
                QStringLiteral("Automatically update the preview once both seed classes are present."));
            m_connectSeedsCheckBox = new QCheckBox(
                QStringLiteral("Connect Seeds"), optionsWidget);
            m_connectSeedsCheckBox->setObjectName(QStringLiteral("connectSeedsCheckBox"));
            m_connectSeedsCheckBox->setChecked(true);
            m_connectSeedsCheckBox->setToolTip(
                QStringLiteral(
                    "Connect seeds of each color through the segment interior before "
                    "running the watershed."));
            m_compactWatershedCheckBox = new QCheckBox(
                QStringLiteral("Compact Watershed"), optionsWidget);
            m_compactWatershedCheckBox->setObjectName(
                QStringLiteral("compactWatershedCheckBox"));
            m_compactWatershedCheckBox->setChecked(true);
            m_compactWatershedCheckBox->setToolTip(
                QStringLiteral(
                    "Regularize the watershed by distance to the seeds while retaining "
                    "the distance-map landscape."));
            m_allowDisconnectedCheckBox = new QCheckBox(
                QStringLiteral("Allow disconnected parts"), optionsWidget);
            m_allowDisconnectedCheckBox->setObjectName(
                QStringLiteral("allowDisconnectedCheckBox"));
            m_allowDisconnectedCheckBox->setChecked(false);
            m_allowDisconnectedCheckBox->setToolTip(
                QStringLiteral(
                    "Allow applying a result whose red or blue part contains multiple "
                    "disconnected regions. A warning remains visible in the preview."));
            m_smoothingSlider = new QSlider(Qt::Horizontal, optionsWidget);
            m_smoothingSlider->setObjectName(QStringLiteral("smoothingSlider"));
            m_smoothingSlider->setRange(0, kMaximumSmoothingSliderValue);
            m_smoothingSlider->setSingleStep(1);
            m_smoothingSlider->setPageStep(5);
            m_smoothingSlider->setFixedWidth(120);
            m_smoothingSlider->setMinimumHeight(28);
            m_smoothingSlider->setValue(static_cast<int>(std::lround(
                m_seededSplitSession.session->landscapeSmoothingSigmaPixels
                * kSmoothingSliderStepsPerPixel)));
            m_smoothingSlider->setToolTip(
                QStringLiteral("Gaussian smoothing sigma for the watershed distance map (0.0–3.0 px)."));
            m_smoothingLabel = new QLabel(
                QStringLiteral("Distance-map Smoothing: %1 px")
                    .arg(m_seededSplitSession.session->landscapeSmoothingSigmaPixels,
                         0, 'f', 1),
                optionsWidget);
            m_smoothingUpdateTimer = new QTimer(this);
            m_smoothingUpdateTimer->setSingleShot(true);
            m_smoothingUpdateTimer->setInterval(150);
            connect(m_seedButtons[0], &QPushButton::clicked,
                    this, [this]() { armSeedPlacement(0); });
            connect(m_seedButtons[1], &QPushButton::clicked,
                    this, [this]() { armSeedPlacement(1); });
            connect(m_splitLineButton, &QPushButton::clicked,
                    this, &Segment3DViewerDialog::beginSplitLineDrawing);
            connect(m_confirmLineSeedsButton, &QPushButton::clicked,
                    this, &Segment3DViewerDialog::confirmSplitLineSeeds);
            connect(m_seedDistanceSlider, &QSlider::valueChanged, this, [this](int value) {
                m_seedDistanceLabel->setText(QStringLiteral("Line Offset: %1 px").arg(value));
                if (!m_splitLineDrawModeActive || m_strokeOverlay == nullptr) {
                    return;
                }
                m_strokeOverlay->setSeedDistancePixels(value);
                if (m_havePendingLineSeeds) {
                    updateSplitLineSeedPreview();
                }
            });
            connect(m_lineSamplingSlider, &QSlider::valueChanged, this, [this](int value) {
                m_lineSamplingLabel->setText(
                    QStringLiteral("Line Sampling: %1 px").arg(value));
                if (m_havePendingLineSeeds) {
                    updateSplitLineSeedPreview();
                }
            });
            connect(m_autoPreviewCheckBox, &QCheckBox::toggled,
                    this, [this](bool enabled) {
                        if (enabled) {
                            autoPreviewSeededSplitIfReady();
                        }
                    });
            const auto watershedOptionChanged = [this]() {
                m_lastSeededSplitResult.reset();
                if (m_seededSplitPreview.has_value()) {
                    replaceSegmentMeshes(m_originalSeededSplitScene, false);
                    m_seededSplitPreview.reset();
                }
                setSeededSplitStatus(
                    QStringLiteral("Watershed options changed; press Preview."));
                updateSeededSplitUiState();
                autoPreviewSeededSplitIfReady();
            };
            connect(m_compactWatershedCheckBox, &QCheckBox::toggled,
                    this, watershedOptionChanged);
            connect(m_connectSeedsCheckBox, &QCheckBox::toggled,
                    this, watershedOptionChanged);
            connect(m_allowDisconnectedCheckBox, &QCheckBox::toggled,
                    this, watershedOptionChanged);
            connect(m_smoothingSlider, &QSlider::valueChanged, this, [this](int value) {
                const double sigmaPixels =
                    static_cast<double>(value) / kSmoothingSliderStepsPerPixel;
                m_smoothingLabel->setText(
                    QStringLiteral("Distance-map Smoothing: %1 px")
                        .arg(sigmaPixels, 0, 'f', 1));
                if (std::abs(
                        sigmaPixels
                        - m_seededSplitSession.session->landscapeSmoothingSigmaPixels)
                    <= 1e-9) {
                    return;
                }
                m_seededSplitSmoothingPending = true;
                m_lastSeededSplitResult.reset();
                if (m_seededSplitPreview.has_value()) {
                    replaceSegmentMeshes(m_originalSeededSplitScene, false);
                    m_seededSplitPreview.reset();
                }
                setSeededSplitStatus(
                    QStringLiteral("Updating distance-map smoothing..."));
                m_smoothingUpdateTimer->start();
                updateSeededSplitUiState();
            });
            connect(m_smoothingUpdateTimer, &QTimer::timeout,
                    this, &Segment3DViewerDialog::updateSeededSplitSmoothing);
            connect(m_clearSeedsButton, &QPushButton::clicked,
                    this, &Segment3DViewerDialog::clearSeededSplit);
            connect(m_previewSplitButton, &QPushButton::clicked,
                    this, &Segment3DViewerDialog::previewSeededSplit);
            connect(m_applySplitButton, &QPushButton::clicked,
                    this, &Segment3DViewerDialog::applySeededSplit);
            m_controlsRow->addWidget(m_seedButtons[0]);
            m_controlsRow->addWidget(m_seedButtons[1]);
            m_controlsRow->addWidget(m_splitLineButton);
            m_controlsRow->addWidget(m_seedDistanceLabel);
            m_controlsRow->addWidget(m_seedDistanceSlider);
            m_controlsRow->addWidget(m_lineSamplingLabel);
            m_controlsRow->addWidget(m_lineSamplingSlider);
            m_controlsRow->addWidget(m_confirmLineSeedsButton);
            m_controlsRow->addWidget(m_clearSeedsButton);
            m_controlsRow->addWidget(m_previewSplitButton);
            m_controlsRow->addWidget(m_applySplitButton);
            m_controlsRow->addWidget(m_seedStatusLabel, 1);

            auto *optionsRow = new QHBoxLayout(optionsWidget);
            optionsRow->setContentsMargins(10, 0, 10, 6);
            optionsRow->setSpacing(8);
            optionsRow->addWidget(m_autoPreviewCheckBox);
            optionsRow->addWidget(m_connectSeedsCheckBox);
            optionsRow->addWidget(m_compactWatershedCheckBox);
            optionsRow->addWidget(m_allowDisconnectedCheckBox);
            optionsRow->addWidget(m_smoothingLabel);
            optionsRow->addWidget(m_smoothingSlider);
            optionsRow->addStretch(1);
            layout->addWidget(optionsWidget, 0);
        }

        if (!showExplodeControls) {
            m_controlsRow->addStretch(1);
        }

        if (QLabel *helpLabel = createHelpBadgeLabel(
                threeDViewHelpText(showExplodeControls,
                                   showCutControls,
                                   showSeededSplitControls),
                m_controlsWidget)) {
            m_controlsRow->addWidget(helpLabel);
        }
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

    if (!showSeededSplitControls) {
        auto *colorCycleShortcut = new QShortcut(QKeySequence(Qt::Key_R), this);
        colorCycleShortcut->setContext(Qt::WindowShortcut);
        connect(colorCycleShortcut, &QShortcut::activated,
                this, &Segment3DViewerDialog::cycleSegmentColors);
    }

    if (showCutControls) {
        auto *cutHelpShortcut = new QShortcut(QKeySequence(Qt::Key_Question), this);
        cutHelpShortcut->setContext(Qt::WindowShortcut);
        connect(cutHelpShortcut, &QShortcut::activated, this, &Segment3DViewerDialog::showCutHelp);

        auto *cutHelpF1Shortcut = new QShortcut(QKeySequence(Qt::Key_F1), this);
        cutHelpF1Shortcut->setContext(Qt::WindowShortcut);
        connect(cutHelpF1Shortcut, &QShortcut::activated, this, &Segment3DViewerDialog::showCutHelp);
    }

    if (showSeededSplitControls) {
        auto *seed1Shortcut = new QShortcut(QKeySequence(Qt::Key_1), this);
        seed1Shortcut->setContext(Qt::WindowShortcut);
        connect(seed1Shortcut, &QShortcut::activated,
                this, [this]() { armSeedPlacement(0); });

        auto *seed2Shortcut = new QShortcut(QKeySequence(Qt::Key_2), this);
        seed2Shortcut->setContext(Qt::WindowShortcut);
        connect(seed2Shortcut, &QShortcut::activated,
                this, [this]() { armSeedPlacement(1); });

        auto *splitLineShortcut = new QShortcut(QKeySequence(Qt::Key_L), this);
        splitLineShortcut->setContext(Qt::WindowShortcut);
        connect(splitLineShortcut, &QShortcut::activated,
                this, &Segment3DViewerDialog::beginSplitLineDrawing);

        auto *exportSplitShortcut = new QShortcut(QKeySequence(Qt::Key_E), this);
        exportSplitShortcut->setContext(Qt::WindowShortcut);
        connect(exportSplitShortcut, &QShortcut::activated,
                this, &Segment3DViewerDialog::exportSeededSplitDebugBundle);
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
    updateSeededSplitUiState();
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

void Segment3DViewerDialog::setDeleteLabelHandler(DeleteLabelHandler handler) {
    m_deleteLabelHandler = std::move(handler);
}

QHBoxLayout *Segment3DViewerDialog::ensureControlsRow() {
    if (m_controlsRow != nullptr) {
        return m_controlsRow;
    }

    auto *mainLayout = qobject_cast<QVBoxLayout *>(layout());
    if (mainLayout == nullptr) {
        return nullptr;
    }

    m_controlsWidget = new QWidget(this);
    m_controlsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_controlsRow = new QHBoxLayout(m_controlsWidget);
    m_controlsRow->setContentsMargins(10, 6, 10, 10);
    m_controlsRow->setSpacing(8);
    mainLayout->addWidget(m_controlsWidget, 0);
    return m_controlsRow;
}

void Segment3DViewerDialog::addOrbitControls(QHBoxLayout *controlsRow) {
    if (controlsRow == nullptr || m_orbitCheckBox != nullptr) {
        return;
    }

    m_orbitCheckBox = new QCheckBox(QStringLiteral("Orbit"), m_controlsWidget);
    m_orbitCheckBox->setObjectName(QStringLiteral("orbitCheckBox"));
    m_orbitCheckBox->setToolTip(QStringLiteral("Rotate the current 3D view"));
    m_orbitSpeedSpinBox = new ContentWidthDoubleSpinBox(m_controlsWidget);
    m_orbitSpeedSpinBox->setObjectName(QStringLiteral("orbitSpeedSpinBox"));
    m_orbitSpeedSpinBox->setRange(0.1, 360.0);
    m_orbitSpeedSpinBox->setSingleStep(5.0);
    m_orbitSpeedSpinBox->setDecimals(1);
    m_orbitSpeedSpinBox->setSuffix(QStringLiteral(" °/s"));
    m_orbitSpeedSpinBox->setValue(40.0);
    m_orbitSpeedSpinBox->setToolTip(QStringLiteral("Orbit speed in degrees per second"));

    controlsRow->addWidget(m_orbitCheckBox);
    controlsRow->addWidget(m_orbitSpeedSpinBox);
    connect(m_orbitCheckBox, &QCheckBox::toggled,
            this, &Segment3DViewerDialog::setOrbitEnabled);
}

void Segment3DViewerDialog::setSingleLabelSession(SingleLabelSessionConfig session)
{
    if (m_targetLabelId == 0 || m_segmentActors.size() != 1) {
        return;
    }

    session.labels.erase(
        std::remove(session.labels.begin(), session.labels.end(), 0),
        session.labels.end());
    std::sort(session.labels.begin(), session.labels.end());
    session.labels.erase(
        std::unique(session.labels.begin(), session.labels.end()),
        session.labels.end());
    m_navigationLabels = std::move(session.labels);
    m_requestLabelHandler = std::move(session.requestLabel);
    m_labelActivatedHandler = std::move(session.labelActivated);
    setDeleteLabelHandler(std::move(session.deleteLabel));

    QHBoxLayout *controlsRow = ensureControlsRow();
    if (controlsRow == nullptr) {
        return;
    }

    if (m_previousLabelButton == nullptr) {
        m_previousLabelButton = new QPushButton(QStringLiteral("Previous"), m_controlsWidget);
        m_previousLabelButton->setObjectName(QStringLiteral("previousLabelButton"));
        m_previousLabelButton->setToolTip(QStringLiteral("Show the previous segment ID (Left Arrow)"));
        m_navigationLabel = new QLabel(m_controlsWidget);
        m_navigationLabel->setObjectName(QStringLiteral("singleLabelNavigationLabel"));
        m_navigationLabel->setAlignment(Qt::AlignCenter);
        m_nextLabelButton = new QPushButton(QStringLiteral("Next"), m_controlsWidget);
        m_nextLabelButton->setObjectName(QStringLiteral("nextLabelButton"));
        m_nextLabelButton->setToolTip(QStringLiteral("Show the next segment ID (Right Arrow)"));

        controlsRow->addWidget(m_previousLabelButton);
        controlsRow->addStretch(1);
        controlsRow->addWidget(m_navigationLabel);
        controlsRow->addSpacing(8);
        addOrbitControls(controlsRow);
        controlsRow->addStretch(1);
        controlsRow->addWidget(m_nextLabelButton);

        connect(m_previousLabelButton, &QPushButton::clicked,
                this, [this]() { requestAdjacentLabel(-1); });
        connect(m_nextLabelButton, &QPushButton::clicked,
                this, [this]() { requestAdjacentLabel(1); });

        auto *previousShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
        previousShortcut->setContext(Qt::WindowShortcut);
        connect(previousShortcut, &QShortcut::activated,
                this, [this]() { requestAdjacentLabel(-1); });

        auto *nextShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
        nextShortcut->setContext(Qt::WindowShortcut);
        connect(nextShortcut, &QShortcut::activated,
                this, [this]() { requestAdjacentLabel(1); });
    }

    m_singleLabelNavigationBusy = false;
    updateSingleLabelNavigationUiState();
    QTimer::singleShot(0, this, [this]() { prefetchAdjacentLabel(); });
}

void Segment3DViewerDialog::setOrbitEnabled(bool enabled) {
    if (!enabled) {
        if (m_orbitTimer != nullptr) {
            m_orbitTimer->stop();
        }
        return;
    }

    if (m_orbitTimer == nullptr) {
        m_orbitTimer = new QTimer(this);
        m_orbitTimer->setInterval(50);
        m_orbitTimer->setTimerType(Qt::PreciseTimer);
        connect(m_orbitTimer, &QTimer::timeout,
                this, &Segment3DViewerDialog::advanceOrbit);
    }
    m_orbitTimer->start();
}

void Segment3DViewerDialog::advanceOrbit() {
    if (!isVisible() || isMinimized() || !isActiveWindow() || m_renderer == nullptr
        || m_vtkWidget == nullptr || m_vtkWidget->renderWindow() == nullptr
        || m_orbitSpeedSpinBox == nullptr || m_orbitTimer == nullptr) {
        return;
    }

    vtkCamera *camera = m_renderer->GetActiveCamera();
    if (camera == nullptr) {
        return;
    }
    const double degreesPerTick =
        m_orbitSpeedSpinBox->value() * m_orbitTimer->interval() / 1000.0;
    camera->Azimuth(degreesPerTick);
    camera->OrthogonalizeViewUp();
    m_renderer->ResetCameraClippingRange();
    m_vtkWidget->renderWindow()->Render();
}

void Segment3DViewerDialog::requestAdjacentLabel(int direction) {
    if (m_singleLabelNavigationBusy || !m_requestLabelHandler || direction == 0) {
        return;
    }

    const auto current = std::lower_bound(
        m_navigationLabels.begin(), m_navigationLabels.end(), m_targetLabelId);
    if (current == m_navigationLabels.end() || *current != m_targetLabelId) {
        return;
    }

    auto requested = current;
    if (direction < 0) {
        if (current == m_navigationLabels.begin()) {
            return;
        }
        --requested;
    } else {
        ++requested;
        if (requested == m_navigationLabels.end()) {
            return;
        }
    }

    activateOrRequestLabel(*requested);
}

void Segment3DViewerDialog::activateOrRequestLabel(dataType::SegmentIdType labelId) {
    const auto cached = m_preparedSceneCache.find(labelId);
    if (cached != m_preparedSceneCache.end()) {
        if (applyPreparedScene(cached->second)) {
            SP_LOG_DEBUG(
                "viewer.three_d",
                QStringLiteral("[3DView] activated cached segment label=%1").arg(labelId));
            if (m_labelActivatedHandler) {
                m_labelActivatedHandler(labelId);
            }
            prunePreparedSceneCache();
            QTimer::singleShot(0, this, [this]() { prefetchAdjacentLabel(); });
        }
        return;
    }

    m_activateWhenReadyLabel = labelId;
    m_unavailableSceneLabels.erase(labelId);
    setSingleLabelNavigationBusy(true, labelId);
    if (m_pendingSceneLabel != 0) {
        return;
    }

    const auto bounds = m_navigationBounds.find(labelId);
    m_pendingSceneLabel = labelId;
    m_requestLabelHandler(
        labelId,
        bounds == m_navigationBounds.end() ? Roi{} : bounds->second);
}

void Segment3DViewerDialog::prefetchAdjacentLabel() {
    if (m_pendingSceneLabel != 0 || m_singleLabelNavigationBusy || !m_requestLabelHandler) {
        return;
    }

    const auto current = std::lower_bound(
        m_navigationLabels.begin(), m_navigationLabels.end(), m_targetLabelId);
    if (current == m_navigationLabels.end() || *current != m_targetLabelId) {
        return;
    }

    std::array<dataType::SegmentIdType, 2> candidates{0, 0};
    if (std::next(current) != m_navigationLabels.end()) {
        candidates[0] = *std::next(current);
    }
    if (current != m_navigationLabels.begin()) {
        candidates[1] = *std::prev(current);
    }

    for (const auto labelId : candidates) {
        if (labelId == 0
            || m_preparedSceneCache.find(labelId) != m_preparedSceneCache.end()
            || m_unavailableSceneLabels.find(labelId) != m_unavailableSceneLabels.end()) {
            continue;
        }
        const auto bounds = m_navigationBounds.find(labelId);
        m_pendingSceneLabel = labelId;
        SP_LOG_DEBUG(
            "viewer.three_d",
            QStringLiteral("[3DView] prefetching adjacent segment label=%1").arg(labelId));
        m_requestLabelHandler(
            labelId,
            bounds == m_navigationBounds.end() ? Roi{} : bounds->second);
        return;
    }
}

void Segment3DViewerDialog::prunePreparedSceneCache() {
    const auto current = std::lower_bound(
        m_navigationLabels.begin(), m_navigationLabels.end(), m_targetLabelId);
    if (current == m_navigationLabels.end() || *current != m_targetLabelId) {
        return;
    }

    std::set<dataType::SegmentIdType> retained{m_targetLabelId, m_pendingSceneLabel};
    if (current != m_navigationLabels.begin()) {
        retained.insert(*std::prev(current));
    }
    if (std::next(current) != m_navigationLabels.end()) {
        retained.insert(*std::next(current));
    }
    for (auto cached = m_preparedSceneCache.begin(); cached != m_preparedSceneCache.end();) {
        if (retained.find(cached->first) == retained.end()) {
            cached = m_preparedSceneCache.erase(cached);
        } else {
            ++cached;
        }
    }
}

bool Segment3DViewerDialog::removeLabelActor(dataType::SegmentIdType labelId) {
    const auto actorIt = std::find_if(
        m_segmentActors.begin(), m_segmentActors.end(),
        [labelId](const SegmentActorInfo &actorInfo) {
            return actorInfo.labelId == labelId;
        });
    if (actorIt == m_segmentActors.end() || m_renderer == nullptr) {
        return false;
    }

    m_renderer->RemoveActor(actorIt->actor);
    m_segmentActors.erase(actorIt);
    m_renderer->ResetCameraClippingRange();
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
    return true;
}

bool Segment3DViewerDialog::deleteCurrentLabel() {
    if (m_targetLabelId == 0 || m_segmentActors.size() != 1
        || !m_requestLabelHandler || !m_deleteLabelHandler) {
        return false;
    }
    // Consume D while a cached neighbor is being prepared. Falling through to
    // the legacy hold-D-and-click mode would mutate the image during that read.
    if (m_singleLabelNavigationBusy || m_pendingSceneLabel != 0) {
        return true;
    }

    const auto current = std::lower_bound(
        m_navigationLabels.begin(), m_navigationLabels.end(), m_targetLabelId);
    if (current == m_navigationLabels.end() || *current != m_targetLabelId) {
        return false;
    }

    const auto deletedLabel = m_targetLabelId;
    if (!m_deleteLabelHandler(deletedLabel)) {
        SP_LOG_WARNING(
            "viewer.three_d",
            QStringLiteral("[3DView] delete current label rejected label=%1")
                .arg(deletedLabel));
        return true;
    }

    removeLabelActor(deletedLabel);
    m_navigationBounds.erase(deletedLabel);
    m_preparedSceneCache.erase(deletedLabel);
    m_unavailableSceneLabels.erase(deletedLabel);
    auto next = m_navigationLabels.erase(current);
    if (m_navigationLabels.empty()) {
        close();
        return true;
    }
    if (next == m_navigationLabels.end()) {
        next = std::prev(m_navigationLabels.end());
    }

    const auto nextLabel = *next;
    SP_LOG_INFO(
        "viewer.three_d",
        QStringLiteral("[3DView] deleted current label=%1 nextLabel=%2 remaining=%3")
            .arg(deletedLabel)
            .arg(nextLabel)
            .arg(m_navigationLabels.size()));
    activateOrRequestLabel(nextLabel);
    return true;
}

void Segment3DViewerDialog::setSingleLabelNavigationBusy(
    bool busy,
    dataType::SegmentIdType pendingLabelId)
{
    m_singleLabelNavigationBusy = busy;
    if (busy && m_navigationLabel != nullptr) {
        m_navigationLabel->setText(
            pendingLabelId == 0
                ? QStringLiteral("Loading segment...")
                : QStringLiteral("Loading segment %1...").arg(pendingLabelId));
    }
    updateSingleLabelNavigationUiState();
}

void Segment3DViewerDialog::updateSingleLabelNavigationUiState() {
    if (m_previousLabelButton == nullptr || m_nextLabelButton == nullptr) {
        return;
    }

    const auto current = std::lower_bound(
        m_navigationLabels.begin(), m_navigationLabels.end(), m_targetLabelId);
    const bool foundCurrent = current != m_navigationLabels.end() && *current == m_targetLabelId;
    const bool canNavigate = foundCurrent && static_cast<bool>(m_requestLabelHandler)
                             && !m_singleLabelNavigationBusy;
    m_previousLabelButton->setEnabled(canNavigate && current != m_navigationLabels.begin());
    m_nextLabelButton->setEnabled(canNavigate
                                  && current != m_navigationLabels.end()
                                  && std::next(current) != m_navigationLabels.end());

    if (!m_singleLabelNavigationBusy && m_navigationLabel != nullptr) {
        if (foundCurrent) {
            const auto position = std::distance(m_navigationLabels.begin(), current) + 1;
            m_navigationLabel->setText(
                QStringLiteral("Segment %1 (%2 of %3)")
                    .arg(m_targetLabelId)
                    .arg(position)
                    .arg(m_navigationLabels.size()));
        } else {
            m_navigationLabel->setText(QStringLiteral("Segment %1").arg(m_targetLabelId));
        }
    }
}

void Segment3DViewerDialog::cachePreparedScene(PreparedScene preparedScene) {
    if (preparedScene.targetLabelId == 0 || preparedScene.meshes.size() != 1) {
        return;
    }
    const auto labelId = preparedScene.targetLabelId;
    preparedScene.navigationLabels.clear();
    preparedScene.navigationBounds.clear();
    preparedScene.navigationCatalogComplete = false;
    m_preparedSceneCache.insert_or_assign(labelId, std::move(preparedScene));
}

bool Segment3DViewerDialog::applyPreparedScene(const PreparedScene &preparedScene) {
    if (preparedScene.targetLabelId == 0 || preparedScene.meshes.size() != 1) {
        return false;
    }
    if (!replaceSegmentMeshes(preparedScene, true)) {
        return false;
    }

    m_targetLabelId = preparedScene.targetLabelId;
    setWindowTitle(preparedScene.windowTitle);
    m_singleLabelNavigationBusy = false;
    updateSingleLabelNavigationUiState();
    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DView] switched single-label scene targetLabel=%1")
                     .arg(m_targetLabelId));
    return true;
}

bool Segment3DViewerDialog::replaceSegmentMeshes(const PreparedScene &preparedScene,
                                                  bool resetCamera) {
    std::vector<SegmentActorInfo> newActors;
    newActors.reserve(preparedScene.meshes.size());
    for (const auto &mesh : preparedScene.meshes) {
        if (mesh.polyData == nullptr
            || mesh.polyData->GetNumberOfPoints() == 0
            || mesh.polyData->GetNumberOfCells() == 0) {
            continue;
        }
        auto actor = createMeshActor(mesh);
        newActors.push_back({actor, mesh.labelId, mesh.centerWorld});
    }

    if (newActors.empty() || m_renderer == nullptr) {
        return false;
    }
    applyColorCycle(newActors);

    for (const auto &actorInfo : m_segmentActors) {
        m_renderer->RemoveActor(actorInfo.actor);
    }
    for (const auto &actorInfo : newActors) {
        m_renderer->AddActor(actorInfo.actor);
    }

    m_segmentActors = std::move(newActors);
    m_sceneCenterWorld = preparedScene.sceneCenterWorld;
    m_sceneExtent = preparedScene.sceneExtent;

    if (resetCamera) {
        m_renderer->ResetCamera();
    }
    m_renderer->ResetCameraClippingRange();
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }

    return true;
}

bool Segment3DViewerDialog::acceptPreparedScene(PreparedScene preparedScene) {
    const auto preparedLabel = preparedScene.targetLabelId;
    if (preparedLabel == 0) {
        return false;
    }

    if (preparedScene.navigationCatalogComplete) {
        const bool targetFound = preparedScene.navigationBounds.find(preparedLabel)
                                 != preparedScene.navigationBounds.end();
        auto rebuiltLabels = std::move(preparedScene.navigationLabels);
        rebuiltLabels.erase(
            std::remove(rebuiltLabels.begin(), rebuiltLabels.end(), 0),
            rebuiltLabels.end());
        std::sort(rebuiltLabels.begin(), rebuiltLabels.end());
        rebuiltLabels.erase(
            std::unique(rebuiltLabels.begin(), rebuiltLabels.end()),
            rebuiltLabels.end());
        m_navigationLabels = std::move(rebuiltLabels);
        m_navigationBounds = std::move(preparedScene.navigationBounds);
        m_preparedSceneCache.clear();
        m_unavailableSceneLabels.clear();
        SP_LOG_INFO(
            "viewer.three_d",
            QStringLiteral(
                "operation=3d_slideshow phase=cache_rebuild status=applied "
                "target_label=%1 target_found=%2 labels=%3 bounds=%4")
                .arg(preparedLabel)
                .arg(targetFound)
                .arg(m_navigationLabels.size())
                .arg(m_navigationBounds.size()));
    }

    if (preparedScene.meshes.size() != 1) {
        updateSingleLabelNavigationUiState();
        return false;
    }

    cachePreparedScene(std::move(preparedScene));
    m_pendingSceneLabel = 0;

    const bool activatePreparedLabel = m_activateWhenReadyLabel == preparedLabel;
    if (activatePreparedLabel) {
        m_activateWhenReadyLabel = 0;
        const auto cached = m_preparedSceneCache.find(preparedLabel);
        if (cached == m_preparedSceneCache.end() || !applyPreparedScene(cached->second)) {
            setSingleLabelNavigationBusy(false);
            return false;
        }
        if (m_labelActivatedHandler) {
            m_labelActivatedHandler(preparedLabel);
        }
    }

    m_singleLabelNavigationBusy = m_activateWhenReadyLabel != 0;
    updateSingleLabelNavigationUiState();
    prunePreparedSceneCache();

    QTimer::singleShot(0, this, [this]() {
        if (m_activateWhenReadyLabel != 0) {
            const auto requestedLabel = m_activateWhenReadyLabel;
            m_activateWhenReadyLabel = 0;
            activateOrRequestLabel(requestedLabel);
        } else {
            prefetchAdjacentLabel();
        }
    });
    return true;
}

bool Segment3DViewerDialog::rejectPreparedScene(dataType::SegmentIdType labelId) {
    if (labelId == 0 || labelId != m_pendingSceneLabel) {
        return false;
    }

    const bool wasRequestedForActivation = m_activateWhenReadyLabel == labelId;
    m_pendingSceneLabel = 0;
    m_unavailableSceneLabels.insert(labelId);
    if (wasRequestedForActivation) {
        m_activateWhenReadyLabel = 0;
    }
    m_singleLabelNavigationBusy = m_activateWhenReadyLabel != 0;
    updateSingleLabelNavigationUiState();
    QTimer::singleShot(0, this, [this]() {
        if (m_activateWhenReadyLabel != 0) {
            const auto requestedLabel = m_activateWhenReadyLabel;
            m_activateWhenReadyLabel = 0;
            activateOrRequestLabel(requestedLabel);
        } else {
            prefetchAdjacentLabel();
        }
    });
    return wasRequestedForActivation;
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
    if (m_strokeOverlay != nullptr) {
        m_strokeOverlay->raise();
    }

    const bool showExplodeControls = m_explodeSlider != nullptr;
    const bool showCutControls = m_strokeOverlay != nullptr && m_drawCutButton != nullptr;
    const bool showSeededSplitControls = m_seedButtons[0] != nullptr;
    SP_LOG_DEBUG(
        "viewer.three_d",
        QStringLiteral("[3DInputDebug] ready targetLabel=%1 segmentActorCount=%2 "
                       "showExplodeControls=%3 showCutControls=%4 "
                       "showSeededSplitControls=%5 interactorEnabled=%6")
            .arg(m_targetLabelId)
            .arg(m_segmentActors.size())
            .arg(showExplodeControls)
            .arg(showCutControls)
            .arg(showSeededSplitControls)
            .arg(m_vtkWidget != nullptr
                 && m_vtkWidget->interactor() != nullptr
                 && m_vtkWidget->interactor()->GetEnabled()));

    m_initialFrameScheduled = false;
}

bool Segment3DViewerDialog::eventFilter(QObject *watched, QEvent *event) {
    if (event != nullptr) {
        auto *watchedWidget = qobject_cast<QWidget *>(watched);
        const bool eventBelongsToDialog =
            watchedWidget != nullptr
            && (watchedWidget == this || isAncestorOf(watchedWidget));
        if (eventBelongsToDialog && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_D && !keyEvent->isAutoRepeat()) {
                if (deleteCurrentLabel()) {
                    event->accept();
                    return true;
                }
                m_deleteModeActive = true;
            }
        } else if (eventBelongsToDialog && event->type() == QEvent::KeyRelease) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_D && !keyEvent->isAutoRepeat()) {
                m_deleteModeActive = false;
            }
        } else if (event->type() == QEvent::ApplicationDeactivate) {
            m_deleteModeActive = false;
        }
    }

    if (watched == m_vtkWidget && event != nullptr) {
        switch (event->type()) {
            case QEvent::FocusOut:
                m_deleteModeActive = false;
                break;

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
                        if (tryHandlePickedLabelInteraction(
                                pickX, pickY, effectiveModifiers, "qt")) {
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

void Segment3DViewerDialog::cycleSegmentColors() {
    if (m_segmentActors.empty()) {
        return;
    }

    do {
        m_colorCycleSeed = QRandomGenerator::global()->generate();
    } while (m_colorCycleSeed == 0);
    applyColorCycle(m_segmentActors);

    SP_LOG_DEBUG(
        "viewer.three_d",
        QStringLiteral("[3DView] cycled segment colors seed=%1 actors=%2")
            .arg(m_colorCycleSeed, 8, 16, QLatin1Char('0'))
            .arg(m_segmentActors.size()));
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
}

void Segment3DViewerDialog::applyColorCycle(
    std::vector<SegmentActorInfo> &actors) const {
    if (m_colorCycleSeed == 0) {
        return;
    }
    for (auto &actorInfo : actors) {
        setActorColor(
            actorInfo.actor,
            randomCycleColor(actorInfo.labelId, m_colorCycleSeed));
    }
}

void Segment3DViewerDialog::beginCutDrawing() {
    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DCutDebug] beginCutDrawing targetLabel=%1 hasApplyCallback=%2")
                     .arg(m_targetLabelId)
                     .arg(static_cast<bool>(m_cutSession.applyCut)));

    if (m_strokeOverlay == nullptr || m_cutApplyInFlight) {
        return;
    }

    m_cutDrawModeActive = true;
    m_strokeOverlay->setDrawingEnabled(true);
    m_strokeOverlay->raise();
    updateCutUiState();
}

void Segment3DViewerDialog::clearCutStroke() {
    SP_LOG_DEBUG("viewer.three_d",
                 QStringLiteral("[3DCutDebug] clearCutStroke targetLabel=%1").arg(m_targetLabelId));

    if (m_strokeOverlay == nullptr || m_cutApplyInFlight) {
        return;
    }
    m_strokeOverlay->clearStroke();
    updateCutUiState();
}

Projected3DCutRequest Segment3DViewerDialog::buildProjectedStrokeRequest() const {
    Projected3DCutRequest request;
    request.targetWorkingLabel = m_targetLabelId;
    request.strokeWidthPixels = 6.0;

    if (m_strokeOverlay != nullptr) {
        request.viewportSize = {m_strokeOverlay->width(), m_strokeOverlay->height()};
        request.strokePixels = m_strokeOverlay->strokePixels();
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
    if (m_strokeOverlay == nullptr || !m_strokeOverlay->hasValidStroke()) {
        QMessageBox::information(this, tr("3D Cut"), tr("Draw a cut stroke before applying the split."));
        return;
    }

    const Projected3DCutRequest request = buildProjectedStrokeRequest();
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

void Segment3DViewerDialog::armSeedPlacement(int seedNumber) {
    if (seedNumber < 0 || seedNumber >= static_cast<int>(m_seedIndices.size())
        || m_seededSplitBusy || m_seededSplitSession.session == nullptr) {
        return;
    }
    if (m_seededSplitPreview.has_value()) {
        replaceSegmentMeshes(m_originalSeededSplitScene, false);
        m_seededSplitPreview.reset();
    }
    m_lastSeededSplitResult.reset();
    m_lastSeededSplitSeeds.reset();
    m_splitLineDrawModeActive = false;
    m_pendingLineSeeds = {};
    m_havePendingLineSeeds = false;
    m_pendingLineSeedsValid = false;
    if (m_strokeOverlay != nullptr) {
        m_strokeOverlay->setDrawingEnabled(false);
        m_strokeOverlay->setSeedDistancePixels(0.0);
        m_strokeOverlay->clearStroke();
    }
    showSeedActors(m_seedIndices);
    m_activeSeed = seedNumber;
    setSeededSplitStatus(
        QStringLiteral("Click the surface to add a seed to class %1.")
            .arg(seedNumber + 1));
    updateSeededSplitUiState();
}

void Segment3DViewerDialog::beginSplitLineDrawing() {
    if (m_seededSplitBusy || m_strokeOverlay == nullptr
        || m_seededSplitSession.session == nullptr) {
        return;
    }
    if (m_seededSplitPreview.has_value()) {
        replaceSegmentMeshes(m_originalSeededSplitScene, false);
        m_seededSplitPreview.reset();
    }
    m_lastSeededSplitResult.reset();
    m_lastSeededSplitSeeds.reset();
    m_activeSeed = -1;
    m_splitLineDrawModeActive = true;
    m_pendingLineSeeds = {};
    m_havePendingLineSeeds = false;
    m_pendingLineSeedsValid = false;
    showSeedActors(m_seedIndices);
    m_strokeOverlay->setSeedDistancePixels(m_seedDistanceSlider->value());
    m_strokeOverlay->clearStroke();
    m_strokeOverlay->setDrawingEnabled(true);
    m_strokeOverlay->raise();
    setSeededSplitStatus(
        QStringLiteral("Draw a line between the two intended parts."));
    updateSeededSplitUiState();
}

std::optional<Segment3DViewerDialog::SeedRayHit>
Segment3DViewerDialog::seedAlongDisplayRay(double displayX, double displayY) const {
    if (m_renderer == nullptr || m_seededSplitSession.session == nullptr) {
        return std::nullopt;
    }
    SeedRayHit hit;
    for (int endpoint = 0; endpoint < 2; ++endpoint) {
        m_renderer->SetDisplayPoint(displayX, displayY, static_cast<double>(endpoint));
        m_renderer->DisplayToWorld();
        double worldPoint[4]{0.0, 0.0, 0.0, 0.0};
        m_renderer->GetWorldPoint(worldPoint);
        if (std::abs(worldPoint[3]) <= 1e-12) {
            return std::nullopt;
        }
        for (int axis = 0; axis < 3; ++axis) {
            hit.rayEndpoints[endpoint][axis] = worldPoint[axis] / worldPoint[3];
        }
    }
    const auto seed = segment_puzzler::seededSplitMaximumAlongWorldRay(
        *m_seededSplitSession.session,
        hit.rayEndpoints[0],
        hit.rayEndpoints[1]);
    if (!seed.has_value()) {
        return std::nullopt;
    }
    hit.index = seed.value();
    return hit;
}

bool Segment3DViewerDialog::updateSplitLineSeedPreview() {
    if (!m_splitLineDrawModeActive || m_seededSplitBusy
        || m_strokeOverlay == nullptr || !m_strokeOverlay->hasValidStroke()
        || m_vtkWidget == nullptr || m_seededSplitSession.session == nullptr) {
        return false;
    }
    const double samplingPixels = m_lineSamplingSlider != nullptr
                                      ? m_lineSamplingSlider->value()
                                      : kDefaultLineSamplingPixels;
    const int sampleCount = std::clamp(
        static_cast<int>(std::ceil(
            polylineLength(m_strokeOverlay->offsetStroke(0.0))
            / samplingPixels)),
        1,
        kMaximumSplitLineSeedPairs);
    const double offsetPixels = m_seedDistanceSlider != nullptr
                                    ? m_seedDistanceSlider->value()
                                    : kDefaultSeedDistancePixels;
    const auto redSamples = samplePolyline(
        m_strokeOverlay->offsetStroke(-offsetPixels), sampleCount);
    const auto blueSamples = samplePolyline(
        m_strokeOverlay->offsetStroke(offsetPixels), sampleCount);
    segment_puzzler::SeededSplitSeedGroups candidateSeeds;
    const double devicePixelRatio = m_vtkWidget->devicePixelRatioF();
    for (std::size_t sample = 0;
         sample < std::min(redSamples.size(), blueSamples.size()); ++sample) {
        const auto displayHit = [this, devicePixelRatio](const QPointF &point) {
            return seedAlongDisplayRay(
                point.x() * devicePixelRatio,
                (m_vtkWidget->height() - point.y() - 1.0) * devicePixelRatio);
        };
        const auto redHit = displayHit(redSamples[sample]);
        const auto blueHit = displayHit(blueSamples[sample]);
        if (!redHit.has_value() || !blueHit.has_value()
            || redHit->index == blueHit->index) {
            continue;
        }
        if (std::find(candidateSeeds[0].begin(), candidateSeeds[0].end(), redHit->index)
            == candidateSeeds[0].end()) {
            candidateSeeds[0].push_back(redHit->index);
        }
        if (std::find(candidateSeeds[1].begin(), candidateSeeds[1].end(), blueHit->index)
            == candidateSeeds[1].end()) {
            candidateSeeds[1].push_back(blueHit->index);
        }
    }
    const bool seedsOverlap = std::any_of(
        candidateSeeds[0].begin(), candidateSeeds[0].end(),
        [&candidateSeeds](const auto &redSeed) {
            return std::find(
                       candidateSeeds[1].begin(),
                       candidateSeeds[1].end(),
                       redSeed)
                   != candidateSeeds[1].end();
        });
    m_pendingLineSeeds = std::move(candidateSeeds);
    m_havePendingLineSeeds = true;
    m_pendingLineSeedsValid = !seedsOverlap
                              && !m_pendingLineSeeds[0].empty()
                              && !m_pendingLineSeeds[1].empty();
    showSeedActors(m_pendingLineSeeds);
    SP_LOG_DEBUG(
        "segmentation",
        QStringLiteral(
            "operation=seeded_watershed_split phase=line_seed_preview "
            "source_label=%1 line_offset_px=%2 line_sampling_px=%3 "
            "requested_pairs=%4 accepted_seed_counts=%5,%6 valid=%7 overlap=%8")
            .arg(m_seededSplitSession.session->sourceLabel)
            .arg(offsetPixels, 0, 'g', 9)
            .arg(samplingPixels, 0, 'g', 9)
            .arg(sampleCount)
            .arg(m_pendingLineSeeds[0].size())
            .arg(m_pendingLineSeeds[1].size())
            .arg(m_pendingLineSeedsValid)
            .arg(seedsOverlap));
    if (seedsOverlap) {
        setSeededSplitStatus(
            QStringLiteral(
                "Red and blue line seeds overlap; increase Line Offset."),
            true);
    } else if (!m_pendingLineSeedsValid) {
        setSeededSplitStatus(
            QStringLiteral(
                "Could not place seeds on both sides; adjust the line settings."),
            true);
    } else {
        setSeededSplitStatus(
            QStringLiteral(
                "Line preview: %1 red and %2 blue seeds. Adjust or Confirm Seeds.")
                .arg(m_pendingLineSeeds[0].size())
                .arg(m_pendingLineSeeds[1].size()));
    }
    updateSeededSplitUiState();
    if (m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
    return m_pendingLineSeedsValid;
}

void Segment3DViewerDialog::confirmSplitLineSeeds() {
    if (m_seededSplitBusy || !m_splitLineDrawModeActive
        || !m_havePendingLineSeeds || !m_pendingLineSeedsValid
        || m_strokeOverlay == nullptr || m_seededSplitSession.session == nullptr) {
        return;
    }
    m_seedIndices = m_pendingLineSeeds;
    m_pendingLineSeeds = {};
    m_havePendingLineSeeds = false;
    m_pendingLineSeedsValid = false;
    m_splitLineDrawModeActive = false;
    m_strokeOverlay->setDrawingEnabled(false);
    m_strokeOverlay->setSeedDistancePixels(0.0);
    m_strokeOverlay->clearStroke();
    showSeedActors(m_seedIndices);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral(
            "operation=seeded_watershed_split phase=line_seeds_confirmed "
            "source_label=%1 line_offset_px=%2 line_sampling_px=%3 "
            "seed_counts=%4,%5 seeds_local_1=%6 seeds_local_2=%7 "
            "seeds_global_1=%8 seeds_global_2=%9")
            .arg(m_seededSplitSession.session->sourceLabel)
            .arg(m_seedDistanceSlider->value())
            .arg(m_lineSamplingSlider->value())
            .arg(m_seedIndices[0].size())
            .arg(m_seedIndices[1].size())
            .arg(formatSeedIndices(m_seedIndices[0]))
            .arg(formatSeedIndices(m_seedIndices[1]))
            .arg(formatSeedIndices(
                m_seedIndices[0], &m_seededSplitSession.session->globalOffset))
            .arg(formatSeedIndices(
                m_seedIndices[1], &m_seededSplitSession.session->globalOffset)));
    setSeededSplitStatus(
        QStringLiteral("Confirmed %1 red and %2 blue seeds.")
            .arg(m_seedIndices[0].size())
            .arg(m_seedIndices[1].size()));
    updateSeededSplitUiState();
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
    autoPreviewSeededSplitIfReady();
}

bool Segment3DViewerDialog::placeSeedAt(int pickX, int pickY) {
    if (m_activeSeed < 0 || m_seededSplitBusy || m_renderer == nullptr
        || m_seededSplitSession.session == nullptr) {
        return false;
    }

    auto picker = vtkSmartPointer<vtkCellPicker>::New();
    picker->SetTolerance(0.0005);
    picker->PickFromListOn();
    for (const auto &actorInfo : m_segmentActors) {
        if (actorInfo.actor != nullptr) {
            picker->AddPickList(actorInfo.actor);
        }
    }
    if (picker->Pick(pickX, pickY, 0.0, m_renderer) == 0) {
        setSeededSplitStatus(QStringLiteral("No segment surface was hit; try again."));
        return true;
    }

    const auto hit = seedAlongDisplayRay(pickX, pickY);
    if (!hit.has_value()) {
        setSeededSplitStatus(
            QStringLiteral("Could not find segment voxels along the viewing ray."));
        return true;
    }
    const auto &raySeed = hit->index;

    const int seedNumber = m_activeSeed;
    const int otherSeed = 1 - seedNumber;
    if (std::find(m_seedIndices[otherSeed].begin(),
                  m_seedIndices[otherSeed].end(),
                  raySeed) != m_seedIndices[otherSeed].end()) {
        setSeededSplitStatus(
            QStringLiteral("That voxel already belongs to seed class %1.")
                .arg(otherSeed + 1));
        return true;
    }
    if (std::find(m_seedIndices[seedNumber].begin(),
                  m_seedIndices[seedNumber].end(),
                  raySeed) != m_seedIndices[seedNumber].end()) {
        setSeededSplitStatus(
            QStringLiteral("That seed is already present in class %1.")
                .arg(seedNumber + 1));
        return true;
    }

    if (m_seededSplitPreview.has_value()) {
        replaceSegmentMeshes(m_originalSeededSplitScene, false);
        m_seededSplitPreview.reset();
    }
    m_seedIndices[seedNumber].push_back(raySeed);

    auto globalSeed = raySeed;
    for (unsigned int axis = 0; axis < 3; ++axis) {
        globalSeed[axis] += m_seededSplitSession.session->globalOffset[axis];
    }
    const auto seedWorld = segment_puzzler::seededSplitSeedWorldPoint(
        *m_seededSplitSession.session, raySeed);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral(
            "operation=seeded_watershed_split phase=seed_placed source_label=%1 "
            "seed_class=%2 class_seed_count=%3 pick_display=[%4,%5] "
            "local_seed=%6 global_seed=%7 "
            "world_seed=%8 distance=%9 maximum_distance=%10 "
            "ray_start=%11 ray_end=%12")
            .arg(m_seededSplitSession.session->sourceLabel)
            .arg(seedNumber + 1)
            .arg(m_seedIndices[seedNumber].size())
            .arg(pickX)
            .arg(pickY)
            .arg(formatTriple(raySeed))
            .arg(formatTriple(globalSeed))
            .arg(formatTriple(seedWorld))
            .arg(m_seededSplitSession.session->distance->GetPixel(raySeed), 0, 'g', 9)
            .arg(m_seededSplitSession.session->maximumDistance, 0, 'g', 9)
            .arg(formatTriple(hit->rayEndpoints[0]))
            .arg(formatTriple(hit->rayEndpoints[1])));

    m_lastSeededSplitResult.reset();
    m_lastSeededSplitSeeds.reset();
    m_activeSeed = -1;
    showSeedActors(m_seedIndices);
    setSeededSplitStatus(
        QStringLiteral("Seed added to class %1 (%2 class 1, %3 class 2).")
            .arg(seedNumber + 1)
            .arg(m_seedIndices[0].size())
            .arg(m_seedIndices[1].size()));
    updateSeededSplitUiState();
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
    autoPreviewSeededSplitIfReady();
    return true;
}

void Segment3DViewerDialog::showSeedActors(
    const segment_puzzler::SeededSplitSeedGroups &seeds)
{
    if (m_renderer == nullptr) {
        return;
    }
    vtkRenderer *seedRenderer =
        m_seedRenderer != nullptr ? m_seedRenderer.GetPointer() : m_renderer.GetPointer();
    for (auto &actors : m_seedActors) {
        for (auto &actor : actors) {
            if (actor != nullptr) {
                seedRenderer->RemoveActor(actor);
            }
        }
        actors.clear();
    }
    if (m_seededSplitSession.session == nullptr) {
        return;
    }

    const auto spacing = m_seededSplitSession.session->mask->GetSpacing();
    for (int seedNumber = 0; seedNumber < static_cast<int>(seeds.size()); ++seedNumber) {
        for (const auto &seed : seeds[seedNumber]) {
            const auto point = segment_puzzler::seededSplitSeedWorldPoint(
                *m_seededSplitSession.session, seed);
            auto sphere = vtkSmartPointer<vtkSphereSource>::New();
            sphere->SetCenter(point.data());
            sphere->SetRadius(std::max(
                2.5 * std::min({spacing[0], spacing[1], spacing[2]}),
                0.012 * m_sceneExtent));
            sphere->SetThetaResolution(20);
            sphere->SetPhiResolution(20);

            auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputConnection(sphere->GetOutputPort());
            auto actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);
            actor->PickableOff();
            if (seedNumber == 0) {
                actor->GetProperty()->SetColor(1.0, 0.2, 0.15);
            } else {
                actor->GetProperty()->SetColor(0.15, 0.85, 1.0);
            }
            seedRenderer->AddActor(actor);
            m_seedActors[seedNumber].push_back(actor);
        }
    }
}

void Segment3DViewerDialog::clearSeededSplit() {
    if (m_seededSplitBusy) {
        return;
    }
    m_activeSeed = -1;
    m_splitLineDrawModeActive = false;
    m_seedIndices = {};
    m_pendingLineSeeds = {};
    m_havePendingLineSeeds = false;
    m_pendingLineSeedsValid = false;
    m_lastSeededSplitResult.reset();
    m_lastSeededSplitSeeds.reset();
    if (m_strokeOverlay != nullptr) {
        m_strokeOverlay->setDrawingEnabled(false);
        m_strokeOverlay->setSeedDistancePixels(0.0);
        m_strokeOverlay->clearStroke();
    }
    showSeedActors(m_seedIndices);
    if (m_seededSplitPreview.has_value()) {
        replaceSegmentMeshes(m_originalSeededSplitScene, false);
        m_seededSplitPreview.reset();
    }
    setSeededSplitStatus(QStringLiteral("Add at least one seed to each class."));
    updateSeededSplitUiState();
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
}

void Segment3DViewerDialog::updateSeededSplitSmoothing() {
    m_seededSplitSmoothingPending = false;
    if (m_seededSplitBusy || m_seededSplitSession.session == nullptr
        || m_smoothingSlider == nullptr) {
        updateSeededSplitUiState();
        return;
    }

    const double sigmaPixels =
        static_cast<double>(m_smoothingSlider->value())
        / kSmoothingSliderStepsPerPixel;
    auto &session = *m_seededSplitSession.session;
    if (std::abs(sigmaPixels - session.landscapeSmoothingSigmaPixels) > 1e-9) {
        try {
            segment_puzzler::updateSeededWatershedSplitLandscape(
                session, sigmaPixels);
            SP_LOG_INFO(
                "segmentation",
                QStringLiteral(
                    "operation=seeded_watershed_split phase=landscape_update "
                    "source_label=%1 smoothing_sigma_px=%2 landscape_hash=%3 "
                    "smoothing_ms=%4")
                    .arg(session.sourceLabel)
                    .arg(session.landscapeSmoothingSigmaPixels, 0, 'f', 1)
                    .arg(formatHash(session.landscapeHash))
                    .arg(session.landscapeSmoothingMs, 0, 'f', 1));
        } catch (const std::exception &error) {
            const QString message = QStringLiteral("Could not update smoothing: %1")
                                        .arg(QString::fromUtf8(error.what()));
            SP_LOG_WARNING(
                "segmentation",
                QStringLiteral(
                    "operation=seeded_watershed_split status=landscape_update_failed "
                    "source_label=%1 smoothing_sigma_px=%2 message=\"%3\"")
                    .arg(session.sourceLabel)
                    .arg(sigmaPixels, 0, 'f', 1)
                    .arg(message));
            setSeededSplitStatus(message);
            updateSeededSplitUiState();
            return;
        }
    }

    setSeededSplitStatus(QStringLiteral("Smoothing updated; press Preview."));
    updateSeededSplitUiState();
    autoPreviewSeededSplitIfReady();
}

void Segment3DViewerDialog::autoPreviewSeededSplitIfReady() {
    if (m_autoPreviewCheckBox == nullptr || !m_autoPreviewCheckBox->isChecked()
        || m_seededSplitBusy || m_seededSplitSmoothingPending
        || m_seededSplitPreview.has_value()) {
        return;
    }
    if (!m_seedIndices[0].empty() && !m_seedIndices[1].empty()) {
        previewSeededSplit();
    }
}

void Segment3DViewerDialog::previewSeededSplit() {
    const bool haveSeeds = !m_seedIndices[0].empty() && !m_seedIndices[1].empty();
    if (m_seededSplitBusy || m_seededSplitSmoothingPending
        || m_seededSplitSession.session == nullptr || !haveSeeds) {
        return;
    }

    const auto session = m_seededSplitSession.session;
    const auto seeds = m_seedIndices;
    segment_puzzler::SeededWatershedSplitOptions options;
    options.compactness = m_compactWatershedCheckBox != nullptr
                                  && m_compactWatershedCheckBox->isChecked()
                              ? segment_puzzler::kDefaultSeededSplitCompactness
                              : 0.0;
    options.connectSeeds = m_connectSeedsCheckBox != nullptr
                           && m_connectSeedsCheckBox->isChecked();
    options.allowDisconnectedParts = m_allowDisconnectedCheckBox != nullptr
                                     && m_allowDisconnectedCheckBox->isChecked();

    const auto maskRegion = session->mask->GetLargestPossibleRegion();
    const QString commonInput = QStringLiteral(
        "source_label=%1 source_mtime=%2 source_voxels=%3 source_roi=[%4,%5,%6,%7,%8,%9] "
        "global_offset=%10 mask_size=%11 spacing=%12 origin=%13 mask_hash=%14 "
        "maximum_distance=%15 distance=foreground_to_background_voxel_center "
        "landscape=negative_mask_normalized_gaussian_distance "
        "landscape_smoothing_sigma_px=%16 landscape_hash=%17 "
        "landscape_smoothing_ms=%18 distance_use_spacing=1 "
        "landscape_smoothing_use_spacing=0 markers=imposed_minima domain_mask=1")
        .arg(session->sourceLabel)
        .arg(static_cast<qulonglong>(session->sourceModifiedTime))
        .arg(session->voxelCount)
        .arg(session->sourceRoi.minX)
        .arg(session->sourceRoi.minY)
        .arg(session->sourceRoi.minZ)
        .arg(session->sourceRoi.maxX)
        .arg(session->sourceRoi.maxY)
        .arg(session->sourceRoi.maxZ)
        .arg(formatTriple(session->globalOffset))
        .arg(formatTriple(maskRegion.GetSize()))
        .arg(formatTriple(session->mask->GetSpacing()))
        .arg(formatTriple(session->mask->GetOrigin()))
        .arg(formatHash(session->maskHash))
        .arg(session->maximumDistance, 0, 'g', 9)
        .arg(session->landscapeSmoothingSigmaPixels, 0, 'g', 9)
        .arg(formatHash(session->landscapeHash))
        .arg(session->landscapeSmoothingMs, 0, 'f', 1);
    const QString algorithmInput = commonInput
        + (options.compactness > 0.0
               ? QStringLiteral(
                     " watershed=compact_marker compactness=%1 fully_connected=0 "
                     "priority=minimax_normalized_landscape_plus_compactness_times_"
                     "normalized_seed_distance")
                     .arg(options.compactness, 0, 'g', 9)
               : QStringLiteral(
                     " watershed=fast_marker compactness=0 fully_connected=0 "
                     "tie_break=minimax_then_geodesic"))
        + QStringLiteral(" connect_seeds=%1 allow_disconnected_parts=%2")
              .arg(options.connectSeeds)
              .arg(options.allowDisconnectedParts);
    SP_LOG_INFO(
        "segmentation",
        QStringLiteral(
            "operation=seeded_watershed_split phase=preview_request mode=seeds %1 "
            "seed_counts=[%2,%3] seeds_local_1=%4 seeds_local_2=%5 "
            "seeds_global_1=%6 seeds_global_2=%7 "
            "seed_distances_1=%8 seed_distances_2=%9")
            .arg(algorithmInput)
            .arg(seeds[0].size())
            .arg(seeds[1].size())
            .arg(formatSeedIndices(seeds[0]))
            .arg(formatSeedIndices(seeds[1]))
            .arg(formatSeedIndices(seeds[0], &session->globalOffset))
            .arg(formatSeedIndices(seeds[1], &session->globalOffset))
            .arg(formatSeedDistances(seeds[0], session->distance))
            .arg(formatSeedDistances(seeds[1], session->distance)));
    if (m_seededSplitPreview.has_value()) {
        replaceSegmentMeshes(m_originalSeededSplitScene, false);
        m_seededSplitPreview.reset();
    }
    m_lastSeededSplitResult.reset();
    m_lastSeededSplitSeeds = seeds;
    m_seededSplitBusy = true;
    setSeededSplitStatus(QStringLiteral("Running marker-controlled watershed..."));
    updateSeededSplitUiState();

    const auto computePreview = [session, seeds, options]() {
        auto result = segment_puzzler::computeSeededWatershedSplit(
            *session, seeds, options);
        PreparedScene scene;
        if (result.valid()) {
            scene = Segment3DViewerDialog::prepareScene(
                result.partition, {{1, 0xF2483D}, {2, 0x26A6FF}});
        }
        return std::make_pair(std::move(result), std::move(scene));
    };
    const auto finishPreview = [this](auto preview) {
        m_seededSplitBusy = false;
        auto &[result, scene] = preview;
        m_lastSeededSplitResult = result;
        if (!result.valid() || !replaceSegmentMeshes(scene, false)) {
            const bool disconnectedRejected =
                !result.valid() && result.hasDisconnectedParts()
                && !result.disconnectedPartsAllowed;
            const QString message = disconnectedRejected
                ? QStringLiteral(
                      "Disconnected result (red: %1 regions, blue: %2 regions). "
                      "Enable Allow disconnected parts to preview and apply it.")
                      .arg(result.connectedComponentCounts[0])
                      .arg(result.connectedComponentCounts[1])
                : result.error.empty()
                    ? QStringLiteral("Could not create both preview surfaces.")
                    : QString::fromStdString(result.error);
            SP_LOG_WARNING(
                "segmentation",
                QStringLiteral(
                    "operation=seeded_watershed_split status=preview_failed "
                    "source_label=%1 marker_voxels=%2,%3 connection_voxels=%4,%5 "
                    "connect_seeds=%6 marker_connection_ms=%7 marker_hash=%8 "
                    "part_voxels=%9,%10 part_components=%11,%12 compactness=%13 "
                    "allow_disconnected_parts=%14 flood_pops=%15 flood_requeues=%16 "
                    "watershed_ms=%17 message=\"%18\"")
                    .arg(m_seededSplitSession.session->sourceLabel)
                    .arg(result.markerVoxelCounts[0])
                    .arg(result.markerVoxelCounts[1])
                    .arg(result.connectionVoxelCounts[0])
                    .arg(result.connectionVoxelCounts[1])
                    .arg(result.connectSeeds)
                    .arg(result.markerConnectionMs, 0, 'f', 1)
                    .arg(formatHash(result.markerHash))
                    .arg(result.voxelCounts[0])
                    .arg(result.voxelCounts[1])
                    .arg(result.connectedComponentCounts[0])
                    .arg(result.connectedComponentCounts[1])
                    .arg(result.compactness, 0, 'g', 9)
                    .arg(result.disconnectedPartsAllowed)
                    .arg(result.floodMetrics.popCount)
                    .arg(result.floodMetrics.requeueCount)
                    .arg(result.watershedMs, 0, 'f', 1)
                    .arg(message));
            setSeededSplitStatus(message, disconnectedRejected);
            QMessageBox::information(this, tr("Seeded Split"), message);
            updateSeededSplitUiState();
            return;
        }

        if (m_strokeOverlay != nullptr) {
            m_strokeOverlay->setDrawingEnabled(false);
        }
        m_seededSplitPreview = result;
        SP_LOG_DEBUG(
            "segmentation",
            QStringLiteral(
                "operation=seeded_watershed_split phase=preview source_label=%1 "
                "marker_voxels=%2,%3 connection_voxels=%4,%5 connect_seeds=%6 "
                "marker_connection_ms=%7 marker_hash=%8 part_voxels=%9,%10 "
                "partition_hash=%11 part_components=%12,%13 compactness=%14 "
                "allow_disconnected_parts=%15 flood_pops=%16 flood_requeues=%17 "
                "flood_ms=%18 watershed_ms=%19")
                .arg(m_seededSplitSession.session->sourceLabel)
                .arg(m_seededSplitPreview->markerVoxelCounts[0])
                .arg(m_seededSplitPreview->markerVoxelCounts[1])
                .arg(m_seededSplitPreview->connectionVoxelCounts[0])
                .arg(m_seededSplitPreview->connectionVoxelCounts[1])
                .arg(m_seededSplitPreview->connectSeeds)
                .arg(m_seededSplitPreview->markerConnectionMs, 0, 'f', 1)
                .arg(formatHash(m_seededSplitPreview->markerHash))
                .arg(m_seededSplitPreview->voxelCounts[0])
                .arg(m_seededSplitPreview->voxelCounts[1])
                .arg(formatHash(m_seededSplitPreview->partitionHash))
                .arg(m_seededSplitPreview->connectedComponentCounts[0])
                .arg(m_seededSplitPreview->connectedComponentCounts[1])
                .arg(m_seededSplitPreview->compactness, 0, 'g', 9)
                .arg(m_seededSplitPreview->disconnectedPartsAllowed)
                .arg(m_seededSplitPreview->floodMetrics.popCount)
                .arg(m_seededSplitPreview->floodMetrics.requeueCount)
                .arg(m_seededSplitPreview->floodMetrics.floodMs, 0, 'f', 1)
                .arg(m_seededSplitPreview->watershedMs, 0, 'f', 1));
        if (m_seededSplitPreview->hasDisconnectedParts()) {
            SP_LOG_WARNING(
                "segmentation",
                QStringLiteral(
                    "operation=seeded_watershed_split status=preview_disconnected "
                    "source_label=%1 part_components=%2,%3 "
                    "allow_disconnected_parts=1")
                    .arg(m_seededSplitSession.session->sourceLabel)
                    .arg(m_seededSplitPreview->connectedComponentCounts[0])
                    .arg(m_seededSplitPreview->connectedComponentCounts[1]));
            setSeededSplitStatus(
                QStringLiteral(
                    "Warning: disconnected result (red: %1 regions, blue: %2 regions). "
                    "Preview: %3 + %4 voxels (%5 ms).")
                    .arg(m_seededSplitPreview->connectedComponentCounts[0])
                    .arg(m_seededSplitPreview->connectedComponentCounts[1])
                    .arg(m_seededSplitPreview->voxelCounts[0])
                    .arg(m_seededSplitPreview->voxelCounts[1])
                    .arg(m_seededSplitPreview->watershedMs, 0, 'f', 1),
                true);
        } else {
            setSeededSplitStatus(
                QStringLiteral("Preview: %1 + %2 voxels (%3 ms).")
                    .arg(m_seededSplitPreview->voxelCounts[0])
                    .arg(m_seededSplitPreview->voxelCounts[1])
                    .arg(m_seededSplitPreview->watershedMs, 0, 'f', 1));
        }
        updateSeededSplitUiState();
    };

    finishPreview(computePreview());
}

void Segment3DViewerDialog::exportSeededSplitDebugBundle() {
    if (m_seededSplitSession.session == nullptr || !m_lastSeededSplitResult.has_value()) {
        QMessageBox::information(
            this,
            tr("Seeded Split Debug Export"),
            tr("Run Preview first. The latest successful or failed preview can then be exported."));
        return;
    }

    const QString selectedDirectory = QFileDialog::getExistingDirectory(
        this, tr("Export Seeded Split Debug Bundle"), QDir::homePath());
    if (selectedDirectory.isEmpty()) {
        return;
    }

    const auto &session = *m_seededSplitSession.session;
    const auto &result = m_lastSeededSplitResult.value();
    const QString bundleName = QStringLiteral("seeded_split_%1_%2")
                                   .arg(session.sourceLabel)
                                   .arg(QDateTime::currentDateTime().toString(
                                       QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    QDir parentDirectory(selectedDirectory);
    if (!parentDirectory.mkdir(bundleName)) {
        QMessageBox::warning(
            this, tr("Seeded Split Debug Export"),
            tr("Could not create the debug bundle directory."));
        return;
    }
    const QString bundlePath = parentDirectory.filePath(bundleName);
    QDir bundleDirectory(bundlePath);

    try {
        QJsonObject files;
        const auto writeImage = [&](const QString &fileName, const auto *image) {
            if (image == nullptr) {
                return;
            }
            writeDebugImage(image, bundleDirectory.filePath(fileName));
            files.insert(fileName.left(fileName.size() - 5), fileName);
        };
        writeImage(QStringLiteral("mask.nrrd"), session.mask.GetPointer());
        writeImage(QStringLiteral("distance.nrrd"), session.distance.GetPointer());
        writeImage(QStringLiteral("landscape.nrrd"), session.landscape.GetPointer());
        writeImage(QStringLiteral("markers.nrrd"), result.markers.GetPointer());
        writeImage(QStringLiteral("partition.nrrd"), result.partition.GetPointer());

        const auto maskRegion = session.mask->GetLargestPossibleRegion();
        const auto direction = session.mask->GetDirection();
        QJsonArray directionRows;
        for (unsigned int row = 0; row < 3; ++row) {
            directionRows.append(QJsonArray{
                direction[row][0], direction[row][1], direction[row][2]});
        }

        QJsonObject input;
        if (m_lastSeededSplitSeeds.has_value()) {
            input.insert(QStringLiteral("mode"), QStringLiteral("seeds"));
            QJsonArray seeds;
            for (std::size_t seedClass = 0;
                 seedClass < m_lastSeededSplitSeeds->size(); ++seedClass) {
                for (const auto &localSeed : m_lastSeededSplitSeeds.value()[seedClass]) {
                    auto globalSeed = localSeed;
                    for (unsigned int axis = 0; axis < 3; ++axis) {
                        globalSeed[axis] += session.globalOffset[axis];
                    }
                    seeds.append(QJsonObject{
                        {QStringLiteral("marker_label"), static_cast<int>(seedClass + 1)},
                        {QStringLiteral("local_index"), jsonTriple(localSeed)},
                        {QStringLiteral("global_index"), jsonTriple(globalSeed)},
                        {QStringLiteral("world"), jsonTriple(
                             segment_puzzler::seededSplitSeedWorldPoint(session, localSeed))},
                        {QStringLiteral("distance"),
                         static_cast<double>(session.distance->GetPixel(localSeed))}});
                }
            }
            input.insert(QStringLiteral("seeds"), seeds);
        } else {
            input.insert(QStringLiteral("mode"), QStringLiteral("unknown"));
        }

        QJsonObject algorithm{
            {QStringLiteral("distance"),
             QStringLiteral("foreground_to_background_voxel_center")},
            {QStringLiteral("distance_use_image_spacing"), true},
            {QStringLiteral("landscape"),
             QStringLiteral("negative_mask_normalized_gaussian_distance")},
            {QStringLiteral("landscape_smoothing_sigma_pixels"),
             session.landscapeSmoothingSigmaPixels},
            {QStringLiteral("landscape_smoothing_use_image_spacing"), false},
            {QStringLiteral("landscape_hash"), formatHash(session.landscapeHash)},
            {QStringLiteral("markers"), QStringLiteral("imposed_minima")},
            {QStringLiteral("domain_mask"), true},
            {QStringLiteral("connect_seeds"), result.connectSeeds},
            {QStringLiteral("seed_connection"),
             QStringLiteral("six_connected_minimax_landscape_then_physical_distance")},
            {QStringLiteral("allow_disconnected_parts"),
             result.disconnectedPartsAllowed}};
        if (result.compactness > 0.0) {
            algorithm.insert(
                QStringLiteral("watershed"),
                QStringLiteral("segmentpuzzler_compact_marker"));
            algorithm.insert(
                QStringLiteral("compactness"),
                result.compactness);
            algorithm.insert(QStringLiteral("fully_connected"), false);
            algorithm.insert(
                QStringLiteral("priority"),
                QStringLiteral(
                    "minimax(normalized_landscape + compactness * "
                    "normalized_seed_distance)"));
            algorithm.insert(
                QStringLiteral("seed_distance"),
                QStringLiteral("physical_euclidean_normalized_by_roi_diagonal"));
        } else {
            algorithm.insert(
                QStringLiteral("watershed"),
                QStringLiteral("segmentpuzzler_fast_marker"));
            algorithm.insert(QStringLiteral("compactness"), 0.0);
            algorithm.insert(QStringLiteral("fully_connected"), false);
            algorithm.insert(
                QStringLiteral("tie_break"),
                QStringLiteral("minimax_level_geodesic_distance_label_index"));
        }

        const QJsonObject metadata{
            {QStringLiteral("format_version"), 3},
            {QStringLiteral("created_utc"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("operation"), QStringLiteral("seeded_watershed_split")},
            {QStringLiteral("source"), QJsonObject{
                 {QStringLiteral("label"), static_cast<double>(session.sourceLabel)},
                 {QStringLiteral("modified_time"),
                  QString::number(static_cast<qulonglong>(session.sourceModifiedTime))},
                 {QStringLiteral("voxel_count"), static_cast<double>(session.voxelCount)},
                 {QStringLiteral("connected_components"),
                  static_cast<double>(session.connectedComponentCount)},
                 {QStringLiteral("mask_hash"), formatHash(session.maskHash)},
                 {QStringLiteral("roi"), QJsonObject{
                      {QStringLiteral("min"), QJsonArray{
                           session.sourceRoi.minX,
                           session.sourceRoi.minY,
                           session.sourceRoi.minZ}},
                      {QStringLiteral("max"), QJsonArray{
                           session.sourceRoi.maxX,
                           session.sourceRoi.maxY,
                           session.sourceRoi.maxZ}}}},
                 {QStringLiteral("global_offset"), jsonTriple(session.globalOffset)}}},
            {QStringLiteral("geometry"), QJsonObject{
                 {QStringLiteral("region_start"), jsonTriple(maskRegion.GetIndex())},
                 {QStringLiteral("region_size"), jsonTriple(maskRegion.GetSize())},
                 {QStringLiteral("spacing"), jsonTriple(session.mask->GetSpacing())},
                 {QStringLiteral("origin"), jsonTriple(session.mask->GetOrigin())},
                 {QStringLiteral("direction"), directionRows}}},
            {QStringLiteral("preparation"), QJsonObject{
                 {QStringLiteral("maximum_distance"), session.maximumDistance},
                 {QStringLiteral("mask_and_connectivity_ms"),
                  session.maskAndConnectivityMs},
                 {QStringLiteral("distance_transform_ms"),
                  session.distanceTransformMs},
                 {QStringLiteral("landscape_smoothing_ms"),
                  session.landscapeSmoothingMs}}},
            {QStringLiteral("algorithm"), algorithm},
            {QStringLiteral("input"), input},
            {QStringLiteral("result"), QJsonObject{
                 {QStringLiteral("valid"), result.valid()},
                 {QStringLiteral("error"), QString::fromStdString(result.error)},
                 {QStringLiteral("marker_hash"), formatHash(result.markerHash)},
                 {QStringLiteral("marker_voxel_counts"), QJsonArray{
                      static_cast<double>(result.markerVoxelCounts[0]),
                      static_cast<double>(result.markerVoxelCounts[1])}},
                 {QStringLiteral("connection_voxel_counts"), QJsonArray{
                      static_cast<double>(result.connectionVoxelCounts[0]),
                      static_cast<double>(result.connectionVoxelCounts[1])}},
                 {QStringLiteral("marker_connection_ms"), result.markerConnectionMs},
                 {QStringLiteral("part_voxel_counts"), QJsonArray{
                      static_cast<double>(result.voxelCounts[0]),
                      static_cast<double>(result.voxelCounts[1])}},
                 {QStringLiteral("part_connected_component_counts"), QJsonArray{
                      static_cast<double>(result.connectedComponentCounts[0]),
                      static_cast<double>(result.connectedComponentCounts[1])}},
                 {QStringLiteral("has_disconnected_parts"),
                  result.hasDisconnectedParts()},
                 {QStringLiteral("partition_hash"), formatHash(result.partitionHash)},
                 {QStringLiteral("watershed_ms"), result.watershedMs},
                 {QStringLiteral("flood_metrics"), QJsonObject{
                      {QStringLiteral("elapsed_ms"), result.floodMetrics.elapsedMs},
                      {QStringLiteral("quantize_ms"), result.floodMetrics.quantizeMs},
                      {QStringLiteral("init_ms"), result.floodMetrics.initMs},
                      {QStringLiteral("flood_ms"), result.floodMetrics.floodMs},
                      {QStringLiteral("writeback_ms"), result.floodMetrics.writebackMs},
                      {QStringLiteral("pop_count"),
                       static_cast<double>(result.floodMetrics.popCount)},
                      {QStringLiteral("stale_pop_count"),
                       static_cast<double>(result.floodMetrics.stalePopCount)},
                      {QStringLiteral("requeue_count"),
                       static_cast<double>(result.floodMetrics.requeueCount)},
                      {QStringLiteral("finalized_voxel_count"),
                       static_cast<double>(result.floodMetrics.finalizedVoxelCount)},
                      {QStringLiteral("max_queue_depth"),
                       static_cast<double>(result.floodMetrics.maxQueueDepth)},
                      {QStringLiteral("scratch_bytes"),
                       static_cast<double>(result.floodMetrics.scratchBytes)}}}}},
            {QStringLiteral("files"), files}};

        QSaveFile metadataFile(bundleDirectory.filePath(QStringLiteral("metadata.json")));
        if (!metadataFile.open(QIODevice::WriteOnly)) {
            throw std::runtime_error(metadataFile.errorString().toStdString());
        }
        if (metadataFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented)) < 0
            || !metadataFile.commit()) {
            throw std::runtime_error(metadataFile.errorString().toStdString());
        }

        SP_LOG_INFO(
            "segmentation",
            QStringLiteral(
                "operation=seeded_watershed_split phase=debug_export source_label=%1 "
                "path=\"%2\" files=%3")
                .arg(session.sourceLabel)
                .arg(bundlePath)
                .arg(files.size()));
        if (result.hasDisconnectedParts()) {
            setSeededSplitStatus(
                QStringLiteral(
                    "Warning: disconnected result (red: %1 regions, blue: %2 regions). "
                    "Debug bundle exported: %3")
                    .arg(result.connectedComponentCounts[0])
                    .arg(result.connectedComponentCounts[1])
                    .arg(bundlePath),
                true);
        } else {
            setSeededSplitStatus(
                QStringLiteral("Debug bundle exported: %1").arg(bundlePath));
        }
    } catch (const std::exception &error) {
        SP_LOG_WARNING(
            "segmentation",
            QStringLiteral(
                "operation=seeded_watershed_split status=debug_export_failed "
                "source_label=%1 path=\"%2\" message=\"%3\"")
                .arg(session.sourceLabel)
                .arg(bundlePath)
                .arg(QString::fromUtf8(error.what())));
        QMessageBox::warning(
            this,
            tr("Seeded Split Debug Export"),
            tr("Could not export the debug bundle: %1")
                .arg(QString::fromUtf8(error.what())));
    }
}

void Segment3DViewerDialog::applySeededSplit() {
    if (m_seededSplitBusy || !m_seededSplitPreview.has_value()
        || !m_seededSplitSession.applySplit) {
        return;
    }

    m_seededSplitBusy = true;
    updateSeededSplitUiState();
    const auto applySplit = m_seededSplitSession.applySplit;
    const auto preview = m_seededSplitPreview.value();
    const auto finishApply = [this](SeededSplitApplyResult result) {
        if (result.mutated) {
            if (m_strokeOverlay != nullptr) {
                m_strokeOverlay->setDrawingEnabled(false);
                m_strokeOverlay->clearStroke();
            }
            if (!result.message.isEmpty()) {
                QMessageBox::warning(this, tr("Seeded Split"), result.message);
            }
            accept();
            return;
        }
        m_seededSplitBusy = false;
        updateSeededSplitUiState();
        QMessageBox::information(
            this,
            tr("Seeded Split"),
            result.message.isEmpty() ? tr("The segment could not be split.") : result.message);
    };

    if (m_seededSplitSession.taskRunner != nullptr) {
        m_seededSplitSession.taskRunner->runWithLabel(
            m_seededSplitSession.progressText,
            [applySplit, preview]() { return applySplit(preview); },
            finishApply,
            [this]() { restoreSeededSplitFocus(); });
    } else {
        finishApply(applySplit(preview));
        restoreSeededSplitFocus();
    }
}

void Segment3DViewerDialog::setSeededSplitStatus(
    const QString &text,
    bool warning)
{
    if (m_seedStatusLabel == nullptr) {
        return;
    }
    m_seedStatusLabel->setText(text);
    m_seedStatusLabel->setStyleSheet(
        warning ? QStringLiteral("color: #e69f00; font-weight: 600;")
                : QString{});
}

void Segment3DViewerDialog::updateSeededSplitUiState() {
    if (m_seedButtons[0] == nullptr) {
        return;
    }
    for (int seedNumber = 0; seedNumber < static_cast<int>(m_seedButtons.size()); ++seedNumber) {
        m_seedButtons[seedNumber]->setEnabled(!m_seededSplitBusy);
        m_seedButtons[seedNumber]->setChecked(m_activeSeed == seedNumber);
        const QString color = seedNumber == 0
                                  ? QStringLiteral("Red")
                                  : QStringLiteral("Blue");
        m_seedButtons[seedNumber]->setText(
            m_activeSeed == seedNumber
                ? QStringLiteral("Adding %1 Seed").arg(color)
                : QStringLiteral("%1 Seed").arg(color));
    }
    const bool haveSeeds = !m_seedIndices[0].empty() && !m_seedIndices[1].empty();
    if (m_strokeOverlay != nullptr) {
        m_strokeOverlay->setDrawingEnabled(
            m_splitLineDrawModeActive && !m_seededSplitBusy
            && !m_havePendingLineSeeds
            && !m_seededSplitPreview.has_value());
    }
    if (m_splitLineButton != nullptr) {
        m_splitLineButton->setEnabled(!m_seededSplitBusy);
        m_splitLineButton->setChecked(m_splitLineDrawModeActive);
        m_splitLineButton->setText(
            m_havePendingLineSeeds
                ? QStringLiteral("Redraw Line")
                : QStringLiteral("Split Line"));
    }
    if (m_seedDistanceSlider != nullptr) {
        m_seedDistanceSlider->setEnabled(
            m_splitLineDrawModeActive && !m_seededSplitBusy);
    }
    if (m_seedDistanceLabel != nullptr) {
        m_seedDistanceLabel->setEnabled(
            m_splitLineDrawModeActive && !m_seededSplitBusy);
    }
    if (m_lineSamplingSlider != nullptr) {
        m_lineSamplingSlider->setEnabled(
            m_splitLineDrawModeActive && !m_seededSplitBusy);
    }
    if (m_lineSamplingLabel != nullptr) {
        m_lineSamplingLabel->setEnabled(
            m_splitLineDrawModeActive && !m_seededSplitBusy);
    }
    if (m_confirmLineSeedsButton != nullptr) {
        m_confirmLineSeedsButton->setEnabled(
            !m_seededSplitBusy && m_splitLineDrawModeActive
            && m_havePendingLineSeeds && m_pendingLineSeedsValid);
    }
    if (m_autoPreviewCheckBox != nullptr) {
        m_autoPreviewCheckBox->setEnabled(!m_seededSplitBusy);
    }
    if (m_connectSeedsCheckBox != nullptr) {
        m_connectSeedsCheckBox->setEnabled(!m_seededSplitBusy);
    }
    if (m_compactWatershedCheckBox != nullptr) {
        m_compactWatershedCheckBox->setEnabled(!m_seededSplitBusy);
    }
    if (m_allowDisconnectedCheckBox != nullptr) {
        m_allowDisconnectedCheckBox->setEnabled(!m_seededSplitBusy);
    }
    if (m_smoothingSlider != nullptr) {
        m_smoothingSlider->setEnabled(!m_seededSplitBusy);
    }
    if (m_smoothingLabel != nullptr) {
        m_smoothingLabel->setEnabled(!m_seededSplitBusy);
    }
    m_clearSeedsButton->setEnabled(!m_seededSplitBusy
                                   && (!m_seedIndices[0].empty()
                                       || !m_seedIndices[1].empty()
                                       || m_splitLineDrawModeActive
                                       || m_havePendingLineSeeds
                                       || m_seededSplitPreview.has_value()));
    m_previewSplitButton->setEnabled(!m_seededSplitBusy
                                     && !m_seededSplitSmoothingPending
                                     && !m_splitLineDrawModeActive
                                     && haveSeeds);
    m_applySplitButton->setEnabled(!m_seededSplitBusy
                                   && !m_seededSplitSmoothingPending
                                   && m_seededSplitPreview.has_value());
}

void Segment3DViewerDialog::restoreSeededSplitFocus() {
    m_seededSplitBusy = false;
    updateSeededSplitUiState();
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible()) {
            return;
        }
        raiseAndRequestActivation();
        if (m_vtkWidget != nullptr) {
            m_vtkWidget->setFocus(Qt::OtherFocusReason);
        }
    });
}

bool Segment3DViewerDialog::tryHandlePickedLabelInteraction(
    int pickX,
    int pickY,
    Qt::KeyboardModifiers modifiers,
    const char *sourceTag)
{
    if (placeSeedAt(pickX, pickY)) {
        return true;
    }

    QElapsedTimer totalTimer;
    totalTimer.start();

    const Qt::KeyboardModifiers effectiveModifiers = modifiers | QApplication::keyboardModifiers();
    const bool hasNavigateHandler = static_cast<bool>(m_navigateToLabelHandler);
    const bool modifierActive = navigationModifierPressed(effectiveModifiers);
    const bool deleteActive = m_deleteModeActive && static_cast<bool>(m_deleteLabelHandler);
    const char *result = "skipped";

    if (!deleteActive && !hasNavigateHandler) {
        result = "skipped_no_handler";
    } else if (!deleteActive && !modifierActive) {
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
        bool interactionHandled = deleteActive;
        if (pickedLabelId != 0) {
            QElapsedTimer dispatchTimer;
            dispatchTimer.start();
            if (deleteActive) {
                if (m_deleteLabelHandler(pickedLabelId)
                    && removeLabelActor(pickedLabelId)) {
                    result = "deleted";
                } else {
                    result = "delete_rejected";
                }
            } else {
                m_navigateToLabelHandler(pickedLabelId);
                interactionHandled = true;
                result = "navigated";
            }
            dispatchNanoseconds = dispatchTimer.nsecsElapsed();
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

        return interactionHandled;
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
    tryHandlePickedLabelInteraction(eventPosition[0],
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
    const bool cutEnabled = m_strokeOverlay != nullptr && static_cast<bool>(m_cutSession.applyCut);
    if (!cutEnabled) {
        return;
    }

    const bool hasStroke = m_strokeOverlay->hasValidStroke();
    m_strokeOverlay->setDrawingEnabled(m_cutDrawModeActive && !m_cutApplyInFlight);

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
