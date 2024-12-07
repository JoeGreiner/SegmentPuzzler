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
        std::cout << position << " " << index << "\n";

        // to prohibit cyclic calls to movesplitter, receiver is blocked from emitting signals
        blockSignals(true);
        moveSplitter(position, index);
        blockSignals(false);
    }

};


class OrthoViewer : public QWidget {
Q_OBJECT
public:
    OrthoViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent = 0);

    ~OrthoViewer();

    void addSignal(itkSignalBase *signal);

    void updateMaximumSizes(double zoomFactor = 1.);


    std::shared_ptr<GraphBase> graphBase;

    std::mutex viewerListMutex;
    std::vector<SliceViewer *> viewerList;

    // public to be callable from GraphBase
    // TODO: Make better/cleaner
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
    void printVal(int val);
    void setViewToMiddleOfStack();


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
