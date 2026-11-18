#include "src/image_storage/StreamVByteImageStorage.h"
#include "src/image_storage/DenseImageStorage.h"
#include <QCoreApplication>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vtkAOSDataArrayTemplate.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkExtractVOI.h>
#include <vtkImageCast.h>
#include <vtkImageConstantPad.h>
#include <vtkImageImport.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkSurfaceNets3D.h>

#include "src/segment_handling/SeededWatershedSplit.h"
#include "src/viewers/Segment3DViewerDialog.h"
#include "src/viewers/itkSignalBase.h"
#include "src/utils/AppLogger.h"
#include "src/utils/utils.h"
#include "projected_3d_cut_sample_utils.h"

namespace {

namespace sample = projected3dcutsample;
using SegmentIdType = dataType::SegmentIdType;

struct ProgramOptions {
    QString segmentsPath;
    int viewportSize = 1024;
    double strokeWidthPixels = 6.0;
    bool sceneOnly = false;
    bool applyOnly = false;
};

int failTest(const std::string &message) {
    std::cerr << "Assertion failed: " << message << "\n";
    return 1;
}

bool parseArgs(int argc, char **argv, ProgramOptions &options) {
    for (int index = 1; index < argc; ++index) {
        const QString arg = QString::fromLocal8Bit(argv[index]);
        if (arg == "--segments" && index + 1 < argc) {
            options.segmentsPath = QString::fromLocal8Bit(argv[++index]);
        } else if (arg == "--viewport" && index + 1 < argc) {
            options.viewportSize = std::max(128, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (arg == "--stroke-width" && index + 1 < argc) {
            options.strokeWidthPixels = std::max(1.0, QString::fromLocal8Bit(argv[++index]).toDouble());
        } else if (arg == "--apply-only") {
            options.applyOnly = true;
        } else if (arg == "--scene-only") {
            options.sceneOnly = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--segments PATH] [--viewport N] [--stroke-width PX] [--scene-only | --apply-only]\n";
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg.toStdString() << "\n";
            return false;
        }
    }

    return true;
}

std::size_t countNonBackgroundVoxels(const dataType::SegmentsImageType::Pointer &image, SegmentIdType backgroundId) {
    const auto voxelCount = image->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *buffer = image->GetBufferPointer();
    std::size_t result = 0;
    for (std::size_t index = 0; index < voxelCount; ++index) {
        result += buffer[index] != backgroundId ? 1U : 0U;
    }
    return result;
}

bool imagesEqual(const dataType::SegmentsImageType::Pointer &left,
                 const dataType::SegmentsImageType::Pointer &right) {
    if (left.IsNull() || right.IsNull() ||
        left->GetLargestPossibleRegion() != right->GetLargestPossibleRegion()) {
        return false;
    }
    const auto voxelCount = left->GetLargestPossibleRegion().GetNumberOfPixels();
    return std::equal(left->GetBufferPointer(),
                      left->GetBufferPointer() + voxelCount,
                      right->GetBufferPointer());
}

bool labelIsConnectedInImage(const dataType::SegmentsImageType::Pointer &image, SegmentIdType label) {
    const auto size = image->GetLargestPossibleRegion().GetSize();
    const std::ptrdiff_t dimX = static_cast<std::ptrdiff_t>(size[0]);
    const std::ptrdiff_t dimY = static_cast<std::ptrdiff_t>(size[1]);
    const std::ptrdiff_t dimZ = static_cast<std::ptrdiff_t>(size[2]);
    const std::ptrdiff_t planeXY = dimX * dimY;
    const std::ptrdiff_t total = planeXY * dimZ;
    const auto *buffer = image->GetBufferPointer();

    std::ptrdiff_t seed = -1;
    std::size_t totalLabelCount = 0;
    for (std::ptrdiff_t index = 0; index < total; ++index) {
        if (buffer[index] != label) {
            continue;
        }
        ++totalLabelCount;
        if (seed < 0) {
            seed = index;
        }
    }
    if (seed < 0) {
        return false;
    }

    std::vector<unsigned char> visited(static_cast<std::size_t>(total), 0);
    std::vector<std::ptrdiff_t> open;
    open.reserve(totalLabelCount);
    open.push_back(seed);
    visited[static_cast<std::size_t>(seed)] = 1;

    std::size_t visitedLabelCount = 0;
    const std::array<std::ptrdiff_t, 6> offsetX{{1, -1, 0, 0, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetY{{0, 0, 1, -1, 0, 0}};
    const std::array<std::ptrdiff_t, 6> offsetZ{{0, 0, 0, 0, 1, -1}};

    for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
        const std::ptrdiff_t current = open[queueIndex];
        ++visitedLabelCount;

        const std::ptrdiff_t z = current / planeXY;
        const std::ptrdiff_t remainder = current % planeXY;
        const std::ptrdiff_t y = remainder / dimX;
        const std::ptrdiff_t x = remainder % dimX;

        for (std::size_t direction = 0; direction < offsetX.size(); ++direction) {
            const std::ptrdiff_t nx = x + offsetX[direction];
            const std::ptrdiff_t ny = y + offsetY[direction];
            const std::ptrdiff_t nz = z + offsetZ[direction];
            if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ) {
                continue;
            }

            const std::ptrdiff_t neighbor = nx + ny * dimX + nz * planeXY;
            if (visited[static_cast<std::size_t>(neighbor)] != 0 || buffer[neighbor] != label) {
                continue;
            }
            visited[static_cast<std::size_t>(neighbor)] = 1;
            open.push_back(neighbor);
        }
    }

    return visitedLabelCount == totalLabelCount;
}

int testRepresentativeVoxelBounds() {
    auto image = dataType::SegmentsImageType::New();
    image->SetRegions({{0, 0, 0}, {10, 3, 3}});
    image->Allocate();
    image->FillBuffer(0);
    image->SetPixel({2, 1, 1}, 1);
    image->SetPixel({4, 1, 1}, 1);
    Roi bounds(Voxel(2, 1, 1));
    bounds.updateBoundingRoi(Voxel(4, 1, 1));
    const auto boundsModifiedTime = image->GetMTime();
    itk::Index<3> fullIndex, boundedIndex;
    if (!utils::findRepresentativeVoxelForLabel(image, 1, fullIndex)
        || !utils::findRepresentativeVoxelForLabel(
            image, 1, boundedIndex, bounds, boundsModifiedTime)
        || fullIndex != boundedIndex || boundedIndex != itk::Index<3>{{2, 1, 1}}
        || !utils::findRepresentativeVoxelForLabel(
            image, 1, boundedIndex, {}, boundsModifiedTime)
        || fullIndex != boundedIndex) {
        return failTest("Cached bounds must preserve the centroid search and tie order.");
    }

    image->SetPixel({6, 1, 1}, 1);
    image->Modified();
    if (!utils::findRepresentativeVoxelForLabel(image, 1, fullIndex)
        || !utils::findRepresentativeVoxelForLabel(
            image, 1, boundedIndex, bounds, boundsModifiedTime)
        || fullIndex != boundedIndex || boundedIndex != itk::Index<3>{{4, 1, 1}}) {
        return failTest("Stale bounds must fall back to the full image.");
    }
    return 0;
}

int testExplodedMeshSplit() {
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->InsertNextPoint(0.0, 0.0, 0.0);
    points->InsertNextPoint(2.0, 0.0, 0.0);
    points->InsertNextPoint(0.0, 2.0, 0.0);
    points->InsertNextPoint(10.0, 0.0, 0.0);
    points->InsertNextPoint(12.0, 0.0, 0.0);
    points->InsertNextPoint(10.0, 2.0, 0.0);
    points->InsertNextPoint(20.0, 0.0, 0.0);
    points->InsertNextPoint(22.0, 0.0, 0.0);
    points->InsertNextPoint(20.0, 2.0, 0.0);
    points->InsertNextPoint(1000.0, 1000.0, 1000.0); // Deliberately unused.

    auto polys = vtkSmartPointer<vtkCellArray>::New();
    const vtkIdType outerA[3]{0, 1, 2};
    const vtkIdType shared[3]{3, 4, 5};
    const vtkIdType outerB[3]{6, 7, 8};
    polys->InsertNextCell(3, outerA);
    polys->InsertNextCell(3, shared);
    polys->InsertNextCell(3, outerB);

    auto boundaryLabels =
        vtkSmartPointer<vtkAOSDataArrayTemplate<SegmentIdType>>::New();
    boundaryLabels->SetName("BoundaryLabels");
    boundaryLabels->SetNumberOfComponents(2);
    const SegmentIdType boundary0[2]{1, 0};
    const SegmentIdType boundary1[2]{1, 2};
    const SegmentIdType boundary2[2]{2, 0};
    boundaryLabels->InsertNextTypedTuple(boundary0);
    boundaryLabels->InsertNextTypedTuple(boundary1);
    boundaryLabels->InsertNextTypedTuple(boundary2);

    auto source = vtkSmartPointer<vtkPolyData>::New();
    source->SetPoints(points);
    source->SetPolys(polys);
    source->GetCellData()->AddArray(boundaryLabels);

    const std::vector<Segment3DViewerDialog::LabelWithColor> labels{
        {1, 0xFF0000}, {2, 0x00FF00}, {3, 0x0000FF}};
    const auto meshes = Segment3DViewerDialog::prepareExplodedMeshes(source, labels);
    if (meshes.size() != 2 || meshes[0].labelId != 1 || meshes[1].labelId != 2) {
        return failTest("Explode split did not produce the expected non-empty label meshes.");
    }

    const std::array<std::array<double, 6>, 2> expectedBounds{{
        {0.0, 12.0, 0.0, 2.0, 0.0, 0.0},
        {10.0, 22.0, 0.0, 2.0, 0.0, 0.0}}};
    const std::array<std::array<double, 3>, 2> expectedCenters{{
        {6.0, 1.0, 0.0}, {16.0, 1.0, 0.0}}};
    for (std::size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
        const auto &mesh = meshes[meshIndex];
        if (mesh.polyData->GetPoints() != points
            || mesh.polyData->GetNumberOfCells() != 2) {
            return failTest("Explode mesh does not share points or has the wrong cell count.");
        }

        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(mesh.polyData);
        const double *mapperBounds = mapper->GetBounds();
        for (int component = 0; component < 6; ++component) {
            if (std::abs(mapperBounds[component] - expectedBounds[meshIndex][component]) > 1e-9) {
                return failTest("Explode mapper bounds include points from another label.");
            }
        }
        for (int component = 0; component < 3; ++component) {
            if (std::abs(mesh.centerWorld[component] - expectedCenters[meshIndex][component]) > 1e-9) {
                return failTest("Explode mesh center is incorrect.");
            }
        }
    }

    vtkIdType pointCount = 0;
    const vtkIdType *pointIds = nullptr;
    meshes[0].polyData->GetCellPoints(1, pointCount, pointIds);
    if (pointCount != 3 || !std::equal(pointIds, pointIds + 3, shared)) {
        return failTest("Explode split did not retain the shared interface for the first label.");
    }
    meshes[1].polyData->GetCellPoints(0, pointCount, pointIds);
    if (pointCount != 3 || !std::equal(pointIds, pointIds + 3, shared)) {
        return failTest("Explode split did not duplicate the shared interface for the second label.");
    }

    // A small budget exercises compaction without allocating a large mesh.
    auto compactSource = vtkSmartPointer<vtkPolyData>::New();
    compactSource->DeepCopy(source);
    auto *sourcePolys = compactSource->GetPolys();
    sourcePolys->InsertNextCell(3, shared);
    auto *sourceLabels = vtkAOSDataArrayTemplate<SegmentIdType>::SafeDownCast(
        compactSource->GetCellData()->GetArray("BoundaryLabels"));
    sourceLabels->InsertNextTypedTuple(boundary1);
    constexpr std::uint64_t budget = 6 * 3 * sizeof(float);
    for (const bool use32BitConnectivity : {false, true}) {
        if (use32BitConnectivity) {
            sourcePolys->ConvertTo32BitStorage();
        } else {
            sourcePolys->ConvertTo64BitStorage();
        }
        auto compactMeshes = Segment3DViewerDialog::prepareExplodedMeshes(compactSource, labels, budget);
        if (compactMeshes.size() != 2
            || compactMeshes[0].polyData->GetPoints() == compactMeshes[1].polyData->GetPoints()) {
            return failTest("Large scene did not receive per-label point buffers.");
        }
        const std::array<std::array<vtkIdType, 3>, 2> sourceCells{{{0, 1, 3}, {1, 2, 3}}};
        for (std::size_t index = 0; index < compactMeshes.size(); ++index) {
            auto *mesh = compactMeshes[index].polyData.GetPointer();
            if (mesh->GetPoints() == compactSource->GetPoints()
                || mesh->GetNumberOfPoints() != 6 || mesh->GetNumberOfCells() != 3) {
                return failTest("Point compaction changed geometry counts.");
            }
            double bounds[6];
            mesh->GetCellsBounds(bounds);
            if (!std::equal(bounds, bounds + 6, expectedBounds[index].begin())) {
                return failTest("Point compaction changed label bounds.");
            }
            for (vtkIdType cell = 0; cell < 3; ++cell) {
                vtkIdType sourceCount = 0;
                const vtkIdType *sourceIds = nullptr;
                compactSource->GetCellPoints(sourceCells[index][cell], sourceCount, sourceIds);
                mesh->GetCellPoints(cell, pointCount, pointIds);
                if (sourceCount != 3 || pointCount != 3) {
                    return failTest("Point compaction changed a triangle.");
                }
                for (int corner = 0; corner < 3; ++corner) {
                    if (pointIds[corner] < 0 || pointIds[corner] >= mesh->GetNumberOfPoints()) {
                        return failTest("Point compaction produced an invalid point id.");
                    }
                    double original[3], compact[3];
                    compactSource->GetPoint(sourceIds[corner], original);
                    mesh->GetPoint(pointIds[corner], compact);
                    if (!std::equal(original, original + 3, compact)) {
                        return failTest("Point compaction changed triangle coordinates or winding.");
                    }
                }
            }
        }
        compactSource->GetCellPoints(1, pointCount, pointIds);
        if (!std::equal(pointIds, pointIds + 3, shared) || compactSource->GetNumberOfPoints() != 10) {
            return failTest("Point compaction mutated the source mesh.");
        }
        try {
            Segment3DViewerDialog::prepareExplodedMeshes(compactSource, labels, budget - 1);
            return failTest("Oversized per-label point buffer was not rejected.");
        } catch (const std::runtime_error &) {
        }
    }

    auto repeatedSource = vtkSmartPointer<vtkPolyData>::New();
    auto repeatedPolys = vtkSmartPointer<vtkCellArray>::New();
    auto repeatedLabels = vtkSmartPointer<vtkAOSDataArrayTemplate<SegmentIdType>>::New();
    repeatedLabels->SetName("BoundaryLabels");
    repeatedLabels->SetNumberOfComponents(2);
    for (int cell = 0; cell < 11; ++cell) {
        repeatedPolys->InsertNextCell(3, outerA);
        repeatedLabels->InsertNextTypedTuple(boundary0);
    }
    repeatedSource->SetPoints(points);
    repeatedSource->SetPolys(repeatedPolys);
    repeatedSource->GetCellData()->AddArray(repeatedLabels);
    try {
        // Points fit exactly, but the triangle-index buffer exceeds the budget.
        Segment3DViewerDialog::prepareExplodedMeshes(repeatedSource, labels, 10 * 3 * sizeof(float));
        return failTest("Oversized triangle-index buffer was not rejected.");
    } catch (const std::runtime_error &) {
    }

    const std::vector<Segment3DViewerDialog::LabelWithColor> duplicateLabels{
        {1, 0xFF0000}, {1, 0x00FF00}};
    if (!Segment3DViewerDialog::prepareExplodedMeshes(source, duplicateLabels).empty()) {
        return failTest("Explode split accepted duplicate labels.");
    }

    auto shortBoundaryLabels =
        vtkSmartPointer<vtkAOSDataArrayTemplate<SegmentIdType>>::New();
    shortBoundaryLabels->SetName("BoundaryLabels");
    shortBoundaryLabels->SetNumberOfComponents(2);
    shortBoundaryLabels->InsertNextTypedTuple(boundary0);
    shortBoundaryLabels->InsertNextTypedTuple(boundary1);
    auto shortBoundarySource = vtkSmartPointer<vtkPolyData>::New();
    shortBoundarySource->SetPoints(points);
    shortBoundarySource->SetPolys(polys);
    shortBoundarySource->GetCellData()->AddArray(shortBoundaryLabels);
    if (!Segment3DViewerDialog::prepareExplodedMeshes(shortBoundarySource, labels).empty()) {
        return failTest("Explode split accepted a short BoundaryLabels array.");
    }

    auto invalidPolys = vtkSmartPointer<vtkCellArray>::New();
    const vtkIdType invalidCell[3]{6, 7, 99};
    invalidPolys->InsertNextCell(3, outerA);
    invalidPolys->InsertNextCell(3, shared);
    invalidPolys->InsertNextCell(3, invalidCell);
    auto invalidSource = vtkSmartPointer<vtkPolyData>::New();
    invalidSource->SetPoints(points);
    invalidSource->SetPolys(invalidPolys);
    invalidSource->GetCellData()->AddArray(boundaryLabels);
    if (!Segment3DViewerDialog::prepareExplodedMeshes(invalidSource, labels).empty()) {
        return failTest("Explode split accepted an out-of-range point id.");
    }
    return 0;
}

dataType::SegmentsImageType::Pointer makeSmallSegmentsImage(bool includeSecondLabel) {
    dataType::SegmentsImageType::SizeType size{{6, 6, 6}};
    dataType::SegmentsImageType::IndexType start{{0, 0, 0}};
    dataType::SegmentsImageType::RegionType region;
    region.SetIndex(start);
    region.SetSize(size);

    auto image = dataType::SegmentsImageType::New();
    image->SetRegions(region);
    image->Allocate();
    image->FillBuffer(0);

    for (int z = 1; z <= 4; ++z) {
        for (int y = 1; y <= 4; ++y) {
            for (int x = 1; x <= 4; ++x) {
                dataType::SegmentsImageType::IndexType index{{x, y, z}};
                image->SetPixel(index, 1);
            }
            if (includeSecondLabel) {
                dataType::SegmentsImageType::IndexType boundaryIndex{{0, y, z}};
                image->SetPixel(boundaryIndex, 2);
            }
        }
    }
    return image;
}

int testAllLabelsSceneUsesSegmentMeshes() {
    constexpr quint32 red = 0xFF0000;
    constexpr quint32 green = 0x00FF00;
    auto overrides = std::make_shared<LabelColorSnapshot::ColorMap>();
    overrides->emplace(1, red);
    overrides->emplace(2, green);
    LabelColorSnapshot labelColors;
    labelColors.overrides = std::move(overrides);

    auto image = makeSmallSegmentsImage(true);
    auto scene = Segment3DViewerDialog::prepareAllLabelsScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), labelColors);
    if (!scene.allLabelsInImage || scene.targetLabelId != 0 || scene.meshes.size() != 2) {
        return failTest("All-label scene did not produce one mesh per label.");
    }
    if (scene.meshes[0].voxelCount != 64 || scene.meshes[1].voxelCount != 16) {
        return failTest("All-label scene did not preserve the segment voxel counts.");
    }
    if (scene.meshes[0].labelId != 1 || scene.meshes[0].lutColor != red
        || scene.meshes[1].labelId != 2 || scene.meshes[1].lutColor != green) {
        return failTest("All-label scene did not preserve label colors.");
    }
    if (scene.meshes[0].touchesImageBoundary
        || !scene.meshes[1].touchesImageBoundary) {
        return failTest("All-label scene did not identify the volume-edge segment.");
    }
    if (scene.meshes[0].polyData->GetPoints()
        != scene.meshes[1].polyData->GetPoints()) {
        return failTest("All-label scene meshes do not share their points.");
    }
    if (!std::isfinite(scene.sceneExtent) || scene.sceneExtent <= 0.0
        || !std::all_of(scene.sceneCenterWorld.begin(),
                        scene.sceneCenterWorld.end(),
                        [](double value) { return std::isfinite(value); })) {
        return failTest("All-label scene bounds are invalid.");
    }

