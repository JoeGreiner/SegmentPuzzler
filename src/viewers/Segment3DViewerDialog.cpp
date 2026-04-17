#include "Segment3DViewerDialog.h"

#include <QAbstractSlider>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QShortcut>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include "src/qtUtils/TaskRunner.h"
#include <QVTKOpenGLNativeWidget.h>

#include <vtkSmartPointer.h>
#include <vtkVersion.h>
#include <vtkType.h>
#include <vtkGenericOpenGLRenderWindow.h>
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
#include <vtkDataArray.h>
#include <vtkUnsignedCharArray.h>
#include <vtkAxesActor.h>
#include <vtkAlgorithmOutput.h>
#include <vtkOrientationMarkerWidget.h>

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 2, 0)
#include <vtkSurfaceNets3D.h>
#else
#include <vtkFlyingEdges3D.h>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#ifdef USE_OMP
#include <omp.h>
#endif

#include "src/utils/utils.h"

namespace {

constexpr bool kLog3DViewSplitDetails = false;
constexpr bool kProfile3DViewExtraction = true;
constexpr vtkIdType kParallelScanVoxelThreshold = 1'000'000;

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

struct ColorLookup {
    dataType::SegmentIdType maxLabelId = 0;
    bool useDense = false;
    std::vector<quint32> denseColors;
    std::unordered_map<dataType::SegmentIdType, quint32> sparseColors;

    quint32 colorFor(dataType::SegmentIdType labelId, quint32 fallbackColor) const {
        if (labelId == 0) {
            return fallbackColor;
        }
        if (useDense) {
            if (labelId <= maxLabelId) {
                const auto color = denseColors[static_cast<std::size_t>(labelId)];
                return color == 0 ? fallbackColor : color;
            }
            return fallbackColor;
        }
        const auto it = sparseColors.find(labelId);
        return it == sparseColors.end() ? fallbackColor : it->second;
    }
};

LabelMembershipLookup buildRequestedLabelLookup(
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    LabelMembershipLookup lookup;
    for (const auto &[labelId, lutColor] : labels) {
        lookup.maxLabelId = std::max(lookup.maxLabelId, labelId);
    }

    constexpr dataType::SegmentIdType kDenseLookupLimit = 1'000'000;
    lookup.useDense = lookup.maxLabelId <= kDenseLookupLimit;
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

ColorLookup buildColorLookup(const std::unordered_map<dataType::SegmentIdType, quint32> &colorByLabel)
{
    ColorLookup lookup;
    for (const auto &[labelId, lutColor] : colorByLabel) {
        lookup.maxLabelId = std::max(lookup.maxLabelId, labelId);
    }

    constexpr dataType::SegmentIdType kDenseLookupLimit = 1'000'000;
    lookup.useDense = lookup.maxLabelId <= kDenseLookupLimit;
    if (lookup.useDense) {
        lookup.denseColors.assign(static_cast<std::size_t>(lookup.maxLabelId) + 1, 0);
        for (const auto &[labelId, lutColor] : colorByLabel) {
            lookup.denseColors[static_cast<std::size_t>(labelId)] = lutColor;
        }
    } else {
        lookup.sparseColors = colorByLabel;
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

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 2, 0)
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

vtkSmartPointer<vtkSurfaceNets3D> createSelectedSurfaceNet(
    vtkImageData *inputData,
    const std::vector<dataType::SegmentIdType> &surfaceNetLabels)
{
    if (inputData == nullptr || surfaceNetLabels.empty()) {
        return nullptr;
    }

    auto surfaceNet = vtkSmartPointer<vtkSurfaceNets3D>::New();
    surfaceNet->SetInputData(inputData);
    configureSurfaceNet(surfaceNet);
    surfaceNet->DataCachingOn();
    surfaceNet->SetOutputStyleToSelected();
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
    mesh.useCellScalars = false;
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
    auto surfaceNet = createSelectedSurfaceNet(inputData, surfaceNetLabels);
    auto polyData = extractSelectedSurfaceNetOutput(surfaceNet, {selectedLabel});
    if (logMesh) {
        std::cout << "[3DView] single-label selected mesh  labelsInVOI=" << surfaceNetLabels.size()
                  << "  points=" << polyData->GetNumberOfPoints()
                  << "  cells=" << polyData->GetNumberOfCells() << std::endl;
    }

    return makePreparedMesh(selectedLabel, lutColor, polyData);
}
#endif

std::vector<Segment3DViewerDialog::PreparedMesh> splitMultiLabelMesh(
    vtkPolyData *combinedPolyData,
    vtkDataArray *boundaryLabels,
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    struct MeshBuilder {
        vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
        vtkSmartPointer<vtkCellArray> polys = vtkSmartPointer<vtkCellArray>::New();
        std::unordered_map<vtkIdType, vtkIdType> pointMap;
        vtkIdType assignedCells = 0;
    };

    std::vector<Segment3DViewerDialog::PreparedMesh> preparedMeshes;
    if (combinedPolyData == nullptr || boundaryLabels == nullptr) {
        return preparedMeshes;
    }

    const double t_splitTotal = kProfile3DViewExtraction ? utils::tic() : 0.0;
    if (kLog3DViewSplitDetails) {
        std::cout << "[3DView] splitMultiLabelMesh entered"
                  << " cells=" << combinedPolyData->GetNumberOfCells()
                  << " points=" << combinedPolyData->GetNumberOfPoints()
                  << " boundaryTuples=" << boundaryLabels->GetNumberOfTuples()
                  << std::endl;
    }

    combinedPolyData->BuildCells();
    dataType::SegmentIdType maxLabelId = 0;
    for (const auto &[labelId, lutColor] : labels) {
        maxLabelId = std::max(maxLabelId, labelId);
    }

    constexpr dataType::SegmentIdType kDenseLookupLimit = 1'000'000;
    const bool useDenseLookup = maxLabelId <= kDenseLookupLimit;
    std::vector<int> denseLookup;
    std::unordered_map<dataType::SegmentIdType, std::size_t> sparseLookup;
    if (useDenseLookup) {
        denseLookup.assign(static_cast<std::size_t>(maxLabelId) + 1, -1);
        for (std::size_t labelIndex = 0; labelIndex < labels.size(); ++labelIndex) {
            denseLookup[labels[labelIndex].first] = static_cast<int>(labelIndex);
        }
    } else {
        sparseLookup.reserve(labels.size());
        for (std::size_t labelIndex = 0; labelIndex < labels.size(); ++labelIndex) {
            sparseLookup.emplace(labels[labelIndex].first, labelIndex);
        }
    }

    const dataType::SegmentIdType *boundaryLabelValues =
        boundaryLabels->GetNumberOfComponents() == 2
        && boundaryLabels->GetDataType() == segmentIdVtkDataType()
        && boundaryLabels->HasStandardMemoryLayout()
            ? static_cast<const dataType::SegmentIdType *>(boundaryLabels->GetVoidPointer(0))
            : nullptr;
    if (kLog3DViewSplitDetails) {
        std::cout << "[3DView] split boundary storage"
                  << " dataType=" << boundaryLabels->GetDataType()
                  << " standardLayout=" << boundaryLabels->HasStandardMemoryLayout()
                  << " typedAccess=" << (boundaryLabelValues != nullptr)
                  << " labelLookup=" << (useDenseLookup ? "dense" : "sparse")
                  << std::endl;
    }

    const auto resolveLabelIndex = [&](dataType::SegmentIdType labelId) -> int {
        if (labelId == 0) {
            return -1;
        }
        if (useDenseLookup) {
            return labelId <= maxLabelId ? denseLookup[labelId] : -1;
        }
        const auto sparseIt = sparseLookup.find(labelId);
        return sparseIt == sparseLookup.end() ? -1 : static_cast<int>(sparseIt->second);
    };

    const double t_splitCount = kProfile3DViewExtraction ? utils::tic() : 0.0;
    std::vector<vtkIdType> assignedCellCounts(labels.size(), 0);
    vtkIdType singleSidedCells = 0;
    vtkIdType sharedCells = 0;
    for (vtkIdType cellId = 0; cellId < combinedPolyData->GetNumberOfCells(); ++cellId) {
        dataType::SegmentIdType a = 0;
        dataType::SegmentIdType b = 0;
        if (boundaryLabelValues != nullptr) {
            const auto tupleIndex = 2 * cellId;
            a = boundaryLabelValues[tupleIndex];
            b = boundaryLabelValues[tupleIndex + 1];
        } else {
            a = static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, 0));
            b = static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, 1));
        }

