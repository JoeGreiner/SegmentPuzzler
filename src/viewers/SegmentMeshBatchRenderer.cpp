#include "SegmentMeshBatchRenderer.h"
#include "MeshPointIdMap.h"

#include <vtkBoundingBox.h>
#include <vtkCellArray.h>
#include <vtkDataObject.h>
#include <vtkFloatArray.h>
#include <vtkHardwareSelector.h>
#include <vtkIdTypeArray.h>
#include <vtkInformation.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLActor.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkOpenGLVertexBufferObject.h>
#include <vtkPointData.h>
#include <vtkProperty.h>
#include <vtkSelection.h>
#include <vtkSelectionNode.h>
#include <vtkShaderProperty.h>
#include <vtkTypeInt32Array.h>
#include <vtkUniforms.h>
#include <vtkUnsignedCharArray.h>

namespace {
// Limit picking IDs to 24 bits and each GPU buffer to 256 MiB,
// including meshes with three distinct vertices per triangle.
constexpr vtkIdType maximumTriangles = 4'000'000;
constexpr std::size_t maximumSegments = 256;

class ExplodedMeshActor : public vtkOpenGLActor {
public:
    static ExplodedMeshActor *New();
    vtkTypeMacro(ExplodedMeshActor, vtkOpenGLActor);
    using vtkOpenGLActor::GetBounds;
    double *GetBounds() override { return Bounds; }
    void setBounds(const double bounds[6]) { std::copy_n(bounds, 6, Bounds); }
};
vtkStandardNewMacro(ExplodedMeshActor);
}

SegmentMeshBatchRenderer::SegmentMeshBatchRenderer(
    std::vector<Segment3DViewerDialog::PreparedMesh> meshes, std::array<double, 3> sceneCenter)
    : m_meshes(std::move(meshes)), m_center(sceneCenter) {
    for (const auto &mesh : m_meshes) {
        m_segments.emplace(mesh.labelId, Segment{mesh.lutColor});
        m_totalTriangles += mesh.polyData->GetNumberOfPolys();
    }
}

int SegmentMeshBatchRenderer::progress() const {
    return m_totalTriangles ? static_cast<int>(100 * m_builtTriangles / m_totalTriangles) : 100;
}

std::pair<std::size_t, vtkIdType> SegmentMeshBatchRenderer::visibleCounts() const {
    std::pair<std::size_t, vtkIdType> counts{};
    for (const auto &[label, segment] : m_segments) {
        if (!segment.visible) continue;
        bool rendered = false;
        for (const auto &range : segment.ranges) {
            if (m_batches[range.batch].actor->GetVisibility()) {
                counts.second += range.triangleCount;
                rendered = true;
            }
        }
        counts.first += rendered;
    }
    return counts;
}