    auto selectedScene = Segment3DViewerDialog::prepareScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), {{1, red}});
    if (selectedScene.meshes.size() != 1) {
        return failTest("Selected-label scene did not produce one mesh.");
    }
    auto *selected = selectedScene.meshes.front().polyData.GetPointer();
    auto *reference = scene.meshes.front().polyData.GetPointer();
    if (selected->GetNumberOfPoints() >= reference->GetNumberOfPoints()
        || selected->GetNumberOfCells() != reference->GetNumberOfCells()
        || selected->GetPolys()->GetConnectivityArray()->GetDataTypeSize() != sizeof(std::int32_t)) {
        return failTest("Selected-label mesh did not compact points and connectivity.");
    }
    std::vector<bool> usedPoints(static_cast<std::size_t>(selected->GetNumberOfPoints()), false);
    for (vtkIdType cell = 0; cell < selected->GetNumberOfCells(); ++cell) {
        vtkIdType count, referenceCount;
        const vtkIdType *ids, *referenceIds;
        selected->GetCellPoints(cell, count, ids);
        reference->GetCellPoints(cell, referenceCount, referenceIds);
        if (count != 3 || referenceCount != 3) {
            return failTest("Selected-label compaction changed a triangle.");
        }
        for (int corner = 0; corner < 3; ++corner) {
            if (ids[corner] < 0 || ids[corner] >= selected->GetNumberOfPoints()) {
                return failTest("Selected-label mesh contains an invalid point id.");
            }
            usedPoints[static_cast<std::size_t>(ids[corner])] = true;
            double point[3], referencePoint[3];
            selected->GetPoint(ids[corner], point);
            reference->GetPoint(referenceIds[corner], referencePoint);
            if (!std::equal(point, point + 3, referencePoint)) {
                return failTest("Selected-label compaction changed geometry or winding.");
            }
        }
    }
    if (std::find(usedPoints.begin(), usedPoints.end(), false) != usedPoints.end()) {
        return failTest("Selected-label mesh retained unused points.");
    }

    auto selectedLabelsScene = Segment3DViewerDialog::prepareScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), {{1, red}, {2, green}});
    if (selectedLabelsScene.meshes.size() != 2
        || selectedLabelsScene.meshes[0].polyData->GetNumberOfCells() != reference->GetNumberOfCells()
        || selectedLabelsScene.meshes[1].polyData->GetNumberOfCells()
            != scene.meshes[1].polyData->GetNumberOfCells()
        || selectedLabelsScene.meshes[0].polyData->GetPoints()
            != selectedLabelsScene.meshes[1].polyData->GetPoints()) {
        return failTest("Selected multi-label scene changed cell counts or stopped sharing points.");
    }

    for (const SegmentIdType neighborLabel : {65535u, 65536u}) {
        auto boundaryImage = makeSmallSegmentsImage(true);
        auto *pixels = boundaryImage->GetBufferPointer();
        const auto voxelCount = boundaryImage->GetLargestPossibleRegion().GetNumberOfPixels();
        std::replace(pixels, pixels + voxelCount, SegmentIdType{2}, neighborLabel);
        auto allLabels = Segment3DViewerDialog::prepareAllLabelsScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*boundaryImage), labelColors);
        auto singleLabel = Segment3DViewerDialog::prepareScene(segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(boundaryImage), {{1, red}});
        if (allLabels.meshes.size() != 2 || allLabels.meshes[1].labelId != neighborLabel
            || allLabels.meshes[1].voxelCount != 16 || singleLabel.meshes.size() != 1) {
            return failTest("Scene lost a label at the uint16 boundary.");
        }
        auto *actual = singleLabel.meshes.front().polyData.GetPointer();
        if (actual->GetNumberOfPoints() != selected->GetNumberOfPoints()
            || actual->GetNumberOfCells() != selected->GetNumberOfCells()) {
            return failTest("Neighbor label width changed the selected mesh.");
        }
        for (vtkIdType pointId = 0; pointId < selected->GetNumberOfPoints(); ++pointId) {
            double point[3], expected[3];
            actual->GetPoint(pointId, point);
            selected->GetPoint(pointId, expected);
            if (!std::equal(point, point + 3, expected)) {
                return failTest("Neighbor label width changed the selected surface coordinates.");
            }
        }
    }

    auto singleLabelScene = Segment3DViewerDialog::prepareAllLabelsScene(
        segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*makeSmallSegmentsImage(false)), labelColors);
    if (!singleLabelScene.allLabelsInImage || singleLabelScene.targetLabelId != 0
        || singleLabelScene.meshes.size() != 1
        || singleLabelScene.meshes.front().labelId != 1
        || singleLabelScene.meshes.front().voxelCount != 64) {
        return failTest("Single-label all-label scene did not use the segment-mesh path.");
    }
    return 0;
}