        const int builderAIndex = resolveLabelIndex(a);
        const int builderBIndex = (b == a) ? -1 : resolveLabelIndex(b);
        if (builderAIndex >= 0) {
            ++assignedCellCounts[static_cast<std::size_t>(builderAIndex)];
        }
        if (builderBIndex >= 0) {
            ++assignedCellCounts[static_cast<std::size_t>(builderBIndex)];
        }
    }
    if (kProfile3DViewExtraction && kLog3DViewSplitDetails) {
        utils::toc(t_splitCount, "[3DView] [segmentpuzzler] split cell count:");
    }

    std::vector<MeshBuilder> builders(labels.size());
    vtkPoints *sourcePoints = combinedPolyData->GetPoints();
    const auto sourcePointCount = sourcePoints != nullptr ? sourcePoints->GetNumberOfPoints() : 0;
    for (std::size_t labelIndex = 0; labelIndex < labels.size(); ++labelIndex) {
        auto &builder = builders[labelIndex];
        const auto assignedCellCount = assignedCellCounts[labelIndex];
        builder.assignedCells = assignedCellCount;
        if (assignedCellCount == 0) {
            continue;
        }

        builder.points->SetDataType(sourcePoints->GetDataType());
        const vtkIdType estimatedPointCount =
            std::min<vtkIdType>(sourcePointCount, std::max<vtkIdType>(assignedCellCount, assignedCellCount * 2));
        builder.points->Reserve(estimatedPointCount);
        builder.polys->AllocateEstimate(assignedCellCount, 3);
        builder.pointMap.reserve(static_cast<std::size_t>(estimatedPointCount));
    }

    const double t_splitAssign = kProfile3DViewExtraction ? utils::tic() : 0.0;
    double point[3];
    for (vtkIdType cellId = 0; cellId < combinedPolyData->GetNumberOfCells(); ++cellId) {
        dataType::SegmentIdType a = 0;
        dataType::SegmentIdType b = 0;
        if (boundaryLabelValues != nullptr) {
            const auto tupleIndex = 2 * cellId;
            a = boundaryLabelValues[tupleIndex];
            b = boundaryLabelValues[tupleIndex + 1];
        } else {
            a = static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, 0));
            b = static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, 1));
        }

        if (a == 0 || b == 0) {
            ++singleSidedCells;
        } else {
            ++sharedCells;
        }

        const int builderAIndex = resolveLabelIndex(a);
        const int builderBIndex = (b == a) ? -1 : resolveLabelIndex(b);
        if (builderAIndex < 0 && builderBIndex < 0) {
            continue;
        }

        vtkIdType npts = 0;
        vtkIdType const *pts = nullptr;
        combinedPolyData->GetCellPoints(cellId, npts, pts);
        auto appendCellToBuilder = [&](MeshBuilder &builder) {
            builder.polys->InsertNextCell(npts);
            for (vtkIdType i = 0; i < npts; ++i) {
                const vtkIdType sourcePointId = pts[i];
                auto [mappedPointIt, inserted] = builder.pointMap.emplace(sourcePointId, -1);
                if (inserted) {
                    sourcePoints->GetPoint(sourcePointId, point);
                    mappedPointIt->second = builder.points->InsertNextPoint(point);
                }
                builder.polys->InsertCellPoint(mappedPointIt->second);
            }
        };

        if (builderAIndex >= 0) {
            appendCellToBuilder(builders[static_cast<std::size_t>(builderAIndex)]);
        }
        if (builderBIndex >= 0) {
            appendCellToBuilder(builders[static_cast<std::size_t>(builderBIndex)]);
        }
    }

    if (kLog3DViewSplitDetails) {
        std::cout << "[3DView] split cell assignment  singleSided=" << singleSidedCells
                  << "  shared=" << sharedCells << std::endl;
    }
    if (kProfile3DViewExtraction && kLog3DViewSplitDetails) {
        utils::toc(t_splitAssign, "[3DView] [segmentpuzzler] split cell assignment:");
    }

    preparedMeshes.reserve(builders.size());
    const double t_splitBuildMeshes = kProfile3DViewExtraction ? utils::tic() : 0.0;
    int loggedLabels = 0;
    for (std::size_t labelIndex = 0; labelIndex < labels.size(); ++labelIndex) {
        const auto &[labelId, lutColor] = labels[labelIndex];
        auto &builder = builders[labelIndex];
        const auto assignedCellCount = builder.assignedCells;
        if (kLog3DViewSplitDetails && loggedLabels < 8) {
            std::cout << "[3DView] split label " << labelId
                      << " assignedCells=" << assignedCellCount << std::endl;
            ++loggedLabels;
        }

        if (assignedCellCount == 0
            || builder.points->GetNumberOfPoints() == 0
            || builder.polys->GetNumberOfCells() == 0) {
            continue;
        }

        auto polyData = vtkSmartPointer<vtkPolyData>::New();
        polyData->SetPoints(builder.points);
        polyData->SetPolys(builder.polys);
        polyData->Squeeze();

        if (polyData->GetNumberOfPoints() == 0 || polyData->GetNumberOfCells() == 0) {
            if (kLog3DViewSplitDetails) {
                std::cout << "[3DView] split label " << labelId
                          << " extraction empty after remap"
                          << " assignedCells=" << assignedCellCount << std::endl;
            }
            continue;
        }

        Segment3DViewerDialog::PreparedMesh mesh;
        mesh.labelId = labelId;
        mesh.polyData = polyData;
        mesh.lutColor = lutColor;
        mesh.useCellScalars = false;
        mesh.centerWorld = centerFromBounds(polyData);
        preparedMeshes.push_back(std::move(mesh));
    }

    if (kLog3DViewSplitDetails) {
        std::cout << "[3DView] split produced meshes=" << preparedMeshes.size() << std::endl;
    }
    if (kProfile3DViewExtraction && kLog3DViewSplitDetails) {
        utils::toc(t_splitBuildMeshes, "[3DView] [segmentpuzzler] split mesh build:");
    }
    if (kProfile3DViewExtraction) {
        utils::toc(t_splitTotal, "[3DView] [segmentpuzzler] split multi-label mesh:");
    }

    return preparedMeshes;
}

