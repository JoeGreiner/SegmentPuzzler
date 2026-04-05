#include "OrthoViewer.h"
#include "SliceViewerITKSignal.h"
#include "src/qtUtils/TaskRunner.h"
#include <QScrollBar>
#include "src/segment_handling/graphBase.h"
#include <QApplication>
#include <QScreen>
#include <QThread>
#include <mutex>

#define CHECK_IF_MAIN_THREAD True

OrthoViewer::~OrthoViewer() = default;


OrthoViewer::OrthoViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn, QWidget *parent) : QWidget(parent) {
    initialized = false;
    graphBase = graphBaseIn;
    taskRunner = taskRunnerIn;

    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    splitterLayout = new QVBoxLayout();

    splitterVertical = new QSplitter(Qt::Vertical);
    splitterHorizontalTop = new QLinkedSplitter();
    splitterHorizontalBottom = new QLinkedSplitter();

    scrollAreaZY = new QScrollAreaNoWheel();
    scrollAreaXZ = new QScrollAreaNoWheel();
    scrollAreaXY = new QScrollAreaNoWheel();

    scrollAreaXY->setAccessibleName("red");
    scrollAreaXZ->setAccessibleName("green");
    scrollAreaZY->setAccessibleName("yellow");


    scrollAreaZY->setFocusPolicy(Qt::NoFocus);
    scrollAreaXZ->setFocusPolicy(Qt::NoFocus);
    scrollAreaXY->setFocusPolicy(Qt::NoFocus);

    scrollAreaZY->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scrollAreaXZ->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scrollAreaXY->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    scrollAreaZY->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scrollAreaXZ->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scrollAreaXY->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    scrollAreaZY->setAlignment(Qt::AlignCenter);
    scrollAreaXZ->setAlignment(Qt::AlignCenter);
    scrollAreaXY->setAlignment(Qt::AlignCenter);

    dummyWidget = new QWidget();

    viewXY = new QWidget();
    viewXZ = new QWidget();
    viewZY = new QWidget();



    layoutXY = new QHBoxLayout();
    layoutXZ = new QHBoxLayout();
    layoutZY = new QHBoxLayout();

    sliderXY = new QSlider(Qt::Vertical);
    sliderXZ = new QSlider(Qt::Vertical);
    sliderZY = new QSlider(Qt::Vertical);

    sliderXY->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    sliderXZ->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    sliderZY->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    layoutXY->addWidget(sliderXY);
    layoutXY->addWidget(scrollAreaXY);
    layoutXZ->addWidget(sliderXZ);
    layoutXZ->addWidget(scrollAreaXZ);
    layoutZY->addWidget(sliderZY);
    layoutZY->addWidget(scrollAreaZY);

    viewXY->setLayout(layoutXY);
    viewXZ->setLayout(layoutXZ);
    viewZY->setLayout(layoutZY);


    splitterHorizontalTop->addWidget(viewXY);
    splitterHorizontalTop->addWidget(viewZY);
    splitterHorizontalBottom->addWidget(viewXZ);
    splitterHorizontalBottom->addWidget(dummyWidget);
    splitterVertical->addWidget(splitterHorizontalTop);
    splitterVertical->addWidget(splitterHorizontalBottom);

    unsigned char stretch_factor_left = 2;
    unsigned char stretch_factor_right = 1;

    splitterHorizontalBottom->setStretchFactor(0, stretch_factor_left);
    splitterHorizontalBottom->setStretchFactor(1, stretch_factor_right);

    splitterHorizontalTop->setStretchFactor(0, stretch_factor_left);
    splitterHorizontalTop->setStretchFactor(1, stretch_factor_right);

    splitterVertical->setStretchFactor(0, stretch_factor_left);
    splitterVertical->setStretchFactor(1, stretch_factor_right);


    // link horizontal and vertical scrollbars
    connect(scrollAreaXY->horizontalScrollBar(), &QScrollBar::valueChanged, scrollAreaXZ->horizontalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaXZ->horizontalScrollBar(), &QScrollBar::valueChanged, scrollAreaXY->horizontalScrollBar(), &QScrollBar::setValue);

    connect(scrollAreaXY->verticalScrollBar(), &QScrollBar::valueChanged, scrollAreaZY->verticalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaZY->verticalScrollBar(), &QScrollBar::valueChanged, scrollAreaXY->verticalScrollBar(), &QScrollBar::setValue);

    // link horizontal splitters, moveSplitterToLinked() blocks signals during execution to prohibit cycles
    connect(splitterHorizontalTop, &QSplitter::splitterMoved, splitterHorizontalBottom, &QLinkedSplitter::moveSplitterToLinked);
    connect(splitterHorizontalBottom, &QSplitter::splitterMoved, splitterHorizontalTop, &QLinkedSplitter::moveSplitterToLinked);

    splitterLayout->addWidget(splitterVertical);
    setLayout(splitterLayout);

    zy = new AnnotationSliceViewer(graphBase, taskRunner, this);
    xz = new AnnotationSliceViewer(graphBase, taskRunner, this);
    xy = new AnnotationSliceViewer(graphBase, taskRunner, this);

    viewerList.reserve(3);
    viewerList.push_back(xy);
    viewerList.push_back(xz);
    viewerList.push_back(zy);

    zy->setLinkedViewers(viewerList);
    xy->setLinkedViewers(viewerList);
    xz->setLinkedViewers(viewerList);
    zy->setOrthoViewer(this);
    xy->setOrthoViewer(this);
    xz->setOrthoViewer(this);

    show();

    splitterHorizontalBottom->moveSplitterExt(300, 1);
}