int testCompactionGrowth() {
    for (const bool useDouble : {false, true}) {
        auto source = vtkSmartPointer<vtkPolyData>::New();
        auto points = vtkSmartPointer<vtkPoints>::New();
        points->SetDataType(useDouble ? VTK_DOUBLE : VTK_FLOAT);
        for (int i = 0; i < 4096; ++i) {
            points->InsertNextPoint(i * 0.123456789012345, i % 17, i % 29);
        }
        auto polys = vtkSmartPointer<vtkCellArray>::New();
        auto labels = vtkSmartPointer<vtkAOSDataArrayTemplate<SegmentIdType>>::New();
        labels->SetName("BoundaryLabels");
        labels->SetNumberOfComponents(2);
        const SegmentIdType pair[]{1, 0};
        std::vector<vtkIdType> expectedIds;
        std::vector<vtkIdType> expectedPoints;
        std::unordered_map<vtkIdType, vtkIdType> reference;
        for (int i = 0; i < 5000; ++i) {
            const vtkIdType triangle[]{(37 * i) % 4096, (37 * i + 1) % 4096, (37 * i + 17) % 4096};
            polys->InsertNextCell(3, triangle);
            labels->InsertNextTypedTuple(pair);
            for (auto id : triangle) {
                const auto [entry, inserted] = reference.emplace(id, reference.size());
                if (inserted) expectedPoints.push_back(id);
                expectedIds.push_back(entry->second);
            }
        }
        source->SetPoints(points);
        source->SetPolys(polys);
        source->GetCellData()->AddArray(labels);
        for (const bool use64Bit : {false, true}) {
            if (use64Bit) polys->ConvertTo64BitStorage();
            else polys->ConvertTo32BitStorage();
            const auto meshes = Segment3DViewerDialog::prepareExplodedMeshes(source, {{1, 0}});
            if (meshes.size() != 1) return failTest("Compaction growth lost the mesh.");
            auto *mesh = meshes.front().polyData.GetPointer();
            if (mesh->GetNumberOfPoints() != static_cast<vtkIdType>(expectedPoints.size())
                || mesh->GetPoints()->GetDataType() != points->GetDataType()) {
                return failTest("Compaction changed unique points or coordinate precision.");
            }
            for (std::size_t i = 0; i < expectedPoints.size(); ++i) {
                double actual[3], expected[3];
                mesh->GetPoint(i, actual);
                points->GetPoint(expectedPoints[i], expected);
                if (!std::equal(actual, actual + 3, expected)) {
                    return failTest("Compaction changed point coordinates or first-occurrence order.");
                }
            }
            for (vtkIdType cell = 0; cell < polys->GetNumberOfCells(); ++cell) {
                vtkIdType count;
                const vtkIdType *ids;
                mesh->GetCellPoints(cell, count, ids);
                if (count != 3 || !std::equal(ids, ids + 3, expectedIds.begin() + 3 * cell)) {
                    return failTest("Compaction changed connectivity after growing its lookup table.");
                }
            }
        }
    }
    return 0;
}

