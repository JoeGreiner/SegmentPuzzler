#ifndef HELLOWORLD_ANNOTATIONSLICEVIEWER_H
#define HELLOWORLD_ANNOTATIONSLICEVIEWER_H

#include <src/viewers/SliceViewer.h>
#include "src/segment_handling/graphBase.h"
#include "src/segment_handling/Graph.h"

class ROIExtractionSliceViewer : public SliceViewer {
Q_OBJECT

public:
    explicit ROIExtractionSliceViewer(std::shared_ptr<GraphBase> graphBaseIn,
                                      TaskRunner *taskRunnerIn,
                                      QWidget *parent = 0,
                                      bool verbose = true);


protected:
    void paintEvent(QPaintEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    void updateFunction() override;


};


#endif //HELLOWORLD_ANNOTATIONSLICEVIEWER_H