void logBoundaryLabelsSummary(
    vtkDataArray *boundaryLabels,
    const std::vector<Segment3DViewerDialog::LabelWithColor> &labels)
{
    if (boundaryLabels == nullptr) {
        std::cout << "[3DView] boundary labels array missing" << std::endl;
        return;
    }

    const auto requestedLookup = buildRequestedLabelLookup(labels);
    const dataType::SegmentIdType *boundaryLabelValues =
        boundaryLabels->GetNumberOfComponents() == 2
        && boundaryLabels->GetDataType() == segmentIdVtkDataType()
        && boundaryLabels->HasStandardMemoryLayout()
            ? static_cast<const dataType::SegmentIdType *>(boundaryLabels->GetVoidPointer(0))
            : nullptr;

    std::unordered_set<dataType::SegmentIdType> observedLabels;
    observedLabels.reserve(static_cast<std::size_t>(labels.size()) + 1);
    vtkIdType matchedEntries = 0;
    for (vtkIdType cellId = 0; cellId < boundaryLabels->GetNumberOfTuples(); ++cellId) {
        for (int component = 0; component < boundaryLabels->GetNumberOfComponents(); ++component) {
            const auto labelId = boundaryLabelValues != nullptr
                                 ? boundaryLabelValues[2 * cellId + component]
                                 : static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, component));
            observedLabels.insert(labelId);
            if (requestedLookup.contains(labelId)) {
                ++matchedEntries;
            }
        }
    }

    std::cout << "[3DView] boundary labels tuples=" << boundaryLabels->GetNumberOfTuples()
              << " comps=" << boundaryLabels->GetNumberOfComponents()
              << " observed=" << observedLabels.size()
              << " requestedMatches=" << matchedEntries << std::endl;
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

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 2, 0)
        auto surfaceNet = vtkSmartPointer<vtkSurfaceNets3D>::New();
        surfaceNet->SetInputConnection(threshold->GetOutputPort());
        configureSurfaceNet(surfaceNet);
        surfaceNet->SetNumberOfLabels(1);
        surfaceNet->SetLabel(0, 1.0);
        surfaceNet->Update();
        auto polyData = detachPolyData(surfaceNet->GetOutput());