int testPreparedVolumeGeometry() {
    for (const SegmentIdType neighbor : {2u, 65535u, 65536u, 1000003u}) {
        auto image = dataType::SegmentsImageType::New();
        image->SetRegions(dataType::SegmentsImageType::RegionType({{0, 0, 0}}, {{12, 10, 9}}));
        image->SetSpacing(dataType::SegmentsImageType::SpacingType{{0.3, 0.7, 1.1}});
        image->SetOrigin(dataType::SegmentsImageType::PointType{{10.0, -20.0, 30.0}});
        auto direction = image->GetDirection();
        direction[0][0] = direction[1][1] = 0;
        direction[0][1] = -1;
        direction[1][0] = 1;
        image->SetDirection(direction);
        image->Allocate();
        image->FillBuffer(0);
        for (int z = 3; z < 7; ++z)
            for (int y = 2; y < 7; ++y)
                for (int x = 4; x < 12; ++x)
                    image->SetPixel({{x, y, z}}, x < 8 ? 1 : neighbor);

        // Independent reference: the previous crop/cast/pad pipeline, with a
        // nonzero ROI, a foreground image boundary, and rotated anisotropic voxels.
        auto importer = vtkSmartPointer<vtkImageImport>::New();
        importer->SetImportVoidPointer(image->GetBufferPointer());
        importer->SetDataScalarType(VTK_UNSIGNED_INT);
        importer->SetNumberOfScalarComponents(1);
        importer->SetWholeExtent(0, 11, 0, 9, 0, 8);
        importer->SetDataExtentToWholeExtent();
        importer->SetDataSpacing(0.3, 0.7, 1.1);
        importer->SetDataOrigin(10.0, -20.0, 30.0);
        importer->SetDataDirection(direction.GetVnlMatrix().data_block());
        auto extract = vtkSmartPointer<vtkExtractVOI>::New();
        extract->SetInputConnection(importer->GetOutputPort());
        extract->SetVOI(3, 11, 1, 7, 2, 7);
        auto cast = vtkSmartPointer<vtkImageCast>::New();
        cast->SetInputConnection(extract->GetOutputPort());
        cast->SetOutputScalarType(neighbor <= 65535 ? VTK_UNSIGNED_SHORT : VTK_UNSIGNED_INT);
        auto pad = vtkSmartPointer<vtkImageConstantPad>::New();
        pad->SetInputConnection(cast->GetOutputPort());
        pad->SetOutputWholeExtent(2, 12, 0, 8, 1, 8);
        pad->SetConstant(0);
        auto surface = vtkSmartPointer<vtkSurfaceNets3D>::New();
        surface->SetInputConnection(pad->GetOutputPort());
        surface->SetBackgroundLabel(0);
        surface->SetOutputMeshTypeToTriangles();
        surface->SetTriangulationStrategyToGreedy();
        surface->SmoothingOn();
        surface->SetNumberOfIterations(3);
        surface->DataCachingOff();
        surface->SetNumberOfLabels(2);
        surface->SetLabel(0, 1);
        surface->SetLabel(1, neighbor);
        surface->Update();
        const auto reference = Segment3DViewerDialog::prepareExplodedMeshes(surface->GetOutput(), {{1, 0}, {neighbor, 0}});
        const auto actual = Segment3DViewerDialog::prepareAllLabelsScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), LabelColorSnapshot{});
        if (actual.meshes.size() != 2 || reference.size() != 2) {
            return failTest("Volume preparation lost a label.");
        }
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &mesh = actual.meshes[i];
            auto *expected = reference[i].polyData.GetPointer();
            if (mesh.voxelCount != 80 || mesh.touchesImageBoundary != (i == 1)
                || mesh.polyData->GetNumberOfCells() != expected->GetNumberOfCells()
                || mesh.polyData->GetNumberOfPoints() != expected->GetNumberOfPoints()
                || mesh.centerWorld != reference[i].centerWorld) {
                return failTest("Volume preparation changed counts, bounds or border labels.");
            }
            for (vtkIdType cell = 0; cell < expected->GetNumberOfCells(); ++cell) {
                vtkIdType count, expectedCount;
                const vtkIdType *ids, *expectedIds;
                mesh.polyData->GetCellPoints(cell, count, ids);
                expected->GetCellPoints(cell, expectedCount, expectedIds);
                if (count != expectedCount || !std::equal(ids, ids + count, expectedIds)) {
                    return failTest("Volume preparation changed triangle connectivity.");
                }
            }
            for (vtkIdType point = 0; point < expected->GetNumberOfPoints(); ++point) {
                double coordinates[3], expectedCoordinates[3];
                mesh.polyData->GetPoint(point, coordinates);
                expected->GetPoint(point, expectedCoordinates);
                if (!std::equal(coordinates, coordinates + 3, expectedCoordinates)) {
                    return failTest("Volume preparation changed world coordinates.");
                }
            }
        }
    }
    return 0;
}