vtkSmartPointer<vtkActor> SegmentMeshBatchRenderer::buildNext() {
    Batch batch;
    batch.colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    batch.colors->SetNumberOfComponents(3);
    batch.visibility = vtkSmartPointer<vtkUnsignedCharArray>::New();
    batch.visibility->SetName("segmentVisible");
    vtkNew<vtkFloatArray> positions;
    positions->SetNumberOfComponents(3);
    vtkNew<vtkFloatArray> offsets;
    offsets->SetName("explodeOffset");
    offsets->SetNumberOfComponents(3);
    vtkNew<vtkTypeInt32Array> connectivity;
    vtkBoundingBox bounds, offsetBounds;
    PointIdMap pointMap;
    vtkIdType cells = 0;
    while (hasPendingMeshes() && cells < maximumTriangles && batch.parts.size() < maximumSegments) {
        auto &mesh = m_meshes[m_nextMesh];
        auto &segment = m_segments.at(mesh.labelId);
        const auto count = std::min(maximumTriangles - cells,
                                   mesh.polyData->GetNumberOfPolys() - m_nextCell);
        const auto firstPoint = positions->GetNumberOfTuples();
        const float offset[3] = {static_cast<float>(mesh.centerWorld[0] - m_center[0]),
                                 static_cast<float>(mesh.centerWorld[1] - m_center[1]),
                                 static_cast<float>(mesh.centerWorld[2] - m_center[2])};
        offsetBounds.AddPoint(offset[0], offset[1], offset[2]);
        const unsigned char color[3] = {static_cast<unsigned char>(qRed(segment.color)),
                                       static_cast<unsigned char>(qGreen(segment.color)),
                                       static_cast<unsigned char>(qBlue(segment.color))};
        pointMap.reset();
        for (vtkIdType cell = m_nextCell; cell < m_nextCell + count; ++cell) {
            vtkIdType size;
            const vtkIdType *ids;
            mesh.polyData->GetPolys()->GetCellAtId(cell, size, ids);
            for (int corner = 0; corner < 3; ++corner) {
                const auto [localId, inserted] = pointMap.insert(ids[corner]);
                connectivity->InsertNextValue(static_cast<vtkTypeInt32>(firstPoint + localId));
                if (inserted) {
                    double point[3];
                    mesh.polyData->GetPoint(ids[corner], point);
                    bounds.AddPoint(point);
                    // Center coordinates before converting to float to preserve local detail.
                    const float local[3] = {static_cast<float>(point[0] - m_center[0]),
                                            static_cast<float>(point[1] - m_center[1]),
                                            static_cast<float>(point[2] - m_center[2])};
                    positions->InsertNextTypedTuple(local);
                    offsets->InsertNextTypedTuple(offset);
                    batch.colors->InsertNextTypedTuple(color);
                    batch.visibility->InsertNextValue(segment.visible);
                }
            }
        }
        segment.ranges.push_back({m_batches.size(), firstPoint,
                                  positions->GetNumberOfTuples() - firstPoint, count});
        cells += count;
        batch.parts.push_back({cells, mesh.labelId});
        if (segment.visible) batch.visibleTriangles += count;
        m_nextCell += count;
        if (m_nextCell == mesh.polyData->GetNumberOfPolys()) {
            mesh.polyData = nullptr; // Geometry has been copied into the batches.
            ++m_nextMesh;
            m_nextCell = 0;
        }
    }
    positions->Squeeze();
    offsets->Squeeze();
    connectivity->Squeeze();
    batch.colors->Squeeze();
    batch.visibility->Squeeze();
    vtkNew<vtkPoints> points;
    points->SetData(positions);
    vtkNew<vtkCellArray> polys;
    polys->SetData(3, connectivity);
    vtkNew<vtkPolyData> data;
    data->SetPoints(points);
    data->SetPolys(polys);
    data->GetPointData()->SetScalars(batch.colors);
    data->GetPointData()->AddArray(batch.visibility);
    data->GetPointData()->AddArray(offsets);
    vtkNew<vtkOpenGLPolyDataMapper> mapper;
    mapper->SetInputData(data);
    mapper->StaticOn();
    mapper->SetScalarModeToUsePointData();
    mapper->SetColorModeToDirectScalars();
    mapper->SetVBOShiftScaleMethod(vtkOpenGLVertexBufferObject::DISABLE_SHIFT_SCALE);
    mapper->MapDataArrayToVertexAttribute("explodeOffset", "explodeOffset",
                                         vtkDataObject::FIELD_ASSOCIATION_POINTS, -1);
    mapper->MapDataArrayToVertexAttribute("segmentVisible", "segmentVisible",
                                         vtkDataObject::FIELD_ASSOCIATION_POINTS, -1);
    auto actor = vtkSmartPointer<ExplodedMeshActor>::New();
    actor->SetMapper(mapper);
    actor->SetPosition(m_center.data());
    actor->ForceOpaqueOn();
    actor->SetVisibility(batch.visibleTriangles > 0);
    actor->GetProperty()->SetAmbient(0.1);
    actor->GetProperty()->SetDiffuse(0.7);
    actor->GetProperty()->SetSpecular(0.3);
    actor->GetProperty()->SetSpecularPower(20.0);
    auto *shader = actor->GetShaderProperty();
    shader->AddVertexShaderReplacement("//VTK::PositionVC::Dec", true,
        "//VTK::PositionVC::Dec\nin vec3 explodeOffset;\nin float segmentVisible;\n", false);
    shader->AddVertexShaderReplacement("//VTK::CustomBegin::Impl", true,
        "vec4 explodedMC = vec4(vertexMC.xyz + explodeFactor * explodeOffset, vertexMC.w);\n", false);
    shader->AddVertexShaderReplacement("MCVCMatrix * vertexMC", false, "MCVCMatrix * explodedMC", true);
    shader->AddVertexShaderReplacement("MCDCMatrix * vertexMC", false, "MCDCMatrix * explodedMC", true);
    shader->AddVertexShaderReplacement("//VTK::CustomEnd::Impl", true,
        "if (segmentVisible < 0.5) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }\n", false);
    // Match the specular tint of individually rendered segments.
    shader->AddFragmentShaderReplacement("specularIntensity * specularColorUniform", false,
        "specularIntensity * specularColorUniform * vertexColorVSOutput.rgb", true);
    shader->GetVertexCustomUniforms()->SetUniformf("explodeFactor", static_cast<float>(m_explode));
    batch.actor = actor;
    bounds.GetBounds(batch.bounds.data());
    offsetBounds.GetBounds(batch.offsets.data());
    updateBounds(batch);
    m_batches.push_back(std::move(batch));
    m_builtTriangles += cells;
    return actor;
}