#else
        auto flyingEdges = vtkSmartPointer<vtkFlyingEdges3D>::New();
        flyingEdges->SetInputConnection(threshold->GetOutputPort());
        flyingEdges->SetValue(0, 0.5);
        flyingEdges->Update();
        auto polyData = detachPolyData(flyingEdges->GetOutput());
#endif

        if (logPerLabel) {
            std::cout << "[3DView] label " << labelId
                      << "  points=" << polyData->GetNumberOfPoints()
                      << "  cells="  << polyData->GetNumberOfCells() << std::endl;
        }

        if (polyData->GetNumberOfPoints() == 0 || polyData->GetNumberOfCells() == 0) {
            continue;
        }

        Segment3DViewerDialog::PreparedMesh mesh;
        mesh.labelId = labelId;
        mesh.polyData = polyData;
        mesh.lutColor = lutColor;
        mesh.useCellScalars = false;
        mesh.centerWorld = centerFromBounds(polyData);
        preparedMeshes.push_back(std::move(mesh));
    }

    return preparedMeshes;
}

bool setCombinedMeshFromPolyData(
    vtkPolyData *polyData,
    const std::unordered_map<dataType::SegmentIdType, quint32> &colorByLabel,
    Segment3DViewerDialog::PreparedScene &preparedScene)
{
    if (polyData == nullptr || polyData->GetNumberOfPoints() == 0 || polyData->GetNumberOfCells() == 0) {
        std::cout << "[3DView] combined mesh rejected"
                  << " points=" << (polyData == nullptr ? -1 : polyData->GetNumberOfPoints())
                  << " cells=" << (polyData == nullptr ? -1 : polyData->GetNumberOfCells()) << std::endl;
        return false;
    }

    preparedScene.sceneCenterWorld = centerFromBounds(polyData);
    {
        double bounds[6];
        polyData->GetBounds(bounds);
        const double dx = bounds[1] - bounds[0];
        const double dy = bounds[3] - bounds[2];
        const double dz = bounds[5] - bounds[4];
        preparedScene.sceneExtent = std::max({dx, dy, dz, 1.0});
    }

    auto *boundaryLabels = polyData->GetCellData()->GetArray("BoundaryLabels");
    bool useCellScalars = false;
    if (boundaryLabels != nullptr && boundaryLabels->GetNumberOfComponents() >= 2) {
        const auto colorLookup = buildColorLookup(colorByLabel);
        const dataType::SegmentIdType *boundaryLabelValues =
            boundaryLabels->GetNumberOfComponents() == 2
            && boundaryLabels->GetDataType() == segmentIdVtkDataType()
            && boundaryLabels->HasStandardMemoryLayout()
                ? static_cast<const dataType::SegmentIdType *>(boundaryLabels->GetVoidPointer(0))
                : nullptr;

        auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
        colors->SetName("Colors");
        colors->SetNumberOfComponents(3);
        colors->SetNumberOfTuples(polyData->GetNumberOfCells());
        auto *colorValues = colors->WritePointer(0, 3 * polyData->GetNumberOfCells());

        for (vtkIdType cellId = 0; cellId < polyData->GetNumberOfCells(); ++cellId) {
            dataType::SegmentIdType a = 0;
            dataType::SegmentIdType b = 0;
            if (boundaryLabelValues != nullptr) {
                const auto tupleIndex = 2 * cellId;
                a = boundaryLabelValues[tupleIndex];
                b = boundaryLabelValues[tupleIndex + 1];
            } else {
                a = static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, 0));
                b = static_cast<dataType::SegmentIdType>(boundaryLabels->GetComponent(cellId, 1));
            }

            dataType::SegmentIdType displayLabel = 0;
            if (a == 0) {
                displayLabel = b;
            } else if (b == 0) {
                displayLabel = a;
            } else {
                displayLabel = a;
            }

            const quint32 lutColor = colorLookup.colorFor(displayLabel, 0xAAAAAA);
            const vtkIdType colorIndex = 3 * cellId;
            colorValues[colorIndex] = static_cast<unsigned char>((lutColor >> 16) & 0xFF);
            colorValues[colorIndex + 1] = static_cast<unsigned char>((lutColor >> 8) & 0xFF);
            colorValues[colorIndex + 2] = static_cast<unsigned char>(lutColor & 0xFF);
        }
        polyData->GetCellData()->SetScalars(colors);
        useCellScalars = true;
    }

    preparedScene.combinedMesh.labelId = 0;
    preparedScene.combinedMesh.polyData = polyData;
    preparedScene.combinedMesh.lutColor = 0xAAAAAA;
    preparedScene.combinedMesh.useCellScalars = useCellScalars;
    preparedScene.combinedMesh.centerWorld = preparedScene.sceneCenterWorld;
    preparedScene.hasCombinedMesh = true;
    std::cout << "[3DView] combined mesh accepted"
              << " points=" << polyData->GetNumberOfPoints()
              << " cells=" << polyData->GetNumberOfCells()
              << " useCellScalars=" << useCellScalars << std::endl;
    return true;
}