int testSceneImageDirection() {
    auto image = makeSmallSegmentsImage(false);
    dataType::SegmentsImageType::SpacingType spacing;
    spacing[0] = 0.4;
    spacing[1] = 0.8;
    spacing[2] = 1.2;
    image->SetSpacing(spacing);
    const dataType::SegmentsImageType::PointType origin{{10.0, 20.0, 30.0}};
    image->SetOrigin(origin);
    auto reference = Segment3DViewerDialog::prepareScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), {{1, 0xFF0000}});

    dataType::SegmentsImageType::DirectionType direction;
    direction.SetIdentity();
    direction[0][0] = direction[1][1] = 0.0;
    direction[0][1] = -1.0;
    direction[1][0] = 1.0;
    image->SetDirection(direction);
    auto rotated = Segment3DViewerDialog::prepareScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), {{1, 0xFF0000}});
    if (reference.meshes.size() != 1 || rotated.meshes.size() != 1) {
        return failTest("Image-direction test did not produce its meshes.");
    }
    double bounds[6], rotatedBounds[6];
    reference.meshes.front().polyData->GetCellsBounds(bounds);
    rotated.meshes.front().polyData->GetCellsBounds(rotatedBounds);
    const double expected[6]{
        origin[0] - (bounds[3] - origin[1]), origin[0] - (bounds[2] - origin[1]),
        origin[1] + (bounds[0] - origin[0]), origin[1] + (bounds[1] - origin[0]),
        bounds[4], bounds[5]};
    for (int component = 0; component < 6; ++component) {
        if (std::abs(rotatedBounds[component] - expected[component]) > 1e-5) {
            return failTest("Scene ignored the image direction, spacing or origin.");
        }
    }
    return 0;
}

bool voxelListIsConnected(const VoxelList &voxels, const Roi &roi) {
    if (voxels.empty()) {
        return false;
    }

    const int sizeX = roi.maxX - roi.minX + 1;
    const int sizeY = roi.maxY - roi.minY + 1;
    const int sizeZ = roi.maxZ - roi.minZ + 1;
    const std::size_t planeXY = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY);
    const std::size_t total = planeXY * static_cast<std::size_t>(sizeZ);

    auto linearIndex = [&](const Voxel &voxel) {
        return static_cast<std::size_t>(voxel.z - roi.minZ) * planeXY +
               static_cast<std::size_t>(voxel.y - roi.minY) * static_cast<std::size_t>(sizeX) +
               static_cast<std::size_t>(voxel.x - roi.minX);
    };

    std::vector<unsigned char> occupied(total, 0);
    for (const Voxel &voxel : voxels) {
        occupied[linearIndex(voxel)] = 1;
    }

    std::vector<unsigned char> visited(total, 0);
    std::vector<Voxel> open;
    open.reserve(voxels.size());
    open.push_back(voxels.front());
    visited[linearIndex(voxels.front())] = 1;

    std::size_t visitedCount = 0;
    for (std::size_t queueIndex = 0; queueIndex < open.size(); ++queueIndex) {
        const Voxel activeVoxel = open[queueIndex];
        ++visitedCount;

        const std::array<Voxel, 6> neighbors{{
            Voxel(activeVoxel.x + 1, activeVoxel.y, activeVoxel.z),
            Voxel(activeVoxel.x - 1, activeVoxel.y, activeVoxel.z),
            Voxel(activeVoxel.x, activeVoxel.y + 1, activeVoxel.z),
            Voxel(activeVoxel.x, activeVoxel.y - 1, activeVoxel.z),
            Voxel(activeVoxel.x, activeVoxel.y, activeVoxel.z + 1),
            Voxel(activeVoxel.x, activeVoxel.y, activeVoxel.z - 1)
        }};

        for (const Voxel &neighbor : neighbors) {
            if (neighbor.x < roi.minX || neighbor.x > roi.maxX ||
                neighbor.y < roi.minY || neighbor.y > roi.maxY ||
                neighbor.z < roi.minZ || neighbor.z > roi.maxZ) {
                continue;
            }

            const std::size_t neighborIndex = linearIndex(neighbor);
            if (occupied[neighborIndex] == 0 || visited[neighborIndex] != 0) {
                continue;
            }
            visited[neighborIndex] = 1;
            open.push_back(neighbor);
        }
    }

    return visitedCount == voxels.size();
}

int checkConsistency(const Graph &graph) {
    if (graph.initialEdgeIdLookup.size() != graph.initialLabelPairToTwoSidedInitialEdge.size())
        return failTest("Initial edge lookups have different sizes.");
    for (const auto &[pair, edge] : graph.initialLabelPairToTwoSidedInitialEdge) {
        const auto &[a, b] = pair;
        const auto &left = graph.initialNodes.at(a);
        const auto &right = graph.initialNodes.at(b);
        if (graph.initialEdgeIdLookup.at(edge->numId) != pair ||
            left->neighborLabelToTwoSidedInitialEdge.at(b) != edge ||
            right->neighborLabelToTwoSidedInitialEdge.at(a) != edge ||
            left->neighborLabelToOneSidedInitialEdge.at(b) != edge->getSmallerLabelOneSidedInitialEdge() ||
            right->neighborLabelToOneSidedInitialEdge.at(a) != edge->getLargerLabelOneSidedInitialEdge())
            return failTest("The endpoints and the graph disagree about an initial edge.");
    }
    return 0;
}

int checkSplitBoundaries(const Graph &graph) {
    if (int result = checkConsistency(graph)) return result;
    using Image = dataType::SegmentsImageType;
    const auto &base = *graph.graphBase;
    // A fresh graph made from the final initial labels is a reference for every
    // directed boundary, including the neighbor side of a split interface.
    auto initialImage = sample::duplicateSegmentsImage(base.pWorkingSegmentsImage);
    initialImage->FillBuffer(graph.backgroundId);
    for (const auto &[id, node] : graph.initialNodes) {
        for (const auto &v : node->voxels) {
            const Image::IndexType index{v.x, v.y, v.z};
            initialImage->SetPixel(index, id);
            if (base.pWorkingSegmentsImage->GetPixel(index) != node->getCurrentWorkingNodeLabel())
                return failTest("Initial node ownership differs from the working image.");
        }
    }
    const auto reference = sample::buildGraphFixture(initialImage);
    const auto overlay = sample::duplicateSegmentsImage(base.pEdgesInitialSegmentsImage);
    auto matched = sample::duplicateSegmentsImage(overlay);
    matched->FillBuffer(0);
    for (const auto &[id, node] : reference.graph->initialNodes) {
        const auto &actual = graph.initialNodes.at(id)->neighborLabelToOneSidedInitialEdge;
        if (actual.size() != node->neighborLabelToOneSidedInitialEdge.size())
            return failTest("The split changed the number of initial contacts.");
        for (const auto &[neighborId, edge] : node->neighborLabelToOneSidedInitialEdge) {
            const auto &side = actual.at(neighborId)->voxels;
            if (side.size() != edge->voxels.size() || !std::equal(side.begin(), side.end(), edge->voxels.begin()))
                return failTest("Split boundary voxels differ from a full graph rebuild.");
            const auto pair = edge->pairId;
            const auto edgeId = graph.initialLabelPairToTwoSidedInitialEdge.at(pair)->numId;
            for (const auto &v : side) {
                const Image::IndexType index{v.x, v.y, v.z};
                const auto visible = overlay->GetPixel(index);
                if (!visible) return failTest("Missing boundary in the edge image.");
                if (visible == edgeId) matched->SetPixel(index, edgeId);
            }
        }
    }
    return imagesEqual(overlay, matched) ? 0 : failTest("The displayed edge does not touch this voxel.");
}

dataType::SegmentsStorage::Pointer splitTestStorage(dataType::SegmentsImageType::Pointer image, bool compressed) {
    if (compressed) return segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image);
    return segment_puzzler::DenseImageStorage<SegmentIdType>::fromImage(image);
}