void OrthoViewer::refreshViewers() {
    for (auto *viewer : viewerList) {
        viewer->recalculateQImages();
    }
}

bool OrthoViewer::isBusy() const {
    return taskRunner != nullptr && taskRunner->isBusy();
}

TaskRunner *OrthoViewer::getTaskRunner() const {
    return taskRunner;
}

void OrthoViewer::updateMaximumSizes(double zoomFactor) {
    int scrollBarOffset = 36;
    int sliderOffset = 42;
    scrollAreaXY->setMaximumHeight(xy->getCurrentSliceHeight() * zoomFactor + scrollBarOffset);
    scrollAreaXY->setMaximumWidth(xy->getCurrentSliceWidth() * zoomFactor + scrollBarOffset);

    scrollAreaZY->setMaximumWidth(zy->getCurrentSliceWidth() * zoomFactor + scrollBarOffset);
    scrollAreaZY->setMaximumHeight(zy->getCurrentSliceHeight() * zoomFactor + scrollBarOffset);

    scrollAreaXZ->setMaximumWidth(xz->getCurrentSliceWidth() * zoomFactor + scrollBarOffset);
    scrollAreaXZ->setMaximumHeight(xz->getCurrentSliceHeight() * zoomFactor + scrollBarOffset);

    dummyWidget->setMaximumWidth(zy->getCurrentSliceWidth() * zoomFactor + scrollBarOffset + sliderOffset);
    dummyWidget->setMaximumHeight(xz->getCurrentSliceHeight() * zoomFactor + scrollBarOffset + sliderOffset);

    viewXY->setMaximumWidth(xy->getCurrentSliceWidth() * zoomFactor + scrollBarOffset + sliderOffset);
    viewZY->setMaximumWidth(zy->getCurrentSliceWidth() * zoomFactor + scrollBarOffset + sliderOffset);
    viewXZ->setMaximumWidth(xz->getCurrentSliceWidth() * zoomFactor + scrollBarOffset + sliderOffset);
}


void OrthoViewer::addSignal(itkSignalBase *signal) {
    std::lock_guard<std::mutex> lock(viewerListMutex);
    if (!initialized) { initialize(); }
    zy->addSignal(new SliceViewerITKSignal(signal, zy->getSliceIndex(), 0));
    xz->addSignal(new SliceViewerITKSignal(signal, xz->getSliceIndex(), 1));
    xy->addSignal(new SliceViewerITKSignal(signal, xy->getSliceIndex(), 2));

    updateMaximumSizes();
    sliderXY->setMinimum(0);
    sliderZY->setMinimum(0);
    sliderXZ->setMinimum(0);
    sliderXY->setMaximum(xy->signalList.front()->getDimZ() - 1);
    sliderZY->setMaximum(xy->signalList.front()->getDimX() - 1);
    sliderXZ->setMaximum(xy->signalList.front()->getDimY() - 1);
}