void SegmentMeshBatchRenderer::updateBounds(Batch &batch) {
    double bounds[6];
    for (int i = 0; i < 6; ++i) bounds[i] = batch.bounds[i] + m_explode * batch.offsets[i];
    static_cast<ExplodedMeshActor *>(batch.actor.GetPointer())->setBounds(bounds);
}

void SegmentMeshBatchRenderer::setExplode(double factor) {
    m_explode = factor;
    for (auto &batch : m_batches) {
        batch.actor->GetShaderProperty()->GetVertexCustomUniforms()->SetUniformf(
            "explodeFactor", static_cast<float>(factor));
        updateBounds(batch);
    }
}

void SegmentMeshBatchRenderer::setVisible(dataType::SegmentIdType label, bool visible) {
    auto &segment = m_segments.at(label);
    if (segment.visible == visible) return;
    segment.visible = visible;
    for (const auto &range : segment.ranges) {
        auto &batch = m_batches[range.batch];
        std::fill_n(batch.visibility->GetPointer(range.firstPoint), range.pointCount,
                    static_cast<unsigned char>(visible));
        batch.visibility->Modified();
        batch.visibleTriangles += visible ? range.triangleCount : -range.triangleCount;
        batch.actor->SetVisibility(batch.visibleTriangles > 0);
    }
}

void SegmentMeshBatchRenderer::setColor(dataType::SegmentIdType label, quint32 color) {
    auto &segment = m_segments.at(label);
    segment.color = color;
    for (const auto &range : segment.ranges) {
        auto *colors = m_batches[range.batch].colors.GetPointer();
        auto *values = colors->GetPointer(3 * range.firstPoint);
        for (vtkIdType i = 0; i < range.pointCount; ++i) {
            *values++ = qRed(color);
            *values++ = qGreen(color);
            *values++ = qBlue(color);
        }
        colors->Modified();
    }
}

dataType::SegmentIdType SegmentMeshBatchRenderer::pick(int x, int y, vtkRenderer *renderer) const {
    vtkNew<vtkHardwareSelector> selector;
    selector->SetRenderer(renderer);
    selector->SetFieldAssociation(vtkDataObject::FIELD_ASSOCIATION_CELLS);
    selector->SetArea(x, y, x, y);
    vtkSmartPointer<vtkSelection> selection;
    selection.TakeReference(selector->Select());
    if (!selection) return 0;
    for (unsigned int i = 0; i < selection->GetNumberOfNodes(); ++i) {
        auto *node = selection->GetNode(i);
        auto *prop = node->GetProperties()->Get(vtkSelectionNode::PROP());
        auto *ids = vtkIdTypeArray::SafeDownCast(node->GetSelectionList());
        if (!ids || !ids->GetNumberOfValues()) continue;
        for (const auto &batch : m_batches) {
            if (batch.actor != prop) continue;
            const auto cell = ids->GetValue(0);
            const auto part = std::upper_bound(batch.parts.begin(), batch.parts.end(), cell,
                [](vtkIdType id, const Part &part) { return id < part.endCell; });
            if (cell >= 0 && part != batch.parts.end()) return part->label;
        }
    }
    return 0;
}