int testSplitApply(bool compressed) {
    using namespace segment_puzzler;
    using Image = dataType::SegmentsImageType;
    auto image = Image::New();
    image->SetRegions(Image::RegionType({0, 0, 0}, {40, 24, 8}));
    image->Allocate(true);
    for (int z = 1; z <= 6; ++z) {
        for (int y = 2; y <= 21; ++y) {
            for (int x = 2; x <= 35; ++x) {
                const SegmentIdType label = 1 + (x > 21) + (x > 27) + (x > 32);
                image->SetPixel({x, y, z}, label);
            }
        }
    }
    auto base = std::make_shared<GraphBase>();
    Graph graph(base, false);
    base->pGraph = &graph;
    base->pWorkingSegmentsImage = splitTestStorage(sample::duplicateSegmentsImage(image), compressed);
    base->pSelectedSegmentation = splitTestStorage(image, compressed);
    base->selectedSegmentationContext->nextLabel = 5;
    graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
    graph.constructFromVolume(base->pWorkingSegmentsImage);

    const auto mergedLabels = graph.mergeEdges({graph.initialLabelPairToTwoSidedInitialEdge.at({2, 3})->numId});

    auto cut = sample::buildCandidateCut(sample::findLargestWorkingSegment(graph), image,
                                         sample::ProjectionPlane::XY, false, 200, 9);
    cut.request.strokePixels = {{0, 0}, {200, 200}};
    std::vector<SegmentIdType> parts;
    if (!graph.splitWorkingNodeByProjected3DCut(cut.request, nullptr, &parts) || parts.size() != 2)
        return failTest("The first projected split failed.");
    const auto selected = graph.transferWorkingNodesToSegmentation(parts);
    if (selected.size() != 2) return failTest("The first transfer failed.");
    const auto before = sample::duplicateSegmentsImage(base->pWorkingSegmentsImage);
    const auto selectedBefore = sample::duplicateSegmentsImage(base->pSelectedSegmentation);
    const auto session = prepareSeededWatershedSplit(base->pSelectedSegmentation, selected[0]);
    const auto left = seededSplitMaximumAlongWorldRay(session, {6, -5, 3}, {6, 30, 3});
    const auto right = seededSplitMaximumAlongWorldRay(session, {17, -5, 3}, {17, 30, 3});
    if (!left || !right) return failTest("Could not place both seeds.");
    const auto preview = computeSeededWatershedSplit(session, {*left, *right});
    if (!preview.valid()) return failTest(preview.error);
    const auto firstWorkingLabel = parts[0];
    std::vector<SegmentIdType> secondParts;
    if (!graph.splitWorkingNodeByVoxelPartition(firstWorkingLabel, preview.partition, session.globalOffset, &secondParts) ||
        secondParts.size() != 2) return failTest("Applying the seeded split failed.");
    const auto transferred = graph.transferWorkingNodesToSegmentation(secondParts);
    if (transferred.size() != 2) return failTest("Transferring the seeded parts failed.");
    if (!graph.workingNodes.count(parts[1]) || !graph.workingNodes.count(*mergedLabels.begin()))
        return failTest("Splitting the target changed a neighboring working node.");

    std::map<SegmentIdType, SegmentIdType> workingByComponent;
    itk::ImageRegionConstIteratorWithIndex<Image> it(before, before->GetLargestPossibleRegion());
    for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
        const auto idx = it.GetIndex();
        const auto working = base->pWorkingSegmentsImage->GetPixel(idx);
        const auto label = base->pSelectedSegmentation->GetPixel(idx);
        if (it.Get() != firstWorkingLabel) {
            if (working != it.Get() || label != selectedBefore->GetPixel(idx))
                return failTest("Split modified a voxel outside the target.");
            continue;
        }
        Image::IndexType local;
        for (unsigned axis = 0; axis < 3; ++axis) local[axis] = idx[axis] - session.globalOffset[axis];
        const auto component = preview.partition->GetPixel(local);
        const auto found = std::find(secondParts.begin(), secondParts.end(), working);
        if (!component || found == secondParts.end() || transferred[found - secondParts.begin()] != label ||
            workingByComponent.emplace(component, working).first->second != working)
            return failTest("Applied split differs from its preview.");
    }
    if (workingByComponent.size() != 2 || workingByComponent.at(1) == workingByComponent.at(2))
        return failTest("The preview components were not kept separate.");
    return checkSplitBoundaries(graph);
}