void OrthoViewer::receiveStatusMessage(QString string) {
    emit sendStatusMessage(string);
}

void OrthoViewer::initialize() {
    initialized = true;
    zy->setSliceAxis(0);
    xz->setSliceAxis(1);
    xy->setSliceAxis(2);
    scrollAreaZY->setWidget(zy);
    scrollAreaXZ->setWidget(xz);
    scrollAreaXY->setWidget(xy);

    connect(sliderXY, &QSlider::valueChanged, xy, &SliceViewer::setSliceIndex);
    connect(sliderZY, &QSlider::valueChanged, zy, &SliceViewer::setSliceIndex);
    connect(sliderXZ, &QSlider::valueChanged, xz, &SliceViewer::setSliceIndex);

    xy->setLinkedSlider(sliderXY);
    zy->setLinkedSlider(sliderZY);
    xz->setLinkedSlider(sliderXZ);

    connect(xy, &SliceViewer::sendStatusMessage, this, &OrthoViewer::receiveStatusMessage);
    connect(xz, &SliceViewer::sendStatusMessage, this, &OrthoViewer::receiveStatusMessage);
    connect(zy, &SliceViewer::sendStatusMessage, this, &OrthoViewer::receiveStatusMessage);
}

void OrthoViewer::setViewToMiddleOfStack() {
#if CHECK_IF_MAIN_THREAD
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
#endif
    xy->modifyZoomInAllViewers(1);

    xy->setSliceIndex(xy->getDimZ() / 2);
    xz->setSliceIndex(xy->getDimY() / 2);
    zy->setSliceIndex(xy->getDimX() / 2);
}

void OrthoViewer::centerViewportsToXYZImageSpace(int x, int y, int z) {
    centerViewportsToXYViewportSpace(scrollAreaXY,
                                     static_cast<double>(x),
                                     static_cast<double>(y),
                                     xy->zoomFactor);
    centerViewportsToXYViewportSpace(scrollAreaXZ,
                                     static_cast<double>(x),
                                     static_cast<double>(z),
                                     xz->zoomFactor);
    centerViewportsToXYViewportSpace(scrollAreaZY,
                                     static_cast<double>(z),
                                     static_cast<double>(y),
                                     zy->zoomFactor);
}


void OrthoViewer::centerViewportsToXYViewportSpace(QScrollArea* scrollArea,
                                                   double xWanted,
                                                   double yWanted,
                                                   double zoomFactor)
{
    if (!scrollArea)
        return;

    double centerXWanted = xWanted * zoomFactor;
    double centerYWanted = yWanted * zoomFactor;

    QRect visibleRect = scrollArea->viewport()->rect();

    QScrollBar* hBar = scrollArea->horizontalScrollBar();
    QScrollBar* vBar = scrollArea->verticalScrollBar();
    if (!hBar || !vBar)
        return;

    int leftInView   = hBar->value();
    int rightInView  = hBar->value() + visibleRect.width();
    int topInView    = vBar->value();
    int bottomInView = vBar->value() + visibleRect.height();

    bool xIsVisible = (centerXWanted >= leftInView && centerXWanted <= rightInView);
    bool yIsVisible = (centerYWanted >= topInView  && centerYWanted <= bottomInView);

    // Already visible, no scrolling needed.
    if (xIsVisible && yIsVisible) {
        return;
    }

    int desiredHScroll = static_cast<int>(centerXWanted - visibleRect.width() / 2.0);
    int desiredVScroll = static_cast<int>(centerYWanted - visibleRect.height() / 2.0);

    desiredHScroll = std::max(desiredHScroll, hBar->minimum());
    desiredHScroll = std::min(desiredHScroll, hBar->maximum());
    desiredVScroll = std::max(desiredVScroll, vBar->minimum());
    desiredVScroll = std::min(desiredVScroll, vBar->maximum());

    hBar->setValue(desiredHScroll);
    vBar->setValue(desiredVScroll);
}