void updateSceneBoundsFromMeshes(Segment3DViewerDialog::PreparedScene &preparedScene) {
    if (preparedScene.meshes.empty()) {
        return;
    }

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    double minZ = std::numeric_limits<double>::max();
    double maxZ = std::numeric_limits<double>::lowest();
    for (const auto &mesh : preparedScene.meshes) {
        if (mesh.polyData == nullptr || mesh.polyData->GetNumberOfPoints() == 0 || mesh.polyData->GetNumberOfCells() == 0) {
            continue;
        }

        double bounds[6];
        mesh.polyData->GetBounds(bounds);
        minX = std::min(minX, bounds[0]);
        maxX = std::max(maxX, bounds[1]);
        minY = std::min(minY, bounds[2]);
        maxY = std::max(maxY, bounds[3]);
        minZ = std::min(minZ, bounds[4]);
        maxZ = std::max(maxZ, bounds[5]);
    }

    preparedScene.sceneCenterWorld = {0.5 * (minX + maxX), 0.5 * (minY + maxY), 0.5 * (minZ + maxZ)};
    preparedScene.sceneExtent = std::max({maxX - minX, maxY - minY, maxZ - minZ, 1.0});
}

}

Segment3DViewerDialog::PreparedScene Segment3DViewerDialog::prepareScene(
    dataType::SegmentsImageType::Pointer segImage,
    std::vector<LabelWithColor> labels)
{
    PreparedScene preparedScene;
    if (labels.size() == 1) {
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

    std::cout << "[3DView] image dims=" << dimX << "x" << dimY << "x" << dimZ
              << "  spacing=" << spacing[0] << "," << spacing[1] << "," << spacing[2]
              << "  origin=" << origin[0] << "," << origin[1] << "," << origin[2]
              << "  labels=" << labels.size() << std::endl;

    const double t_total = utils::tic();
    const auto requestedLabelLookup = buildRequestedLabelLookup(labels);

    const double t_bbox = utils::tic();
    const auto bounds = scanBoundsForRequestedLabels(buf, dimX, dimY, dimZ, requestedLabelLookup);
    const int minX = bounds.minX;
    const int maxX = bounds.maxX;
    const int minY = bounds.minY;
    const int maxY = bounds.maxY;
    const int minZ = bounds.minZ;
    const int maxZ = bounds.maxZ;
    utils::toc(t_bbox, "[3DView] [segmentpuzzler] bbox scan:");

    if (maxX < 0) {
        std::cout << "[3DView] no requested labels found in image — no mesh" << std::endl;
        utils::toc(t_total, "[3DView] total:");
        return preparedScene;
    }

    const int padX0 = std::max(0, minX - 1), padX1 = std::min(dimX - 1, maxX + 1);
    const int padY0 = std::max(0, minY - 1), padY1 = std::min(dimY - 1, maxY + 1);
    const int padZ0 = std::max(0, minZ - 1), padZ1 = std::min(dimZ - 1, maxZ + 1);

    std::cout << "[3DView] union bbox  x=[" << minX << "," << maxX << "]"
              << "  y=[" << minY << "," << maxY << "]"
              << "  z=[" << minZ << "," << maxZ << "]" << std::endl;

    const double t_voi = utils::tic();
    auto importer = vtkSmartPointer<vtkImageImport>::New();
    importer->SetImportVoidPointer(const_cast<dataType::SegmentIdType *>(buf));
    importer->SetDataScalarType(VTK_UNSIGNED_INT);
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
    utils::toc(t_voi, "[3DView] [segmentpuzzler] VOI extract + pad:");

    const double t_surfaces = utils::tic();
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 2, 0)
    if (labels.size() == 1) {
        std::cout << "[3DView] using vtkSurfaceNets3D selected-label extraction" << std::endl;

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
    } else if (labels.size() > 1) {
        std::cout << "[3DView] using vtkSurfaceNets3D cached multi-label extraction" << std::endl;

        std::unordered_map<dataType::SegmentIdType, quint32> colorByLabel;
        for (const auto &[labelId, lutColor] : labels) {
            colorByLabel[labelId] = lutColor;
        }

        const double t_collectLabels = kProfile3DViewExtraction ? utils::tic() : 0.0;
        auto surfaceNetLabels = collectLabelsInExtent(
            buf, dimX, dimY, padX0, padX1, padY0, padY1, padZ0, padZ1);
        auto requestedLabels = collectRequestedLabels(labels);
        if (kProfile3DViewExtraction) {
            utils::toc(t_collectLabels, "[3DView] [segmentpuzzler] collect labels in VOI:");
        }

        const double t_selectedExtraction = kProfile3DViewExtraction ? utils::tic() : 0.0;
        auto selectedSurfaceNet = createSelectedSurfaceNet(paddedImage, surfaceNetLabels);
        auto combinedPolyData = extractSelectedSurfaceNetOutput(selectedSurfaceNet, requestedLabels);
        if (kProfile3DViewExtraction) {
            utils::toc(t_selectedExtraction, "[3DView] [vtksurfacenets] selected extraction:");
        }
        std::cout << "[3DView] multi-label selected mesh  labelsInVOI=" << surfaceNetLabels.size()
                  << "  requested=" << requestedLabels.size()
                  << "  points=" << combinedPolyData->GetNumberOfPoints()
                  << "  cells=" << combinedPolyData->GetNumberOfCells() << std::endl;

        const double t_boundarySummary = kProfile3DViewExtraction ? utils::tic() : 0.0;
        auto *selectedBoundaryLabels = combinedPolyData->GetCellData()->GetArray("BoundaryLabels");
        logBoundaryLabelsSummary(selectedBoundaryLabels, labels);
        if (kProfile3DViewExtraction) {
            utils::toc(t_boundarySummary, "[3DView] [segmentpuzzler] boundary summary:");
        }

        const bool canSplitSelected =
            selectedBoundaryLabels != nullptr && selectedBoundaryLabels->GetNumberOfComponents() >= 2;
        if (!canSplitSelected) {
            std::cout << "[3DView] selected multi-label split unavailable (no BoundaryLabels)" << std::endl;
        }

        // Shallow-copy before setCombinedMeshFromPolyData, which calls SetScalars() and
        // clobbers the cell data array list in place (BoundaryLabels becomes unreachable by name).
        vtkSmartPointer<vtkPolyData> splitPolyData;
        if (canSplitSelected) {
            splitPolyData = vtkSmartPointer<vtkPolyData>::New();
            splitPolyData->ShallowCopy(combinedPolyData);
        }

        const double t_selectedCombinedMesh = kProfile3DViewExtraction ? utils::tic() : 0.0;
        const bool combinedSelectedOk = setCombinedMeshFromPolyData(combinedPolyData, colorByLabel, preparedScene);
        if (kProfile3DViewExtraction) {
            utils::toc(t_selectedCombinedMesh, "[3DView] [segmentpuzzler] combined mesh colors:");
        }
        if (combinedSelectedOk && canSplitSelected) {
            preparedScene.splitSourcePolyData = splitPolyData;
            preparedScene.splitLabels = labels;
        }

        if (!preparedScene.hasCombinedMesh) {
            std::cout << "[3DView] selected combined mesh failed, falling back to legacy multi-label path" << std::endl;

            auto surfaceNet = vtkSmartPointer<vtkSurfaceNets3D>::New();
            surfaceNet->SetInputData(paddedImage);
            configureSurfaceNet(surfaceNet);
            surfaceNet->SetOutputStyleToDefault();
            setSurfaceNetLabels(surfaceNet, requestedLabels);
            const double t_legacyExtraction = kProfile3DViewExtraction ? utils::tic() : 0.0;
            surfaceNet->Update();
            auto polyData = detachPolyData(surfaceNet->GetOutput());
            if (kProfile3DViewExtraction) {
                utils::toc(t_legacyExtraction, "[3DView] [vtksurfacenets] legacy extraction:");
            }
            std::cout << "[3DView] legacy multi-label mesh  points=" << polyData->GetNumberOfPoints()
                      << "  cells=" << polyData->GetNumberOfCells() << std::endl;

            const double t_legacyBoundarySummary = kProfile3DViewExtraction ? utils::tic() : 0.0;
            auto *boundaryLabels = polyData->GetCellData()->GetArray("BoundaryLabels");
            logBoundaryLabelsSummary(boundaryLabels, labels);
            if (kProfile3DViewExtraction) {
                utils::toc(t_legacyBoundarySummary, "[3DView] [segmentpuzzler] legacy boundary summary:");
            }
            const bool canSplitLegacy =
                boundaryLabels != nullptr && boundaryLabels->GetNumberOfComponents() >= 2;
            if (!canSplitLegacy) {
                std::cout << "[3DView] legacy multi-label split unavailable" << std::endl;
            }

            vtkSmartPointer<vtkPolyData> legacySplitPolyData;
            if (canSplitLegacy) {
                legacySplitPolyData = vtkSmartPointer<vtkPolyData>::New();
                legacySplitPolyData->ShallowCopy(polyData);
            }

            const double t_legacyCombinedMesh = kProfile3DViewExtraction ? utils::tic() : 0.0;
            const bool combinedLegacyOk = setCombinedMeshFromPolyData(polyData, colorByLabel, preparedScene);
            if (kProfile3DViewExtraction) {
                utils::toc(t_legacyCombinedMesh, "[3DView] [segmentpuzzler] legacy combined mesh colors:");
            }
            if (combinedLegacyOk && canSplitLegacy) {
                preparedScene.splitSourcePolyData = legacySplitPolyData;
                preparedScene.splitLabels = labels;
            }

            if (!preparedScene.hasCombinedMesh) {
                std::cout << "[3DView] legacy combined mesh also failed, falling back to per-label extraction" << std::endl;
                preparedScene.meshes = extractSingleLabelMeshes(paddedImage, labels, false);
            }
        }
    } else
#endif
    {
        preparedScene.meshes = extractSingleLabelMeshes(paddedImage, labels, labels.size() == 1);
    }
    if (!preparedScene.meshes.empty() && !preparedScene.hasCombinedMesh) {
        updateSceneBoundsFromMeshes(preparedScene);
    }
    utils::toc(t_surfaces, "[3DView] [total] surface extraction:");
    utils::toc(t_total, "[3DView] [total] total:");

    return preparedScene;
}

