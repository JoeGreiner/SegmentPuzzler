#ifndef HELLOWORLD_ORTHOVIEWER_H
#define HELLOWORLD_ORTHOVIEWER_H


#include <QMainWindow>
#include <QDockWidget>
#include <QScrollArea>
#include <QEvent>
#include <QWheelEvent>
#include <QSplitter>
#include <QVBoxLayout>
#include "src/viewers/AnnotationSliceViewer.h"
#include "itkSignal.h"

class TaskRunner;

class QScrollAreaNoWheel : public QScrollArea {
Q_OBJECT
protected:
    void wheelEvent(QWheelEvent *event) override {
        event->ignore();
    }
};

class QLinkedSplitter : public QSplitter {
Q_OBJECT
public slots:
    void moveSplitterExt(int position, int index) {
        moveSplitter(position, index);
    }

    void moveSplitterToLinked(int position, int index) {
        // Block signals while mirroring the linked splitter to avoid cycles.
        blockSignals(true);
        moveSplitter(position, index);
        blockSignals(false);
    }

};


class OrthoViewer : public QWidget {
Q_OBJECT
public:
    OrthoViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn, QWidget *parent = 0);

    ~OrthoViewer();

    void addSignal(itkSignalBase *signal);
    void refreshViewers();
    bool isBusy() const;
    TaskRunner *getTaskRunner() const;

    void updateMaximumSizes(double zoomFactor = 1.);


    std::shared_ptr<GraphBase> graphBase;
    TaskRunner *taskRunner;

    std::mutex viewerListMutex;
    std::vector<SliceViewer *> viewerList;

    // Public for direct coordination from controllers and viewers.
    AnnotationSliceViewer *zy;
    AnnotationSliceViewer *xz;
    AnnotationSliceViewer *xy;

    QScrollAreaNoWheel *scrollAreaZY;
    QScrollAreaNoWheel *scrollAreaXZ;
    QScrollAreaNoWheel *scrollAreaXY;


signals:
    void sendStatusMessage(QString);

public slots:
    void receiveStatusMessage(QString string);
    void setViewToMiddleOfStack();
    void centerViewportsToXYZImageSpace(int x, int y, int z);
    void centerViewportsToXYViewportSpace(QScrollArea* scrollArea,
                                                       double xWanted,
                                                       double yWanted,
                                                       double zoomFactor);

private:
    void initialize();

    bool initialized;

    QSplitter *splitterVertical;
    QLinkedSplitter *splitterHorizontalBottom;
    QLinkedSplitter *splitterHorizontalTop;

    QVBoxLayout *splitterLayout;

    QWidget *viewXY;
    QWidget *viewXZ;
    QWidget *viewZY;

    QHBoxLayout *layoutXY;
    QHBoxLayout *layoutXZ;
    QHBoxLayout *layoutZY;

    QSlider *sliderXY;
    QSlider *sliderXZ;
    QSlider *sliderZY;

    QWidget *dummyWidget;
};


#endif //HELLOWORLD_ORTHOVIEWER_H
