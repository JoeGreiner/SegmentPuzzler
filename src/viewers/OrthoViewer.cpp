#include "OrthoViewer.h"
#include "SliceViewerITKSignal.h"
#include <QScrollBar>
#include "src/segment_handling/graphBase.h"
#include <QApplication>
#include <QScreen>
#include <QThread>
#include <mutex>

#define CHECK_IF_MAIN_THREAD True

OrthoViewer::~OrthoViewer() {
//    clean up e.g. the added signals
}


OrthoViewer::OrthoViewer(std::shared_ptr<GraphBase> graphBaseIn, QWidget *parent) : QWidget(parent) {
    initialized = false;
    graphBase = graphBaseIn;

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
    connect(scrollAreaXY->horizontalScrollBar(), SIGNAL(valueChanged(int)), scrollAreaXZ->horizontalScrollBar(),
            SLOT(setValue(int)));
    connect(scrollAreaXZ->horizontalScrollBar(), SIGNAL(valueChanged(int)), scrollAreaXY->horizontalScrollBar(),
            SLOT(setValue(int)));

    connect(scrollAreaXY->verticalScrollBar(), SIGNAL(valueChanged(int)), scrollAreaZY->verticalScrollBar(),
            SLOT(setValue(int)));
    connect(scrollAreaZY->verticalScrollBar(), SIGNAL(valueChanged(int)), scrollAreaXY->verticalScrollBar(),
            SLOT(setValue(int)));

    // link horizontal splitters, movedsplittertolinked() blocks signals during execution to prohibit cycles
    connect(splitterHorizontalTop, SIGNAL(splitterMoved(int, int)), splitterHorizontalBottom,
            SLOT(moveSplitterToLinked(int, int)));
    connect(splitterHorizontalBottom, SIGNAL(splitterMoved(int, int)), splitterHorizontalTop,
            SLOT(moveSplitterToLinked(int, int)));

    // syncrhonize bottom horizontal splitters
//    std::cout << splitterHorizontalTop->pos().x() << " " << splitterHorizontalTop->pos().y() << "\n";



    splitterLayout->addWidget(splitterVertical);
    setLayout(splitterLayout);

    zy = new AnnotationSliceViewer(graphBase, this);
    xz = new AnnotationSliceViewer(graphBase, this);
    xy = new AnnotationSliceViewer(graphBase, this);

    viewerList.reserve(3);
    viewerList.push_back(xy);
    viewerList.push_back(xz);
    viewerList.push_back(zy);

    graphBase->viewerList.push_back(xy);
    graphBase->viewerList.push_back(xz);
    graphBase->viewerList.push_back(zy);

    zy->setLinkedViewers(viewerList);
    xy->setLinkedViewers(viewerList);
    xz->setLinkedViewers(viewerList);

    show();

//    splitterHorizontalTop->moveSplitterExt(400, 1);
    splitterHorizontalBottom->moveSplitterExt(300, 1);
}

void OrthoViewer::updateMaximumSizes(double zoomFactor) {
    //    // TODO: Layout does not really work out initially

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
    std::cout << "Added Signal!" << std::endl;
    if (!initialized) { initialize(); }
    //TODO: Isnt it wise to make it a unique ptr instead?
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
};


void OrthoViewer::printVal(int val) {
    std::cout << val << std::endl;
}

void OrthoViewer::initialize() {
    initialized = true;
//    zy = new AnnotationSliceViewer(this);
//    xz = new AnnotationSliceViewer(this);
//    xy = new AnnotationSliceViewer(this);
//    zy = new SliceViewer(this);
//    xz = new SliceViewer(this);
//    xy = new SliceViewer(this);

//    viewerList.push_back(xy);
//    viewerList.push_back(xz);
//    viewerList.push_back(zy);
//
//    zy->setLinkedViewers(viewerList);
//    xy->setLinkedViewers(viewerList);
//    xz->setLinkedViewers(viewerList);


//
//    zy->addLinkedViewers(xy);
//    zy->addLinkedViewers(xz);
//    xy->addLinkedViewers(zy);
//    xy->addLinkedViewers(xz);
//    xz->addLinkedViewers(zy);
//    xz->addLinkedViewers(xy);

    zy->setSliceAxis(0);
    xz->setSliceAxis(1);
    xy->setSliceAxis(2);
    scrollAreaZY->setWidget(zy);
    scrollAreaXZ->setWidget(xz);
    scrollAreaXY->setWidget(xy);

    connect(sliderXY, SIGNAL(valueChanged(int)), xy, SLOT(setSliceIndex(int)));
    connect(sliderZY, SIGNAL(valueChanged(int)), zy, SLOT(setSliceIndex(int)));
    connect(sliderXZ, SIGNAL(valueChanged(int)), xz, SLOT(setSliceIndex(int)));

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
    //calculate zoom in a way that the whole stack is visible
    //TODO: calculate zoom better. multiple of twos? actually consider x,y, and z?
//    QRect rec =    QGuiApplication::primaryScreen()->geometry();
//    unsigned int screenWidth = rec.width();
//    unsigned int screenHeight = rec.height();
//    double zoomX = static_cast<double>(screenWidth) / static_cast<double>(2 * xy->getDimX());
//    std::cout << "initial zoomlevel: " <<  xy->getDimX() << " " << screenWidth << " " << zoomX <<  "\n";

//    int zoomY = xy->getDimY() / screenWidth;
//    xy->modifyZoomInAllViewers(zoomX / xy->zoomFactor);
    xy->modifyZoomInAllViewers(1);

    xy->setSliceIndex(xy->getDimZ() / 2);
    xz->setSliceIndex(xy->getDimY() / 2);
    zy->setSliceIndex(xy->getDimX() / 2);
}