Segment3DViewerDialog::Segment3DViewerDialog(PreparedScene preparedScene,
                                             QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(preparedScene.windowTitle);
    resize(600, 600);

    auto renWin = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    m_renderer = vtkSmartPointer<vtkRenderer>::New();
    m_renderer->SetBackground(0.1, 0.1, 0.1);
    renWin->AddRenderer(m_renderer);

    if (preparedScene.hasCombinedMesh && preparedScene.combinedMesh.polyData != nullptr
        && preparedScene.combinedMesh.polyData->GetNumberOfPoints() > 0
        && preparedScene.combinedMesh.polyData->GetNumberOfCells() > 0) {
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(preparedScene.combinedMesh.polyData);
        if (preparedScene.combinedMesh.useCellScalars) {
            mapper->SetScalarModeToUseCellData();
            mapper->ScalarVisibilityOn();
        } else {
            mapper->ScalarVisibilityOff();
        }

        m_combinedActor = vtkSmartPointer<vtkActor>::New();
        m_combinedActor->SetMapper(mapper);
        if (!preparedScene.combinedMesh.useCellScalars) {
            const double cr = ((preparedScene.combinedMesh.lutColor >> 16) & 0xFF) / 255.0;
            const double cg = ((preparedScene.combinedMesh.lutColor >> 8) & 0xFF) / 255.0;
            const double cb = (preparedScene.combinedMesh.lutColor & 0xFF) / 255.0;
            m_combinedActor->GetProperty()->SetColor(cr, cg, cb);
        }
        m_combinedActor->GetProperty()->SetAmbient(0.1);
        m_combinedActor->GetProperty()->SetDiffuse(0.7);
        m_combinedActor->GetProperty()->SetSpecular(0.3);
        m_combinedActor->GetProperty()->SetSpecularPower(20.0);
        m_renderer->AddActor(m_combinedActor);
    }

    // For the per-label fallback path (no combined mesh), populate m_explodeActors directly.
    m_explodeActors.reserve(preparedScene.meshes.size());
    for (const auto &mesh : preparedScene.meshes) {
        if (mesh.polyData == nullptr || mesh.polyData->GetNumberOfPoints() == 0 || mesh.polyData->GetNumberOfCells() == 0) {
            continue;
        }

        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(mesh.polyData);
        if (mesh.useCellScalars) {
            mapper->SetScalarModeToUseCellData();
            mapper->ScalarVisibilityOn();
        } else {
            mapper->ScalarVisibilityOff();
        }

        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        if (!mesh.useCellScalars) {
            const double cr = ((mesh.lutColor >> 16) & 0xFF) / 255.0;
            const double cg = ((mesh.lutColor >> 8) & 0xFF) / 255.0;
            const double cb = (mesh.lutColor & 0xFF) / 255.0;
            actor->GetProperty()->SetColor(cr, cg, cb);
        }
        actor->GetProperty()->SetAmbient(0.1);
        actor->GetProperty()->SetDiffuse(0.7);
        actor->GetProperty()->SetSpecular(0.3);
        actor->GetProperty()->SetSpecularPower(20.0);
        m_renderer->AddActor(actor);
        m_explodeActors.push_back({actor, mesh.centerWorld});
    }

    m_sceneCenterWorld = preparedScene.sceneCenterWorld;
    m_sceneExtent = preparedScene.sceneExtent;
    m_splitSourcePolyData = std::move(preparedScene.splitSourcePolyData);
    m_splitLabels = std::move(preparedScene.splitLabels);

    if (m_combinedActor != nullptr || !m_explodeActors.empty()) {
        m_renderer->ResetCamera();
    }

    // Show explode controls when a deferred split is available (combined-mesh path)
    // or when individual actors are already loaded (fallback path with >1 mesh).
    const bool showExplodeControls =
        m_splitSourcePolyData != nullptr || m_explodeActors.size() > 1;

    m_vtkWidget = new QVTKOpenGLNativeWidget(this);
    m_vtkWidget->setRenderWindow(renWin);
    m_vtkWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_vtkWidget->setFocusPolicy(Qt::StrongFocus);

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
    layout->addWidget(m_vtkWidget, 1);

    if (showExplodeControls) {
        m_taskRunner = new TaskRunner(this, this);

        m_controlsWidget = new QWidget(this);
        m_controlsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        m_explodeSlider = new QSlider(Qt::Horizontal, m_controlsWidget);
        m_explodeSlider->setRange(0, 100);
        m_explodeSlider->setSingleStep(2);
        m_explodeSlider->setPageStep(10);
        m_explodeSlider->setValue(0);
        m_explodeSlider->setFocusPolicy(Qt::StrongFocus);
        m_explodeSlider->setToolTip(QStringLiteral("Explode segments apart to inspect connections."));
        m_explodeSlider->setMinimumHeight(28);

        // Slider disabled until the user activates explode mode.
        // For the fallback path (no combined mesh) actors are already loaded — enable directly.
        const bool hasPreloadedActors = !m_explodeActors.empty();
        m_explodeSlider->setEnabled(hasPreloadedActors);

        connect(m_explodeSlider, &QSlider::valueChanged, this, [this](int value) {
            if (m_explodeActors.empty()) {
                return;
            }
            const double offsetDistance =
                (static_cast<double>(value) / 100.0) * 0.35 * m_sceneExtent;
            for (const auto &actorInfo : m_explodeActors) {
                if (actorInfo.actor == nullptr) {
                    continue;
                }
                const double dx = actorInfo.centerWorld[0] - m_sceneCenterWorld[0];
                const double dy = actorInfo.centerWorld[1] - m_sceneCenterWorld[1];
                const double dz = actorInfo.centerWorld[2] - m_sceneCenterWorld[2];
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len > 1e-9) {
                    actorInfo.actor->SetPosition(offsetDistance * dx / len,
                                                 offsetDistance * dy / len,
                                                 offsetDistance * dz / len);
                } else {
                    actorInfo.actor->SetPosition(0.0, 0.0, 0.0);
                }
            }
            m_renderer->ResetCameraClippingRange();
            if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
                m_vtkWidget->renderWindow()->Render();
            }
        });

        auto *controlsRow = new QHBoxLayout(m_controlsWidget);
        controlsRow->setContentsMargins(10, 6, 10, 10);
        controlsRow->setSpacing(8);

        if (m_splitSourcePolyData != nullptr) {
            m_explodeToggle = new QCheckBox(QStringLiteral("Explode"), m_controlsWidget);
            connect(m_explodeToggle, &QCheckBox::toggled, this,
                    &Segment3DViewerDialog::onExplodeToggled);
            controlsRow->addWidget(m_explodeToggle);
        }

        controlsRow->addWidget(m_explodeSlider, 1);

        layout->addWidget(m_controlsWidget, 0);
    }

    auto *closeShortcut = new QShortcut(QKeySequence(Qt::Key_Q), this);
    closeShortcut->setContext(Qt::WindowShortcut);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::close);

    if (showExplodeControls) {
        auto *stepLeftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
        stepLeftShortcut->setContext(Qt::WindowShortcut);
        connect(stepLeftShortcut, &QShortcut::activated, this, [this]() { stepExplodeSlider(-1); });

        auto *stepRightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
        stepRightShortcut->setContext(Qt::WindowShortcut);
        connect(stepRightShortcut, &QShortcut::activated, this, [this]() { stepExplodeSlider(1); });
    }

    QTimer::singleShot(0, this, [this]() {
        if (m_controlsWidget != nullptr) {
            m_controlsWidget->raise();
        }
        if (m_vtkWidget != nullptr && m_vtkWidget->interactor() != nullptr) {
            m_vtkWidget->interactor()->Initialize();
            m_vtkWidget->interactor()->Enable();
        }
        if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
            m_renderer->ResetCameraClippingRange();
            m_vtkWidget->renderWindow()->Render();
        }
    });
}