int testDisconnectedSplitEdges(bool compressed) {
    using Image = dataType::SegmentsImageType;
    constexpr int rows = 64;
    auto image = Image::New();
    image->SetRegions(Image::SizeType{{7, rows, 3}});
    image->Allocate(true);
    auto partition = sample::duplicateSegmentsImage(image);
    for (int z = 0; z < 3; ++z) {
        for (int y = 0; y < rows; ++y) {
            image->SetPixel({0, y, z}, 10 + y);  // One neighbor per row on the left.
            for (int x = 1; x <= 4; ++x) {
                image->SetPixel({x, y, z}, x <= 2 ? 1 : 2);
                partition->SetPixel({x, y, z}, x > 2 && y == 1 ? 1 : 1 + y % 3);
            }
            image->SetPixel({5, y, z}, z == 1 ? 0 : 3);  // Two separate contact patches on the right.
            image->SetPixel({6, y, z}, 3);
        }
    }
    auto base = std::make_shared<GraphBase>();
    base->pWorkingSegmentsImage = splitTestStorage(sample::duplicateSegmentsImage(image), compressed);
    Graph graph(base, false);
    base->pGraph = &graph;
    graph.setPointerToIgnoredSegmentLabels(&base->ignoredSegmentLabels);
    graph.constructFromVolume(base->pWorkingSegmentsImage);
    const auto merged = graph.mergeEdges({graph.initialLabelPairToTwoSidedInitialEdge.at({1, 2})->numId});
    std::vector<SegmentIdType> parts;
    const auto nextLabel = graph.nextFreeId;
    const auto edgeCounter = base->edgeCounter;
    const auto edgeTime = base->pEdgesInitialSegmentsImage->GetMTime();
    const auto workingTime = base->pWorkingSegmentsImage->GetMTime();
    auto missing = graph.initialNodes.extract(3);
    if (graph.splitWorkingNodeByVoxelPartition(*merged.begin(), partition, {0, 0, 0}, &parts) ||
        !parts.empty() || graph.nextFreeId != nextLabel || base->edgeCounter != edgeCounter ||
        base->pEdgesInitialSegmentsImage->GetMTime() != edgeTime || base->pWorkingSegmentsImage->GetMTime() != workingTime)
        return failTest("Rejected split changed the graph or image.");
    graph.initialNodes.insert(std::move(missing));
    base->ignoredSegmentLabels.push_back(nextLabel);

    if (!graph.splitWorkingNodeByVoxelPartition(*merged.begin(), partition, {0, 0, 0}, &parts) ||
        parts.size() != 3) return failTest("Could not apply the three-part split.");
    if (graph.initialNodes.count(nextLabel) || graph.workingNodes.count(nextLabel))
        return failTest("The split assigned an ignored label.");
    std::map<SegmentIdType, SegmentIdType> workingByPart;
    itk::ImageRegionConstIteratorWithIndex<Image> voxel(partition, partition->GetLargestPossibleRegion());
    for (voxel.GoToBegin(); !voxel.IsAtEnd(); ++voxel) {
        const auto index = voxel.GetIndex();
        const auto owner = base->pWorkingSegmentsImage->GetPixel(index);
        if (voxel.Get()) {
            if (workingByPart.emplace(voxel.Get(), owner).first->second != owner)
                return failTest("Disconnected pieces lost their common working label.");
        } else if (owner != image->GetPixel(index)) {
            return failTest("Split changed a voxel outside the three-part partition.");
        }
    }
    return checkSplitBoundaries(graph);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SegmentPuzzler"));

    ProgramOptions options;
    if (!parseArgs(argc, argv, options)) {
        return 1;
    }

    if (options.applyOnly) {
        for (const bool compressed : {false, true}) {
            if (const int result = testSplitApply(compressed)) return result;
            if (const int result = testDisconnectedSplitEdges(compressed)) return result;
        }
        return 0;
    }
    if (options.sceneOnly && !options.segmentsPath.isEmpty()) {
        using namespace segment_puzzler::app_logging;
        auto settings = AppLogger::defaultSettings();
        settings.minimumLevel = LogLevel::Debug;
        AppLogger::setSettings(settings, false);
        auto image = ITKImageLoader<SegmentIdType>(options.segmentsPath);
        auto scene = Segment3DViewerDialog::prepareAllLabelsScene(segment_puzzler::StreamVByteImageStorage<SegmentIdType>::fromImage(*image), {});
        if (scene.meshes.empty()) {
            return failTest("Input segmentation did not produce a scene.");
        }
        std::cout << "Input segmentation scene passed: " << scene.meshes.size() << " labels\n";
        return 0;
    }

    if (const int boundsTestResult = testRepresentativeVoxelBounds(); boundsTestResult != 0) {
        return boundsTestResult;
    }
    if (const int explodeTestResult = testExplodedMeshSplit(); explodeTestResult != 0) {
        return explodeTestResult;
    }
    if (const int allLabelsTestResult = testAllLabelsSceneUsesSegmentMeshes();
        allLabelsTestResult != 0) {
        return allLabelsTestResult;
    }
    if (const int directionTestResult = testSceneImageDirection(); directionTestResult != 0) {
        return directionTestResult;
    }
    if (const int result = testCompactionGrowth(); result != 0) return result;
    if (const int result = testPreparedVolumeGeometry(); result != 0) return result;

    if (options.sceneOnly) {
        std::cout << "Projected 3D scene tests passed\n";
        return 0;
    }

    if (options.segmentsPath.isEmpty()) {
        options.segmentsPath = sample::defaultSampleSegmentsPath();
    }
    if (!QFileInfo::exists(options.segmentsPath)) {
        std::cerr << "Sample segments file not found: " << options.segmentsPath.toStdString() << "\n";
        std::cerr << "Expected cached sample data in the temp directory. Run the app sample download first,\n";
        std::cerr << "or pass --segments /path/to/Watershed_MC.tif\n";
        return 1;
    }

    QString segmentsPath = options.segmentsPath;
    auto originalSegmentsImage = ITKImageLoader<dataType::SegmentIdType>(segmentsPath);
    if (originalSegmentsImage.IsNull()) {
        std::cerr << "Failed to load segments image.\n";
        return 1;
    }

    auto referenceFixture = sample::buildGraphFixture(originalSegmentsImage);
    const sample::LargestSegmentInfo largestSegment = sample::findLargestWorkingSegment(*referenceFixture.graph);
    if (largestSegment.label == 0 || largestSegment.voxelCount == 0) {
        std::cerr << "No working segment was found in the sample data.\n";
        return 1;
    }

    const auto candidates = sample::buildCandidateCuts(
        largestSegment,
        originalSegmentsImage,
        options.viewportSize,
        options.strokeWidthPixels);
    if (candidates.empty()) {
        std::cerr << "No cut candidates were generated.\n";
        return 1;
    }

    {
        auto failedFixture = sample::buildGraphFixture(originalSegmentsImage);
        auto failedWorkingBefore =
            sample::duplicateSegmentsImage(failedFixture.graphBase->pWorkingSegmentsImage);
        auto failedSelected =
            sample::duplicateSegmentsImage(failedFixture.graphBase->pWorkingSegmentsImage);
        failedSelected->FillBuffer(failedFixture.graph->backgroundId);
        failedFixture.graphBase->pSelectedSegmentation = segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(failedSelected);
        failedFixture.graphBase->selectedSegmentationContext->nextLabel = std::uint64_t(failedFixture.graph->getLargestIdInSegmentVolume(failedSelected)) + 1;
        auto failedSelectedBefore = sample::duplicateSegmentsImage(failedSelected);
        const SegmentIdType failedSelectedMaxBefore =
            (failedFixture.graphBase->selectedSegmentationContext->nextLabel - 1);

        Projected3DCutRequest unsuccessfulRequest = candidates.front().request;
        unsuccessfulRequest.strokePixels = {{-100.0, -100.0}, {-50.0, -50.0}};
        std::vector<SegmentIdType> unsuccessfulLabels{largestSegment.label};
        if (failedFixture.graph->splitWorkingNodeByProjected3DCut(
                unsuccessfulRequest, nullptr, &unsuccessfulLabels) ||
            !unsuccessfulLabels.empty() ||
            !imagesEqual(failedWorkingBefore, sample::duplicateSegmentsImage(failedFixture.graphBase->pWorkingSegmentsImage)) ||
            !imagesEqual(failedSelectedBefore, failedSelected) ||
            (failedFixture.graphBase->selectedSegmentationContext->nextLabel - 1) != failedSelectedMaxBefore) {
            return failTest("An unsuccessful projected cut should leave both segmentations and labels unchanged.");
        }
    }

    sample::GraphFixture successfulFixture;
    dataType::SegmentsImageType::Pointer beforeCutImage;
    sample::CandidateCut chosenCandidate;
    std::vector<SegmentIdType> cutResultWorkingLabels;
    bool foundSuccessfulCut = false;

    for (const auto &candidate : candidates) {
        auto candidateFixture = sample::buildGraphFixture(originalSegmentsImage);
        auto candidateBeforeImage = sample::duplicateSegmentsImage(candidateFixture.graphBase->pWorkingSegmentsImage);
        std::vector<SegmentIdType> candidateResultWorkingLabels;
        if (!candidateFixture.graph->splitWorkingNodeByProjected3DCut(
                candidate.request, nullptr, &candidateResultWorkingLabels)) {
            continue;
        }

        successfulFixture = std::move(candidateFixture);
        beforeCutImage = candidateBeforeImage;
        chosenCandidate = candidate;
        cutResultWorkingLabels = std::move(candidateResultWorkingLabels);
        foundSuccessfulCut = true;
        break;
    }

    if (!foundSuccessfulCut) {
        std::cerr << "No diagonal candidate split the largest sample segment.\n";
        return 1;
    }

    const auto postCutImage = sample::duplicateSegmentsImage(successfulFixture.graphBase->pWorkingSegmentsImage);
    const SegmentIdType backgroundId = successfulFixture.graph->backgroundId;
    if (countNonBackgroundVoxels(beforeCutImage, backgroundId) !=
        countNonBackgroundVoxels(postCutImage, backgroundId)) {
        return failTest("Projected cut changed the total non-background voxel count.");
    }

    const auto voxelCount = beforeCutImage->GetLargestPossibleRegion().GetNumberOfPixels();
    const auto *beforeBuffer = beforeCutImage->GetBufferPointer();
    const auto *afterBuffer = postCutImage->GetBufferPointer();
    std::size_t changedVoxelCount = 0;
    std::unordered_set<SegmentIdType> resultingWorkingLabels;
    resultingWorkingLabels.reserve(8);

    for (std::size_t index = 0; index < voxelCount; ++index) {
        const SegmentIdType beforeLabel = beforeBuffer[index];
        const SegmentIdType afterLabel = afterBuffer[index];
        if (beforeLabel == largestSegment.label) {
            if (afterLabel == backgroundId) {
                return failTest("Projected cut left background holes inside the original target segment.");
            }
            resultingWorkingLabels.insert(afterLabel);
        }
        if (beforeLabel == afterLabel) {
            continue;
        }
        ++changedVoxelCount;
        if (beforeLabel != largestSegment.label) {
            return failTest("Projected cut changed a voxel outside the original target working segment.");
        }
    }

    if (changedVoxelCount == 0) {
        return failTest("Projected cut reported success but did not change any voxel labels.");
    }
    if (resultingWorkingLabels.size() < 2) {
        return failTest("Projected cut did not produce multiple working-segment labels.");
    }
    if (resultingWorkingLabels.count(largestSegment.label) == 0) {
        return failTest("Projected cut did not reuse the original target working label.");
    }
    if (cutResultWorkingLabels.size() != resultingWorkingLabels.size() ||
        cutResultWorkingLabels.empty() ||
        cutResultWorkingLabels.front() != largestSegment.label) {
        return failTest("Projected cut did not report all result labels with the preserved target first.");
    }
    for (const SegmentIdType workingLabel : cutResultWorkingLabels) {
        if (resultingWorkingLabels.count(workingLabel) == 0) {
            return failTest("Projected cut reported a label outside the split target.");
        }
    }

    std::unordered_set<SegmentIdType> seenInitialLabels;
    seenInitialLabels.reserve(16);
    for (const SegmentIdType workingLabel : resultingWorkingLabels) {
        if (successfulFixture.graph->workingNodes.count(workingLabel) == 0) {
            return failTest("Resulting working label is missing from the graph.");
        }
        if (!labelIsConnectedInImage(postCutImage, workingLabel)) {
            return failTest("A resulting working segment is not 6-connected in the working image.");
        }

        const auto &workingNode = successfulFixture.graph->workingNodes.at(workingLabel);
        for (const auto &initialEntry : workingNode->subInitialNodes) {
            const auto inserted = seenInitialLabels.insert(initialEntry.first);
            if (!inserted.second) {
                return failTest("A resulting initial node was attached to multiple working nodes.");
            }

            const auto &initialNode = initialEntry.second;
            if (initialNode == nullptr) {
                return failTest("A resulting working node contains a null initial-node pointer.");
            }
            if (initialNode->getCurrentWorkingNodeLabel() != workingLabel) {
                return failTest("A resulting initial node reports the wrong current working label.");
            }
            if (!voxelListIsConnected(initialNode->voxels, initialNode->roi)) {
                return failTest("A resulting initial node is not 6-connected.");
            }

            SegmentIdType imageLabel = backgroundId;
            for (const Voxel &voxel : initialNode->voxels) {
                const SegmentIdType voxelLabel = postCutImage->GetPixel({voxel.x, voxel.y, voxel.z});
                if (imageLabel == backgroundId) {
                    imageLabel = voxelLabel;
                } else if (voxelLabel != imageLabel) {
                    return failTest("A resulting initial node spans multiple working-image labels.");
                }
            }
            if (imageLabel != workingLabel) {
                return failTest("A resulting initial node does not map back to its owning working label.");
            }
        }
    }

    const auto selectedMaxBeforeMissingTransfer = (successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1);
    if (!successfulFixture.graph->transferWorkingNodesToSegmentation(cutResultWorkingLabels).empty() ||
        (successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1) != selectedMaxBeforeMissingTransfer) {
        return failTest("Batch transfer without a selected segmentation should fail without consuming labels.");
    }

    auto selectedSegmentation = sample::duplicateSegmentsImage(postCutImage);
    selectedSegmentation->FillBuffer(backgroundId);
    successfulFixture.graphBase->pSelectedSegmentation = segment_puzzler::DenseImageStorage<dataType::SegmentIdType>::fromImage(selectedSegmentation);
    successfulFixture.graphBase->selectedSegmentationContext->nextLabel = std::uint64_t(successfulFixture.graph->getLargestIdInSegmentVolume(selectedSegmentation)) + 1;
    const SegmentIdType selectedMaxBeforeTransfer =
        (successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1);

    const auto assignedSegmentationLabels =
        successfulFixture.graph->transferWorkingNodesToSegmentation(cutResultWorkingLabels);
    if (assignedSegmentationLabels.size() != cutResultWorkingLabels.size()) {
        return failTest("Batch transfer did not assign one selected-segmentation label per cut result.");
    }
    for (std::size_t resultIndex = 0; resultIndex < cutResultWorkingLabels.size(); ++resultIndex) {
        const SegmentIdType expectedSegmentationLabel =
            static_cast<SegmentIdType>(selectedMaxBeforeTransfer + resultIndex + 1);
        if (assignedSegmentationLabels[resultIndex] != expectedSegmentationLabel) {
            return failTest("Batch transfer did not assign consecutive selected-segmentation labels.");
        }

        const auto &workingNode = successfulFixture.graph->workingNodes.at(cutResultWorkingLabels[resultIndex]);
        for (const auto &initialEntry : workingNode->subInitialNodes) {
            for (const Voxel &voxel : initialEntry.second->voxels) {
                if (selectedSegmentation->GetPixel({voxel.x, voxel.y, voxel.z}) != expectedSegmentationLabel) {
                    return failTest("Batch transfer did not copy a cut-result voxel into the selected segmentation.");
                }
            }
        }
    }
    if ((successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1) != assignedSegmentationLabels.back()) {
        return failTest("Batch transfer did not update the selected-segmentation maximum label.");
    }
    if ((successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1) !=
        static_cast<SegmentIdType>(selectedMaxBeforeTransfer + assignedSegmentationLabels.size())) {
        return failTest("Batch transfer did not increase the selected-segmentation maximum by the result count.");
    }

    const std::unordered_set<SegmentIdType> distinctAssignedLabels(
        assignedSegmentationLabels.begin(), assignedSegmentationLabels.end());
    if (distinctAssignedLabels.size() != assignedSegmentationLabels.size()) {
        return failTest("Cut results were not transferred under distinct selected-segmentation labels.");
    }

    std::unordered_map<SegmentIdType, SegmentIdType> selectedLabelByWorkingLabel;
    selectedLabelByWorkingLabel.reserve(cutResultWorkingLabels.size());
    for (std::size_t resultIndex = 0; resultIndex < cutResultWorkingLabels.size(); ++resultIndex) {
        selectedLabelByWorkingLabel.emplace(
            cutResultWorkingLabels[resultIndex], assignedSegmentationLabels[resultIndex]);
    }

    std::unordered_map<SegmentIdType, std::size_t> transferredVoxelCounts;
    transferredVoxelCounts.reserve(assignedSegmentationLabels.size());
    const auto *selectedBuffer = selectedSegmentation->GetBufferPointer();
    for (std::size_t voxelIndex = 0; voxelIndex < voxelCount; ++voxelIndex) {
        if (beforeBuffer[voxelIndex] != largestSegment.label) {
            if (selectedBuffer[voxelIndex] != backgroundId) {
                return failTest("Batch transfer changed a selected-segmentation voxel outside the cut target.");
            }
            continue;
        }

        const auto expectedLabelIt = selectedLabelByWorkingLabel.find(afterBuffer[voxelIndex]);
        if (expectedLabelIt == selectedLabelByWorkingLabel.end() ||
            selectedBuffer[voxelIndex] != expectedLabelIt->second) {
            return failTest("Batch transfer did not completely cover every projected-cut result voxel.");
        }
        ++transferredVoxelCounts[expectedLabelIt->second];
    }
    for (const SegmentIdType assignedLabel : assignedSegmentationLabels) {
        if (transferredVoxelCounts[assignedLabel] == 0) {
            return failTest("A projected-cut result received no voxels in the selected segmentation.");
        }
    }

    SegmentIdType missingWorkingLabel = std::numeric_limits<SegmentIdType>::max();
    while (successfulFixture.graph->workingNodes.count(missingWorkingLabel) > 0) {
        --missingWorkingLabel;
    }
    const SegmentIdType selectedMaxBeforeInvalidBatch =
        (successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1);
    const auto &firstTransferredNode =
        successfulFixture.graph->workingNodes.at(cutResultWorkingLabels.front());
    const Voxel firstTransferredVoxel = firstTransferredNode->subInitialNodes.begin()->second->voxels.front();
    const SegmentIdType firstTransferredVoxelLabelBefore =
        selectedSegmentation->GetPixel({firstTransferredVoxel.x, firstTransferredVoxel.y, firstTransferredVoxel.z});
    if (!successfulFixture.graph->transferWorkingNodesToSegmentation(
            {cutResultWorkingLabels.front(), missingWorkingLabel}).empty() ||
        (successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1) != selectedMaxBeforeInvalidBatch ||
        selectedSegmentation->GetPixel(
            {firstTransferredVoxel.x, firstTransferredVoxel.y, firstTransferredVoxel.z}) !=
            firstTransferredVoxelLabelBefore) {
        return failTest("An invalid batch should fail before mutating labels or selected-segmentation voxels.");
    }
    if (!successfulFixture.graph->transferWorkingNodesToSegmentation(
            {cutResultWorkingLabels.front(), cutResultWorkingLabels.front()}).empty() ||
        (successfulFixture.graphBase->selectedSegmentationContext->nextLabel - 1) != selectedMaxBeforeInvalidBatch) {
        return failTest("A duplicate-label batch should fail without consuming selected-segmentation labels.");
    }

    std::cout << "Projected 3D cut test passed.\n";
    std::cout << "Chosen candidate: " << chosenCandidate.name.toStdString() << "\n";
    std::cout << "Largest segment label: " << largestSegment.label
              << " voxels=" << largestSegment.voxelCount << "\n";
    std::cout << "Changed voxels: " << changedVoxelCount << "\n";
    std::cout << "Resulting working labels:";
    for (const SegmentIdType workingLabel : resultingWorkingLabels) {
        std::cout << " " << workingLabel;
    }
    std::cout << "\n";

    return 0;
}
