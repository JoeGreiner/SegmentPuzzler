#ifndef HELLOWORLD_ORTHOVIEWER_H
#define HELLOWORLD_ORTHOVIEWER_H


#include <QMainWindow>
#include <QDockWidget>
#include <QScrollArea>
#include <QEvent>
#include <QWheelEvent>
#include <QSplitter>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QHash>
#include <QSet>
#include "src/viewers/AnnotationSliceViewer.h"
#include "itkSignal.h"

class TaskRunner;
class VisibleRectOverlay;
class ShortcutLegendWidget;

class QScrollAreaNoWheel : public QScrollArea {
Q_OBJECT
public:
    void setIndicatorMargins(int left, int top, int right, int bottom) {
        setViewportMargins(left, top, right, bottom);
    }

    QMargins getIndicatorMargins() const {
        return viewportMargins();
    }

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
    enum class ShortcutLegendProfile {
        Default,
        Watershed
    };

    OrthoViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn, QWidget *parent = 0);

    ~OrthoViewer();

    void addSignal(itkSignalBase *signal);
    void removeSignal(itkSignalBase *signal);
    void refreshViewers();
    bool isBusy() const;
    TaskRunner *getTaskRunner() const;

    void refreshZoomLayout();
    double computeFittedZoom() const;
    void refreshInteractionModeIndicators();
    void flashShortcutLegendKey(const QString &shortcutId);
    void setShortcutLegendProfile(ShortcutLegendProfile profile);


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
    void setMorphologyOpeningRadius(int radius);
    void setMorphologyClosingRadius(int radius);
    void setMorphologyDilationRadius(int radius);
    void setMorphologyErosionRadius(int radius);
    void centerViewportsToXYZImageSpace(int x, int y, int z);
    void centerViewportsToXYViewportSpace(QScrollArea* scrollArea,
                                                       double xWanted,
                                                       double yWanted,
                                                       double zoomFactor);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void initialize();
    void onViewportResized();
    void placeSplittersForZoom(double zoom);
    void reclaimBoundarySlack();
    void adjustSplittersForCurrentZoom();
    void updateExternalScrollBars();
    void updatePlaneIndicators();
    void schedulePlaneIndicatorRefresh();
    bool hasCollapsedOrthoPane() const;

    bool initialized;

    QSplitter *splitterVertical;
    QLinkedSplitter *splitterHorizontalBottom;
    QLinkedSplitter *splitterHorizontalTop;

    QVBoxLayout *splitterLayout;

    QWidget *viewXY;
    QWidget *viewXZ;
    QWidget *viewZY;

    QGridLayout *layoutXY;
    QGridLayout *layoutXZ;
    QGridLayout *layoutZY;

    QSlider *sliderXY;
    QSlider *sliderXZ;
    QSlider *sliderZY;

    QScrollBar *externalTopScrollBarXY;
    QScrollBar *externalBottomScrollBarXY;
    QScrollBar *externalLeftScrollBarXY;
    QScrollBar *externalRightScrollBarXY;

    QScrollBar *externalTopScrollBarXZ;
    QScrollBar *externalBottomScrollBarXZ;
    QScrollBar *externalLeftScrollBarXZ;
    QScrollBar *externalRightScrollBarXZ;

    QScrollBar *externalTopScrollBarZY;
    QScrollBar *externalBottomScrollBarZY;
    QScrollBar *externalLeftScrollBarZY;
    QScrollBar *externalRightScrollBarZY;

    ShortcutLegendWidget *shortcutLegendWidget;
    VisibleRectOverlay *xyIndicator;
    VisibleRectOverlay *xzIndicator;
    VisibleRectOverlay *zyIndicator;
    QSet<QString> flashedShortcutIds;
    QHash<QString, int> shortcutFlashGenerations;
    ShortcutLegendProfile shortcutLegendProfile = ShortcutLegendProfile::Default;
    bool autoAdjustingSplitters = false;
};


#endif //HELLOWORLD_ORTHOVIEWER_H
