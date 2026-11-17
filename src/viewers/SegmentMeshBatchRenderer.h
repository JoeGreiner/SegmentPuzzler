#ifndef SEGMENT_MESH_BATCH_RENDERER_H
#define SEGMENT_MESH_BATCH_RENDERER_H

#include "Segment3DViewerDialog.h"
#include <unordered_map>

class vtkUnsignedCharArray;

// Build and upload at most one batch per event-loop turn.
class SegmentMeshBatchRenderer {
public:
    SegmentMeshBatchRenderer(std::vector<Segment3DViewerDialog::PreparedMesh> meshes,
                             std::array<double, 3> sceneCenter);
    bool hasPendingMeshes() const { return m_nextMesh < m_meshes.size(); }
    int progress() const;
    std::pair<std::size_t, vtkIdType> visibleCounts() const;
    vtkSmartPointer<vtkActor> buildNext();
    void setExplode(double factor);
    void setVisible(dataType::SegmentIdType label, bool visible);
    void setColor(dataType::SegmentIdType label, quint32 color);
    dataType::SegmentIdType pick(int x, int y, vtkRenderer *renderer) const;

private:
    struct Range {
        std::size_t batch;
        vtkIdType firstPoint, pointCount, triangleCount;
    };
    struct Segment {
        quint32 color;
        bool visible = true;
        std::vector<Range> ranges;
    };
    struct Part {
        vtkIdType endCell;
        dataType::SegmentIdType label;
    };
    struct Batch {
        vtkSmartPointer<vtkActor> actor;
        vtkSmartPointer<vtkUnsignedCharArray> colors, visibility;
        std::vector<Part> parts;
        std::array<double, 6> bounds, offsets;
        vtkIdType visibleTriangles = 0;
    };
    void updateBounds(Batch &batch);

    std::vector<Segment3DViewerDialog::PreparedMesh> m_meshes;
    std::array<double, 3> m_center;
    std::unordered_map<dataType::SegmentIdType, Segment> m_segments;
    std::vector<Batch> m_batches;
    std::size_t m_nextMesh = 0;
    vtkIdType m_nextCell = 0, m_totalTriangles = 0, m_builtTriangles = 0;
    double m_explode = 0;
};

#endif