void Segment3DViewerDialog::stepExplodeSlider(int direction) {
    if (m_explodeSlider == nullptr || !m_explodeSlider->isEnabled()) {
        return;
    }

    m_explodeSlider->triggerAction(direction < 0
                                       ? QAbstractSlider::SliderSingleStepSub
                                       : QAbstractSlider::SliderSingleStepAdd);
}

void Segment3DViewerDialog::onExplodeToggled(bool checked) {
    if (checked) {
        if (!m_explodeActors.empty()) {
            // Already split — just show the actors.
            if (m_combinedActor != nullptr) {
                m_combinedActor->SetVisibility(false);
            }
            for (const auto &info : m_explodeActors) {
                if (info.actor != nullptr) {
                    info.actor->SetVisibility(true);
                }
            }
            if (m_explodeSlider != nullptr) {
                m_explodeSlider->setEnabled(true);
            }
            m_renderer->ResetCameraClippingRange();
            if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
                m_vtkWidget->renderWindow()->Render();
            }
            return;
        }

        // First activation — run the split in the background.
        if (m_explodeToggle != nullptr) {
            m_explodeToggle->setEnabled(false);
        }
        vtkSmartPointer<vtkPolyData> sourcePolyData = m_splitSourcePolyData;
        std::vector<LabelWithColor> splitLabels = m_splitLabels;
        m_taskRunner->runWithLabel(
            QStringLiteral("Preparing explode view..."),
            [sourcePolyData, splitLabels]() -> std::vector<PreparedMesh> {
                if (sourcePolyData == nullptr) {
                    return {};
                }
                auto *boundaryLabels = sourcePolyData->GetCellData()->GetArray("BoundaryLabels");
                if (boundaryLabels == nullptr || boundaryLabels->GetNumberOfComponents() < 2) {
                    return {};
                }
                return splitMultiLabelMesh(sourcePolyData, boundaryLabels, splitLabels);
            },
            [this](std::vector<PreparedMesh> meshes) {
                if (m_explodeToggle != nullptr) {
                    m_explodeToggle->setEnabled(true);
                }
                if (meshes.empty()) {
                    // Split produced nothing — revert toggle.
                    if (m_explodeToggle != nullptr) {
                        m_explodeToggle->setChecked(false);
                    }
                    return;
                }
                activateExplodeActors(meshes);
            });
    } else {
        // Toggle OFF — switch back to the combined mesh.
        for (const auto &info : m_explodeActors) {
            if (info.actor != nullptr) {
                info.actor->SetVisibility(false);
                info.actor->SetPosition(0.0, 0.0, 0.0);
            }
        }
        if (m_combinedActor != nullptr) {
            m_combinedActor->SetVisibility(true);
        }
        if (m_explodeSlider != nullptr) {
            m_explodeSlider->setEnabled(false);
            m_explodeSlider->setValue(0);
        }
        m_renderer->ResetCameraClippingRange();
        if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
            m_vtkWidget->renderWindow()->Render();
        }
    }
}

