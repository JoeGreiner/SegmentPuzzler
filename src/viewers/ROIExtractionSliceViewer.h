#ifndef HELLOWORLD_ANNOTATIONSLICEVIEWER_H
#define HELLOWORLD_ANNOTATIONSLICEVIEWER_H

#include <src/viewers/SliceViewer.h>
#include "src/segment_handling/graphBase.h"
#include "src/segment_handling/graph.h"

class ROIExtractionSliceViewer : public SliceViewer {
Q_OBJECT

public:
    explicit ROIExtractionSliceViewer(QWidget *parent = 0, bool verbose = True);


protected:
    void paintEvent(QPaintEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    void updateFunction() override;


};


#endif //HELLOWORLD_ANNOTATIONSLICEVIEWER_H