void Segment3DViewerDialog::activateExplodeActors(const std::vector<PreparedMesh> &meshes) {
    for (const auto &mesh : meshes) {
        if (mesh.polyData == nullptr || mesh.polyData->GetNumberOfPoints() == 0
            || mesh.polyData->GetNumberOfCells() == 0) {
            continue;
        }

        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(mesh.polyData);
        if (mesh.useCellScalars) {
            mapper->SetScalarModeToUseCellData();
            mapper->ScalarVisibilityOn();
        } else {
            mapper->ScalarVisibilityOff();
        }

        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        if (!mesh.useCellScalars) {
            const double cr = ((mesh.lutColor >> 16) & 0xFF) / 255.0;
            const double cg = ((mesh.lutColor >> 8) & 0xFF) / 255.0;
            const double cb = (mesh.lutColor & 0xFF) / 255.0;
            actor->GetProperty()->SetColor(cr, cg, cb);
        }
        actor->GetProperty()->SetAmbient(0.1);
        actor->GetProperty()->SetDiffuse(0.7);
        actor->GetProperty()->SetSpecular(0.3);
        actor->GetProperty()->SetSpecularPower(20.0);
        m_renderer->AddActor(actor);
        m_explodeActors.push_back({actor, mesh.centerWorld});
    }

    if (m_combinedActor != nullptr) {
        m_combinedActor->SetVisibility(false);
    }
    if (m_explodeSlider != nullptr) {
        m_explodeSlider->setEnabled(true);
    }
    m_renderer->ResetCameraClippingRange();
    if (m_vtkWidget != nullptr && m_vtkWidget->renderWindow() != nullptr) {
        m_vtkWidget->renderWindow()->Render();
    }
}
