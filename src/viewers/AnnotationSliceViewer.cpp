#include <QPainter>
#include <QtWidgets>
#ifdef USE_OMP
#include <omp.h>
#endif
#include <Qt>
#include <itkLabelGeometryImageFilter.h>
#include <itkRegionOfInterestImageFilter.h>
#include <itkBinaryBallStructuringElement.h>
#include <itkBinaryMorphologicalClosingImageFilter.h>
#include <itkBinaryMorphologicalOpeningImageFilter.h>
#include <itkBinaryThresholdImageFunction.h>
#include <itkFloodFilledImageFunctionConditionalIterator.h>
#include "AnnotationSliceViewer.h"
#include "itkImageRegionIteratorWithIndex.h"
#include <unordered_map>
#include "src/utils/utils.h"
#include "OrthoViewer.h"
#include "src/qtUtils/TaskRunner.h"


AnnotationSliceViewer::AnnotationSliceViewer(std::shared_ptr<GraphBase> graphBaseIn,
                                             TaskRunner *taskRunnerIn,
                                             QWidget *parent,
                                             bool)
    : SliceViewer(graphBaseIn, taskRunnerIn, parent) {
    if (verbose) { std::cout << "AnnotationSliceViewer: Constructor\n"; }

    paintModeIsActive = false;
    paintBoundaryModeIsActive = false;

    pThresholdedBoundaries = nullptr;

    ROISelectionModeIsActive = false;
    ROISelectionRubberBand = nullptr;

    labelOfClickedSegmentation = 0;

    rightClicked = false;

    scribbling = false;

    this->setMouseTracking(true);

}


void AnnotationSliceViewer::paintEvent(QPaintEvent *event) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    std::lock_guard<std::mutex> lock(signalListMutex);

    int topLeftX = event->rect().topLeft().x();
    int topLeftY = event->rect().topLeft().y();
    int eventWidth = event->rect().width();
    int eventHeight = event->rect().height();

    if (verbose) {
        std::cout << "AnnotationViewer: Paintevent triggered: " << event->rect().width()
                  << " x " << event->rect().height() << std::endl;
        printf("x0: %d y0: %d x1: %d y1: %d\n", topLeftX, topLeftY, eventWidth, eventHeight);
    }
    double tic = utils::tic();
//    QRectF sourceRect(topLeftX, topLeftY, eventWidth, eventHeight);
//    QRectF targetRect = sourceRect;


    QPainter painter(this);
    painter.scale(zoomFactor, zoomFactor);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

//    painter.drawImage(targetRect, backGroundImage, sourceRect);
    painter.drawImage(0, 0, backGroundImage);

    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            if (verbose) { std::cout << "AnnotationViewer: Painting new signal" << std::endl; }
//            painter.drawImage(targetRect, *(signal->getAddressSliceQImage()), sourceRect);
            painter.drawImage(0, 0, *(signal->getAddressSliceQImage()));
        }
    }

//    painter.drawImage(targetRect, annotationImage, sourceRect);
    painter.drawImage(0, 0, annotationImage);
//        painter.drawImage(event->rect(), edgeImage, event->rect());

    // update sliceIndicatorImage in own viewer (draw other indicator for other views in viewer)
    sliceIndicatorImage.fill(QColor(0, 0, 0, 0)); // erase old slice indicator image!
    for (auto *viewer : linkedViewerList) {
        drawOtherViewerSliceIndicator(viewer->getSliceAxis(), viewer->getSliceIndex());
    }
//    painter.drawImage(targetRect, sliceIndicatorImage, sourceRect);
    painter.drawImage(0, 0, sliceIndicatorImage);

    qreal dotRadius = 5/zoomFactor;
    int dotAlpha = 255;

    QColor xy_red = QColor(255, 0, 0, dotAlpha);
    QColor xz_green = QColor(0, 255, 0, dotAlpha);
    QColor yz_yellow = QColor(255, 255, 0, dotAlpha);


    switch (sliceAxis) {
        case 0:
            painter.setBrush(QBrush(xy_red));
            painter.drawEllipse(QPointF(indexVerticalIndicator+0.5, lastMouseY+0.5), dotRadius, dotRadius);
            painter.setBrush(QBrush(xz_green));
            painter.drawEllipse(QPointF(lastMouseZ+0.5, indexHorizontalIndicator+0.5), dotRadius, dotRadius);
            break;
        case 1:
            painter.setBrush(QBrush(yz_yellow));
            painter.drawEllipse(QPointF(indexVerticalIndicator+0.5, lastMouseZ+0.5), dotRadius, dotRadius);
            painter.setBrush(QBrush(xy_red));
            painter.drawEllipse(QPointF(lastMouseX+0.5, indexHorizontalIndicator+0.5), dotRadius, dotRadius);
            break;
        case 2:
            painter.setBrush(QBrush(yz_yellow));
            painter.drawEllipse(QPointF(indexVerticalIndicator+0.5, lastMouseY+0.5), dotRadius, dotRadius);
            painter.setBrush(QBrush(xz_green));
            painter.drawEllipse(QPointF(lastMouseX+0.5, indexHorizontalIndicator+0.5), dotRadius, dotRadius);
            break;
        default:
            throw (std::logic_error("SliceAxis not implemented!"));
    }


    if (verbose) {    utils::toc(tic, "AnnotationViewer PaintEvent finished: ");}

}


void AnnotationSliceViewer::keyPressEvent(QKeyEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()) {
        std::cout << "Currently Calculating, not accepting more KeyPressEvents!" << std::endl;
        return;
    }
//    std::cout << event->key() << std::endl;
    if (event->key() == Qt::Key_R) {
        if(graphBase->pWorkingSegments != nullptr) {
            graphBase->pWorkingSegments->randomizeCategoricalLUT();
            graphBase->pWorkingSegments->setLUTValueToBlack(graphBase->ignoredSegmentLabels.front());
        }
        if (graphBase->pSelectedSegmentationSignal != nullptr) {
            graphBase->pSelectedSegmentationSignal->checkAndResizeLUT(graphBase->selectedSegmentationMaxSegmentId);
            graphBase->pSelectedSegmentationSignal->randomizeCategoricalLUT();
        }
        if (graphBase->pRefinementWatershedSignal != nullptr) {
            graphBase->pRefinementWatershedSignal->randomizeCategoricalLUT();
        }
        for (auto *viewer : linkedViewerList) {
            viewer->recalculateQImages();
        }
    } else if (event->key() == Qt::Key_Plus) {
        modifyZoomInAllViewers(2);
    } else if (event->key() == Qt::Key_Minus) {
        modifyZoomInAllViewers(0.5);
    } else if (event->key() == Qt::Key_1) {
        updatePenWidthInAllViewers(5);
    } else if (event->key() == Qt::Key_2) {
        updatePenWidthInAllViewers(10);
    } else if (event->key() == Qt::Key_3) {
        updatePenWidthInAllViewers(15);
    } else if (event->key() == Qt::Key_4) {
        updatePenWidthInAllViewers(20);
    } else if (event->key() == Qt::Key_5) {
        updatePenWidthInAllViewers(25);
    } else if (event->key() == Qt::Key_6) {
        updatePenWidthInAllViewers(30);
    } else if (event->key() == Qt::Key_7) {
        updatePenWidthInAllViewers(35);
    } else if (event->key() == Qt::Key_8) {
        updatePenWidthInAllViewers(55);
    } else if (event->key() == Qt::Key_9) {
        updatePenWidthInAllViewers(75);
    } else if (event->key() == Qt::Key_0) {
        updatePenWidthInAllViewers(100);
    } else if (event->key() == Qt::Key_Up) {
        incrementSliceIndex();
    } else if (event->key() == Qt::Key_Down) {
        decrementSliceIndex();
    } else if (event->key() == Qt::Key_X) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Split;
        }
    } else if (event->key() == Qt::Key_C) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Cut;
        }
    } else if (event->key() == Qt::Key_Control) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Ctrl;
        }
    } else if (event->key() == Qt::Key_U) {
        this->exportView();
    } else if (event->key() == Qt::Key_V) {
        this->exportVideo();
    } else if (event->key() == Qt::Key_S) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Transfer;
        }
    } else if (event->key() == Qt::Key_P) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Refine;
        }
    } else if (event->key() == Qt::Key_Q) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::SelectColor;
        }
    } else if (event->key() == Qt::Key_D) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Delete;
        }
    } else if (event->key() == Qt::Key_F) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Fill;
        }
    } else if (event->key() == Qt::Key_G) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Open;
        }
    } else if (event->key() == Qt::Key_H) {
        for (auto *viewer : linkedViewerList) {
            viewer->activeTool = ToolMode::Insert;
        }
    } else if (event->key() == Qt::Key_E) {
        exportDebugInformation();
//        graphBase->pGraph->printMergeTreeToFile("mergeTree.txt");
//        graphBase->pGraph->printEdgesToFile("edges.txt");
//        graphBase->pGraph->printEdgeIdLookupToFile("edgeIds.txt");
//        graphBase->pGraph->writeInitialEdgesToFile("initialEdges.nrrd");
//    } else if(event->key() == Qt::Key_F) {
//        graphBase->pGraph->printMergeTreeToFile("mergeTree.txt");
    }
}

void AnnotationSliceViewer::exportDebugInformation() {
    std::cout << "Exporting Debug Information from AnnotationsliceViewer\n";
    graphBase->pGraph->printEdgeIdLookUpToFile("edgeIdLookup.txt");
    graphBase->pGraph->printWorkingNodesToFile("workingNodes.txt");
    graphBase->pGraph->printWorkingEdgesToFile("workingEdges.txt");
    graphBase->pGraph->printInitialNodesToFile("initialNodes.txt");
    graphBase->pGraph->printInitialTwoSidedEdgesToFile("initialTwoSidedEdges.txt");
    graphBase->pGraph->printInitialOneSidedEdgesToFile("initialOneSidedEdges.txt");
    graphBase->pGraph->ITKImageWriter<dataType::EdgeImageType>(graphBase->pEdgesInitialSegmentsImage,
                                                                "initialEdges.nrrd");
    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(graphBase->pWorkingSegmentsImage,
                                                                    "workingSegments.nrrd");
}

void AnnotationSliceViewer::keyReleaseEvent(QKeyEvent *event) {

//    std::cout << "Release: " << event->key() << "\n";
    static const std::unordered_map<int, ToolMode> keyToToolMode = {
        {Qt::Key_Control, ToolMode::Ctrl},
        {Qt::Key_S,       ToolMode::Transfer},
        {Qt::Key_P,       ToolMode::Refine},
        {Qt::Key_D,       ToolMode::Delete},
        {Qt::Key_X,       ToolMode::Split},
        {Qt::Key_C,       ToolMode::Cut},
        {Qt::Key_Q,       ToolMode::SelectColor},
        {Qt::Key_F,       ToolMode::Fill},
        {Qt::Key_G,       ToolMode::Open},
        {Qt::Key_H,       ToolMode::Insert},
    };
    auto it = keyToToolMode.find(event->key());
    if (it != keyToToolMode.end()) {
        for (auto *viewer : linkedViewerList) {
            if (viewer->activeTool == it->second)
                viewer->activeTool = ToolMode::None;
        }
    }
}


void AnnotationSliceViewer::mousePressEvent(QMouseEvent *event) {
    if (taskRunner != nullptr && taskRunner->isBusy()) {
        return;
    }
    if (graphBase->pWorkingSegmentsImage == nullptr &&
        activeTool != ToolMode::None && activeTool != ToolMode::Ctrl) {
        return;
    }
    switch (activeTool) {
    case ToolMode::None:
        if (ROISelectionModeIsActive) {
            ROISelectionOrigin = event->pos();
            if (ROISelectionRubberBand != nullptr) { delete ROISelectionRubberBand; }
            ROISelectionRubberBand = new QRubberBand(QRubberBand::Line, this);
            ROISelectionRubberBand->setGeometry(QRect(ROISelectionOrigin, QSize(1, 1)));
            ROISelectionRubberBand->show();
        } else if (event->button() == Qt::LeftButton) {
            lastPoint = QPoint(event->pos().x() / zoomFactor, event->pos().y() / zoomFactor);
            scribbling = true;
            rightClicked = false;
            drawPoint(event->pos());
        } else if (event->button() == Qt::RightButton) {
            lastPoint = QPoint(event->pos().x() / zoomFactor, event->pos().y() / zoomFactor);
            rightClicked = true;
            scribbling = true;
            drawPoint(event->pos());
        } else if (event->button() == Qt::MiddleButton) {
            old_middle_click_translate_x_pos = event->pos().x();
            old_middle_click_translate_y_pos = event->pos().y();
        }
        break;
    case ToolMode::Ctrl: {
        setAllViewersToXYZCoordinates(event->pos().x(), event->pos().y());
        int x, y, z;
        getXYZfromPixmapPos(event->pos().x(), event->pos().y(), x, y, z);
        if (sliceAxis == 0) {
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXY, x, y, zoomFactor);
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXZ, x, z, zoomFactor);
        } else if (sliceAxis == 1) {
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXY, x, y, zoomFactor);
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaZY, z, y, zoomFactor);
        } else if (sliceAxis == 2) {
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaXZ, x, z, zoomFactor);
            orthoViewer()->centerViewportsToXYViewportSpace(orthoViewer()->scrollAreaZY, z, y, zoomFactor);
        }
        break;
    }
    case ToolMode::Split:
        splitWorkingNodeIntoInitialNodes(event->pos().x(), event->pos().y());
        break;
    case ToolMode::Refine:
        refineSegmentByPosition(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->activeTool = ToolMode::None; }
        break;
    case ToolMode::Transfer:
        transferWorkingNodeToSegmentation(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->recalculateQImages(); }
        break;
    case ToolMode::Delete:
        deleteConnectedLabelFromSegmentation(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->recalculateQImages(); }
        break;
    case ToolMode::Cut:
        removeInitialSegmentFromWorkingSegmentAtClick(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->recalculateQImages(); }
        break;
    case ToolMode::SelectColor:
        getSegmentationLabelIdAtCursor(event->pos().x(), event->pos().y());
        break;
    case ToolMode::Fill:
        runFillSegmentationLabel(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->activeTool = ToolMode::None; }
        break;
    case ToolMode::Open:
        runOpenSegmentationLabel(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->activeTool = ToolMode::None; }
        break;
    case ToolMode::Insert:
        runInsertSegmentationSegmentIntoInitialSegments(event->pos().x(), event->pos().y());
        for (auto *viewer : linkedViewerList) { viewer->activeTool = ToolMode::None; }
        break;
    }

}

void AnnotationSliceViewer::runInsertSegmentationSegmentIntoInitialSegments(int posX, int posY){
    std::cout << "Running InsertSegmentationSegmentIntoInitialSegments\n";
    std::cout << "AnnotationSliceViewer::InsertSegmentationSegmentIntoInitialSegments called" << std::endl;
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Insert segment at position: %d %d %d\n", x, y, z);

    taskRunner->run(
        [this, x, y, z]() { graphBase->pGraph->transferSegmentationSegmentToInitialSegment(x, y, z); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::runOpenSegmentationLabel(int posX, int posY){
    std::cout << "Running OpenSegmentationLabel\n";
    taskRunner->run(
        [this, posX, posY]() { openSegmentationLabel(posX, posY); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::runFillSegmentationLabel(int posX, int posY){
    taskRunner->run(
        [this, posX, posY]() { fillSegmentationLabel(posX, posY); },
        [this]() {
            orthoViewer()->refreshViewers();
        });
}

// openSegmentationLabel
void AnnotationSliceViewer::openSegmentationLabel(int posX, int posY){
    double tic = utils::tic();

    double time_first_part = utils::tic("OpenSegmentationLabel first part started: ");
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Open segment at position: %d %d %d\n", x, y, z);


    // calculate labelmap
    // get roi
    // create new empty image
    // fill in 1:1 segment with floodfill
    // open
    // put back in

    dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x,y,z});
    std::cout << "Label at click position: " << labelAtClickPosition << "\n";

    if ((labelAtClickPosition == 0) | (labelAtClickPosition == graphBase->pGraph->backgroundId)){
        std::cout << "Label at click position: is 0 (background), not refining.\n";
        return void();
    }

    auto [fx, fy, fz, tx, ty, tz] = utils::calculateBoundingBoxForLabel(graphBase->pSelectedSegmentation, labelAtClickPosition);

    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
    ROIExtractionFilterType::Pointer ROIExtractionFilter = ROIExtractionFilterType::New();
    ROIExtractionFilter->SetInput(graphBase->pSelectedSegmentation);

    dataType::SegmentsImageType::IndexType extractedCellIndex;
    extractedCellIndex.at(0) = fx;
    extractedCellIndex.at(1) = fy;
    extractedCellIndex.at(2) = fz;

    dataType::SegmentsImageType::SizeType extracedCellSize;
    extracedCellSize.at(0) = tx - fx + 1;
    extracedCellSize.at(1) = ty - fy + 1;
    extracedCellSize.at(2) = tz - fz + 1;

    dataType::SegmentsImageType::RegionType pExtracedCellROI(extractedCellIndex, extracedCellSize);
    ROIExtractionFilter->SetRegionOfInterest(pExtracedCellROI);

    dataType::SegmentsImageType ::Pointer pExtractedCell = ROIExtractionFilter->GetOutput();
    ROIExtractionFilter->Update();
    utils::toc(time_first_part, "OpenSegmentationLabel first part finished: ");

    double time_second_part = utils::tic("OpenSegmentationLabel second part started: ");
    using StructuringElementType = itk::BinaryBallStructuringElement<dataType::SegmentIdType , dataType::Dimension>;
    StructuringElementType structuringElement;
    structuringElement.SetRadius(3);
    structuringElement.CreateStructuringElement();

    using BinaryMorphologicalOpeningImageFilterType =
            itk::BinaryMorphologicalOpeningImageFilter<dataType::SegmentsImageType , dataType::SegmentsImageType, StructuringElementType>;
    BinaryMorphologicalOpeningImageFilterType::Pointer openingFilter = BinaryMorphologicalOpeningImageFilterType::New();
    openingFilter->SetInput(pExtractedCell);
    openingFilter->SetKernel(structuringElement);
    openingFilter->SetForegroundValue(labelAtClickPosition);
    openingFilter->Update();
    auto pExtractedCellClosed = openingFilter->GetOutput();

//    std::cout << "Safe border: " << openingFilter->GetSafeBorder() << "\n";
    utils::toc(time_second_part, "OpenSegmentationLabel second part finished: ");
//
////     try the same thing with a fast binary dil and erode
////    this is not faster!
//    auto start_manual_open = utils::tic("Manual Open started: ");
////    BinaryDilateImageFilter
//    using BinaryDilateImageFilterType = itk::BinaryDilateImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType, StructuringElementType>;
//    BinaryDilateImageFilterType::Pointer dilateFilter = BinaryDilateImageFilterType::New();
//    dilateFilter->SetInput(pExtractedCell);
//    dilateFilter->SetKernel(structuringElement);
//    dilateFilter->SetForegroundValue(labelAtClickPosition);
//    dilateFilter->Update();
//
//    using BinaryErodeImageFilterType = itk::BinaryErodeImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType, StructuringElementType>;
//    BinaryErodeImageFilterType::Pointer erodeFilter = BinaryErodeImageFilterType::New();
//    erodeFilter->SetInput(dilateFilter->GetOutput());
//    erodeFilter->SetKernel(structuringElement);
//    erodeFilter->SetForegroundValue(labelAtClickPosition);
//    erodeFilter->Update();
//
//    auto pExtractedCellClosedManual = erodeFilter->GetOutput();
//    utils::toc(start_manual_open, "Manual Open finished: ");






//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCellClosed,
//                                                                  "/home/greinerj/testClosed.nrrd");
//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCell,
//                                                                   "/home/greinerj/test.nrrd");

    // delete old label; this is only needed if more is done than closing
    itk::ImageRegionIterator<dataType::SegmentsImageType> itDelete(graphBase->pSelectedSegmentation, pExtracedCellROI);
    itDelete.GoToBegin();
    while (!itDelete.IsAtEnd()) {
        if(itDelete.Get() == labelAtClickPosition){
            itDelete.Set(graphBase->pGraph->backgroundId);
        }
        ++itDelete;
    }

    // insert new label, if d
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(pExtractedCellClosed, pExtractedCellClosed->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        if(it.Get() == labelAtClickPosition){
            dataType::SegmentsImageType::IndexType newIndex = it.GetIndex();
            newIndex[0] += fx;
            newIndex[1] += fy;
            newIndex[2] += fz;
            graphBase->pSelectedSegmentation->SetPixel(newIndex, labelAtClickPosition);
        }
        ++it;
    }

    utils::toc(tic, "OpenSegmentationLabel finished: ");
}



void AnnotationSliceViewer::fillSegmentationLabel(int posX, int posY){
    double tic = utils::tic();

    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Filling holes segments at position: %d %d %d\n", x, y, z);

    // calculate labelmap

    // get roi

    // create new empty image

    // fill in 1:1 segment with floodfill

    // close

    // put back in
    dataType::SegmentIdType labelAtClickPosition = graphBase->pSelectedSegmentation->GetPixel({x,y,z});
    std::cout << "Label at click position: " << labelAtClickPosition << "\n";

    if ((labelAtClickPosition == 0) | (labelAtClickPosition == graphBase->pGraph->backgroundId)){
        std::cout << "Label at click position: is 0 (background), not refining.\n";
        return void();
    }


    auto [fx, fy, fz, tx, ty, tz] = utils::calculateBoundingBoxForLabel(graphBase->pSelectedSegmentation, labelAtClickPosition);

    using ROIExtractionFilterType = itk::RegionOfInterestImageFilter<dataType::SegmentsImageType, dataType::SegmentsImageType>;
    ROIExtractionFilterType::Pointer ROIExtractionFilter = ROIExtractionFilterType::New();
    ROIExtractionFilter->SetInput(graphBase->pSelectedSegmentation);

    dataType::SegmentsImageType::IndexType extractedCellIndex;
    extractedCellIndex.at(0) = fx;
    extractedCellIndex.at(1) = fy;
    extractedCellIndex.at(2) = fz;

    dataType::SegmentsImageType::SizeType extracedCellSize;
    extracedCellSize.at(0) = tx - fx + 1;
    extracedCellSize.at(1) = ty - fy + 1;
    extracedCellSize.at(2) = tz - fz + 1;

    dataType::SegmentsImageType::RegionType pExtracedCellROI(extractedCellIndex, extracedCellSize);
    ROIExtractionFilter->SetRegionOfInterest(pExtracedCellROI);

    dataType::SegmentsImageType ::Pointer pExtractedCell = ROIExtractionFilter->GetOutput();
    ROIExtractionFilter->Update();

    using StructuringElementType = itk::BinaryBallStructuringElement<dataType::SegmentIdType , dataType::Dimension>;
    StructuringElementType structuringElement;
    structuringElement.SetRadius(8);
    structuringElement.CreateStructuringElement();

    using BinaryMorphologicalClosingImageFilterType =
    itk::BinaryMorphologicalClosingImageFilter<dataType::SegmentsImageType , dataType::SegmentsImageType, StructuringElementType>;
    BinaryMorphologicalClosingImageFilterType::Pointer closingFilter = BinaryMorphologicalClosingImageFilterType::New();
    closingFilter->SetInput(pExtractedCell);
    closingFilter->SetKernel(structuringElement);
    closingFilter->SetForegroundValue(labelAtClickPosition);
    closingFilter->Update();
    std::cout << "Safe border: " << closingFilter->GetSafeBorder() << "\n";

    auto pExtractedCellClosed = closingFilter->GetOutput();

//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCellClosed,
//                                                                  "/home/greinerj/testClosed.nrrd");
//    graphBase->pGraph->ITKImageWriter<dataType::SegmentsImageType>(pExtractedCell,
//                                                                   "/home/greinerj/test.nrrd");

    // delete old label; this is only needed if more is done than closing
    itk::ImageRegionIterator<dataType::SegmentsImageType> itDelete(graphBase->pSelectedSegmentation, pExtracedCellROI);
    itDelete.GoToBegin();
    while (!itDelete.IsAtEnd()) {
        if(itDelete.Get() == labelAtClickPosition){
            itDelete.Set(graphBase->pGraph->backgroundId);
        }
        ++itDelete;
    }

    // insert new label, if d
    itk::ImageRegionConstIterator<dataType::SegmentsImageType> it(pExtractedCellClosed, pExtractedCellClosed->GetLargestPossibleRegion());
    it.GoToBegin();
    while (!it.IsAtEnd()) {
        if(it.Get() == labelAtClickPosition){
            dataType::SegmentsImageType::IndexType newIndex = it.GetIndex();
            newIndex[0] += fx;
            newIndex[1] += fy;
            newIndex[2] += fz;
            graphBase->pSelectedSegmentation->SetPixel(newIndex, labelAtClickPosition);
        }
        ++it;
    }

    utils::toc(tic, "FillSegmentationLabel finished: ");
}


void AnnotationSliceViewer::refineSegmentByPosition(int posX, int posY) {
    std::cout << "AnnotationSliceViewer::refineSegmentByPosition called" << std::endl;
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Refining segments at position: %d %d %d\n", x, y, z);

    taskRunner->run(
        [this, x, y, z]() { graphBase->pGraph->refineSegmentByPosition(x, y, z); },
        [this]() {
            graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
            orthoViewer()->refreshViewers();
        });
}

void AnnotationSliceViewer::splitWorkingNodeIntoInitialNodes(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    std::cout << "Splitting workingsegment into initial nodes at position: " << x << " " << y << " " << z << "\n";
    graphBase->pGraph->splitWorkingNodeIntoInitialNodes(x, y, z);
    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    orthoViewer()->refreshViewers();
}

void AnnotationSliceViewer::removeInitialSegmentFromWorkingSegmentAtClick(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    std::cout << "Removing initialsegment from workingsegment at position: " << x << " " << y << " " << z << "\n";
    graphBase->pGraph->removeInitialNodeFromWorkingNodeAtPosition(x, y, z);

    graphBase->pEdgesInitialSegmentsITKSignal->calculateLUT();
    orthoViewer()->refreshViewers();
}

void AnnotationSliceViewer::transferWorkingNodeToSegmentation(int posX, int posY) {
    int x, y, z;
    getXYZfromPixmapPos(posX, posY, x, y, z);
    printf("Transfering WorkingNode to segmentation at position: %d %d %d\n", x, y, z);
    graphBase->pGraph->transferWorkingNodeToSegmentation(x, y, z);
}


void AnnotationSliceViewer::deleteConnectedLabelFromSegmentation(int posX, int posY) {
    double tic = utils::tic();
    int x, y, z;
    if (graphBase->pSelectedSegmentation != nullptr) {
        getXYZfromPixmapPos(posX, posY, x, y, z);
        dataType::SegmentIdType labelAtPosition = graphBase->pSelectedSegmentation->GetPixel({x, y, z});
        itk::ImageRegionIterator<dataType::SegmentsImageType> it(graphBase->pSelectedSegmentation,
                                                                  graphBase->pSelectedSegmentation->GetLargestPossibleRegion());

        dataType::SegmentIdType bgLabel = 0;
        it.GoToBegin();
        while (!it.IsAtEnd()) {
            if (it.Get() == labelAtPosition) {
                it.Set(bgLabel);
            }
            ++it;
        }
    }
    utils::toc(tic, "DeleteConnectedLabelFromSegmentation finished: ");
}



void AnnotationSliceViewer::mouseMoveEvent(QMouseEvent *event) {
    int x, y, z;
    getXYZfromPixmapPos(event->pos().x(), event->pos().y(), x, y, z);

    QString logMessage = QString("x: %1/%2 y: %3/%4 z:%5/%6 sliceAxis:%7").arg(lastMouseX).arg(getDimX() - 1)
            .arg(lastMouseY).arg(getDimY() - 1)
            .arg(lastMouseZ).arg(getDimZ() - 1)
            .arg(sliceAxis);


    for (auto &signal : signalList) {
        if (signal->getIsActive()) {
            logMessage += " " + signal->getName() + ":" + signal->getNumberOfXYZAsString(x, y, z);
        }
    }

//        logMessage.sprintf("x: %01.0d y: %01.0d z: %01.0d", lastMouseY, lastMouseY, lastMouseZ);
    sendStatusMessage(logMessage);



    for (auto *viewer : linkedViewerList) {
        viewer->updateMousePosition(x, y, z);
        viewer->updateFunction();
    }


    if (activeTool == ToolMode::Ctrl) {
        if (event->buttons() == Qt::LeftButton) {
            setAllViewersToXYZCoordinates(event->pos().x(), event->pos().y());
        }
    } else if (scribbling) {
        if (event->buttons() == Qt::LeftButton) {
            drawLineTo(event->pos());
        } else if (event->buttons() == Qt::RightButton) {
            drawLineTo(event->pos());
        }
    } else if (event->buttons() == Qt::MiddleButton) {
        int current_x = event->pos().x();
        int current_y = event->pos().y();
        double scaleFactor = 0.4;
        double delta_x = scaleFactor * (current_x - old_middle_click_translate_x_pos);
        double delta_y = scaleFactor * (current_y - old_middle_click_translate_y_pos);

        QScrollAreaNoWheel *currentScrollArea;
        if (sliceAxis == 0) {
            currentScrollArea = orthoViewer()->scrollAreaZY;
        } else if (sliceAxis == 1) { // xz
            currentScrollArea = orthoViewer()->scrollAreaXZ;
        } else if (sliceAxis == 2) {
            currentScrollArea = orthoViewer()->scrollAreaXY;
        } else {
            throw std::logic_error("slice axis not implemented");
        }

        int current_horizontal_value = currentScrollArea->horizontalScrollBar()->value();
        int current_vertical_value = currentScrollArea->verticalScrollBar()->value();
        currentScrollArea->horizontalScrollBar()->setValue(current_horizontal_value + delta_x);
        currentScrollArea->verticalScrollBar()->setValue(current_vertical_value + delta_y);


        old_middle_click_translate_x_pos = event->pos().x();
        old_middle_click_translate_y_pos = event->pos().y();

    } else if(ROISelectionModeIsActive){
        if(event->buttons() == Qt::LeftButton) {
            if(ROISelectionRubberBand != nullptr) {
                ROISelectionRubberBand->setGeometry(QRect(ROISelectionOrigin, event->pos()).normalized());
            }
        }
    }
}

void AnnotationSliceViewer::mouseReleaseEvent(QMouseEvent *event) {
    if ((event->button() == Qt::LeftButton) && scribbling) {
        drawLineTo(event->pos());
        scribbling = false;
        processAnnotationImage(annotationImage);
        annotationImage.fill(QColor(0, 0, 0, 0));
        updateFunction();
    } else if ((event->button() == Qt::RightButton) && scribbling) {
        drawLineTo(event->pos());
        scribbling = false;
        processAnnotationImage(annotationImage);
        annotationImage.fill(QColor(0, 0, 0, 0));
        updateFunction();
    } else if (ROISelectionModeIsActive) {
        if (ROISelectionRubberBand != nullptr) {
            //TODO: think about zoom
            std::cout << "x: " << ROISelectionRubberBand->x() << " y: " << ROISelectionRubberBand->y();
            std::cout << " width: " << ROISelectionRubberBand->width() << " height: "
                      << ROISelectionRubberBand->height() << "\n";
            if (sliceAxis == 0) {
                graphBase->ROI_fz = static_cast<int>((ROISelectionRubberBand->x()) / zoomFactor);
                graphBase->ROI_tz = static_cast<int>((ROISelectionRubberBand->x() + ROISelectionRubberBand->width()) /
                                                     zoomFactor);
                graphBase->ROI_fy = static_cast<int>((ROISelectionRubberBand->y()) / zoomFactor);
                graphBase->ROI_ty = static_cast<int>((ROISelectionRubberBand->y() + ROISelectionRubberBand->height()) /
                                                     zoomFactor);
                graphBase->ROI_fx = 0;
                graphBase->ROI_tx = graphBase->pWorkingSegments->getDimX();
            } else if (sliceAxis == 1) {
                graphBase->ROI_fx = static_cast<int>((ROISelectionRubberBand->x()) / zoomFactor);
                graphBase->ROI_tx = static_cast<int>((ROISelectionRubberBand->x() + ROISelectionRubberBand->width()) /
                                                     zoomFactor);
                graphBase->ROI_fz = static_cast<int>((ROISelectionRubberBand->y()) / zoomFactor);
                graphBase->ROI_tz = static_cast<int>((ROISelectionRubberBand->y() + ROISelectionRubberBand->height()) /
                                                     zoomFactor);
                graphBase->ROI_fy = 0;
                graphBase->ROI_ty = graphBase->pWorkingSegments->getDimY();
            } else if (sliceAxis == 2) {
                graphBase->ROI_fx = static_cast<int>((ROISelectionRubberBand->x()) / zoomFactor);
                graphBase->ROI_tx = static_cast<int>((ROISelectionRubberBand->x() + ROISelectionRubberBand->width()) /
                                                     zoomFactor);
                graphBase->ROI_fy = static_cast<int>((ROISelectionRubberBand->y()) / zoomFactor);
                graphBase->ROI_ty = static_cast<int>((ROISelectionRubberBand->y() + ROISelectionRubberBand->height()) /
                                                     zoomFactor);
                graphBase->ROI_fz = 0;
                graphBase->ROI_tz = graphBase->pWorkingSegments->getDimZ();
            }
            graphBase->ROI_set = true;
//            ROISelectionRubberBand->hide();
        }
    }
}

void AnnotationSliceViewer::toggleROISelectonModeIsActive() {
    if (ROISelectionModeIsActive) {
        turnROISelectonModeInactive();
    } else {
        turnROISelectonModeActive();
    }
}

void AnnotationSliceViewer::turnROISelectonModeInactive() {
    ROISelectionModeIsActive = false;
    if (ROISelectionRubberBand != nullptr) {
        ROISelectionRubberBand->hide();
    }
}


void AnnotationSliceViewer::turnROISelectonModeActive() {
    ROISelectionModeIsActive = true;
    if (ROISelectionRubberBand != nullptr) {
        ROISelectionRubberBand->show();
    }
}



void AnnotationSliceViewer::drawPoint(QPoint point) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    point.setX(point.x() / zoomFactor);
    point.setY(point.y() / zoomFactor);
    std::cout << "Drawing point: " << point.x() << " " << point.y() << "\n";

    if ((point.x() < 0) | (point.y() < 0) | (point.x() >= annotationImage.width()) | (point.y() >= annotationImage.height())) {
        return;
    }


    QPainter painter(&annotationImage);
    if (paintModeIsActive | paintBoundaryModeIsActive) {
        myPenColor = rightClicked ? Qt::black : cursorColor;
    } else {
        myPenColor = rightClicked ? Qt::red : Qt::green;
    }
    painter.setPen(QPen(myPenColor, myPenWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawPoint(point);

    // while the point is drawn on the annotationimage as normal, update() works on the scaled picture!
    int topLeftPoint_x = static_cast<int>((point.x() - myPenWidth) * zoomFactor);
    int topLeftPoint_y = static_cast<int>((point.y() - myPenWidth) * zoomFactor);
    int updateRect_width = static_cast<int>((2 * myPenWidth) * zoomFactor);
    int updateRect_height = static_cast<int>((2 * myPenWidth) * zoomFactor);
    update(QRect(topLeftPoint_x, topLeftPoint_y, updateRect_width, updateRect_height));
//    update();
}

void AnnotationSliceViewer::drawLineTo(QPoint endPoint) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());


    endPoint.setX(endPoint.x() / zoomFactor);
    endPoint.setY(endPoint.y() / zoomFactor);
    std::cout << "Drawing line to: " << endPoint.x() << " " << endPoint.y() << "\n";

    if ((endPoint.x() < 0) | (endPoint.y() < 0) | (endPoint.x() >= annotationImage.width()) | (endPoint.y() >= annotationImage.height())) {
        std::cout << "Point outside image\n";
        return;
    }

    QPainter painter(&annotationImage);
    if (paintModeIsActive | paintBoundaryModeIsActive) {
        myPenColor = rightClicked ? Qt::black : cursorColor;
    } else {
        myPenColor = rightClicked ? Qt::red : Qt::green;
    }

    painter.setPen(QPen(myPenColor, myPenWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawLine(lastPoint, endPoint);

    int rad = (myPenWidth / 2) + 2;
    update(QRect(lastPoint, endPoint).normalized()
                   .adjusted(-rad, -rad, +rad, +rad));
    lastPoint = endPoint;
}


void AnnotationSliceViewer::updatePenWidthInAllViewers(int newPenWidth) {
    orthoViewer()->xy->setPenWidth(newPenWidth);
    orthoViewer()->zy->setPenWidth(newPenWidth);
    orthoViewer()->xz->setPenWidth(newPenWidth);
}


void AnnotationSliceViewer::setPenWidth(int newPenWidth) {
    myPenWidth = newPenWidth;
    setUpCustomCursor();
}


void AnnotationSliceViewer::processAnnotationImage(QImage image) {
    if ((graphBase->pEdgesInitialSegmentsImage != nullptr) || (pThresholdedBoundaries != nullptr)) {
        //TODO: Separate function into smaller parts, make sliceblabla general

        int bytesPerPixel = 4;
        unsigned char *bits = image.bits(); // pointer to the first image data
        std::vector<unsigned char> annotationImageRed(image.width() * image.height(), 0);
        std::vector<unsigned char> annotationImageGreen(image.width() * image.height(), 0);
        std::vector<unsigned char> annotationImageBlue(image.width() * image.height(), 0);
        std::vector<unsigned char> annotationImageAlpha(image.width() * image.height(), 0);

        int x = 0;
        int y = 0;

        int offset_1;
        int offset_2;
        int offset_3;
        int vecIndex;
        for (y = 0; y < image.height(); y++) {
            offset_1 = y * image.width();
            offset_2 = offset_1 * bytesPerPixel;
            for (x = 0; x < image.width(); x++) {
                offset_3 = x * bytesPerPixel + offset_2;
                vecIndex = x + offset_1;
                annotationImageRed[vecIndex] = bits[offset_3];
                annotationImageGreen[vecIndex] = bits[1 + offset_3];
                annotationImageBlue[vecIndex] = bits[2 + offset_3];
                annotationImageAlpha[vecIndex] = bits[3 + offset_3];
            }
        }


        if (!paintModeIsActive && !paintBoundaryModeIsActive) { // edge merging/unmerging modus
            if (graphBase->pEdgesInitialSegmentsImage != nullptr) {
                std::set<unsigned int> annotatedEdgeNumIdsToMerge;
                std::set<unsigned int> annotatedEdgeNumIdsToUnmerge;

                for (y = 0; y < image.height(); y++) {
                    for (x = 0; x < image.width(); x++) {
                        int worldX, worldY, worldZ;
                        if (annotationImageRed[x + image.width() * y] == 255) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            int edgeNumId = graphBase->pEdgesInitialSegmentsImage->GetPixel({worldX, worldY, worldZ});;
                            if (edgeNumId != 0) {
                                //std::cout << "Unmerge: EdgeNumId: " << edgeNumId << " at position: " << worldX << " " << worldY << " " << worldZ << "\n";
                                annotatedEdgeNumIdsToUnmerge.insert(edgeNumId);
                            }
                        } else if (annotationImageGreen[x + image.width() * y] == 255) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            int edgeNumId = graphBase->pEdgesInitialSegmentsImage->GetPixel({worldX, worldY, worldZ});;
                            if (edgeNumId != 0) {
                                //std::cout << "Merge: EdgeNumId: " << edgeNumId << " at position: " << worldX << " " << worldY << " " << worldZ << "\n";
                                annotatedEdgeNumIdsToMerge.insert(edgeNumId);
                            }
                        }
                    }
                }

                if (!annotatedEdgeNumIdsToMerge.empty()) {
                    graphBase->pGraph->mergeEdges(annotatedEdgeNumIdsToMerge);
                }
                for (auto &annotatedEdgeId : annotatedEdgeNumIdsToMerge) {
                    graphBase->edgeStatus[annotatedEdgeId] = 2;
                }

                if (!annotatedEdgeNumIdsToUnmerge.empty()) {
                    graphBase->pGraph->unmergeEdges(annotatedEdgeNumIdsToUnmerge);
                }
                for (auto &annotatedEdgeId : annotatedEdgeNumIdsToUnmerge) {
                    graphBase->edgeStatus[annotatedEdgeId] = -2;
                }

                // Todo: Update LUT calculation for Edges
                // only update if changes were done
                if (!annotatedEdgeNumIdsToMerge.empty() || !annotatedEdgeNumIdsToUnmerge.empty()) {
                    if (!annotatedEdgeNumIdsToMerge.empty()) {
                        graphBase->pEdgesInitialSegmentsITKSignal->updateLUTEdge(annotatedEdgeNumIdsToMerge);
                    }
                    if (!annotatedEdgeNumIdsToUnmerge.empty()) {
                        graphBase->pEdgesInitialSegmentsITKSignal->updateLUTEdge(annotatedEdgeNumIdsToUnmerge);
                    }

                    for (auto *viewer : linkedViewerList) {
                        viewer->recalculateQImages();
                    }
                }
            }
        } else { // edit the segmentation modus
            if (labelOfClickedSegmentation != 0) {
                for (y = 0; y < image.height(); y++) {
                    for (x = 0; x < image.width(); x++) {
                        int worldX, worldY, worldZ;
                        unsigned char r, g, b, a;
                        r = annotationImageRed[x + image.width() * y];
                        g = annotationImageGreen[x + image.width() * y];
                        b = annotationImageBlue[x + image.width() * y];
                        a = annotationImageAlpha[x + image.width() * y];

                        // insert color
                        if (QColor(r, g, b, a) == (cursorColor)) {
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            if (paintModeIsActive) {
                                graphBase->pSelectedSegmentation->SetPixel({worldX, worldY, worldZ},
                                                                           labelOfClickedSegmentation);
                            } else {
                                if (pThresholdedBoundaries) {
                                    pThresholdedBoundaries->SetPixel({worldX, worldY, worldZ},
                                                          labelOfClickedSegmentation);
                                }
                            }
                        } else if (QColor(r, g, b, a) == Qt::black) { // delete stuff
                            getXYZfromPixmapPos(x, y, worldX, worldY, worldZ, false);
                            if (paintModeIsActive) {
                                if (graphBase->pSelectedSegmentation->GetPixel({worldX, worldY, worldZ}) ==
                                    labelOfClickedSegmentation) {
                                    graphBase->pSelectedSegmentation->SetPixel({worldX, worldY, worldZ}, 0);
                                }
                            } else {
                                if (pThresholdedBoundaries) {
                                    pThresholdedBoundaries->SetPixel({worldX, worldY, worldZ}, 0);
                                }
                            }
                        }
                    }
                }
            }
            for (auto *viewer : linkedViewerList) {
                viewer->recalculateQImages();
            }
        }
    }
}


void AnnotationSliceViewer::resetQImages() {
    if (getCurrentSliceWidth() <= 0 || getCurrentSliceHeight() <= 0) {
        qWarning() << "AnnotationSliceViewer::resetQImages() called with invalid dimensions!";
        return;
    }
    backGroundImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                             static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    backGroundImage.fill(Qt::black);

    annotationImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                             static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    annotationImage.fill(QColor(0, 0, 0, 0));
    sliceIndicatorImage = QImage(static_cast<int>(getCurrentSliceWidth()),
                                 static_cast<int>(getCurrentSliceHeight()), QImage::Format_RGBA8888);
    sliceIndicatorImage.fill(QColor(0, 0, 0, 0));
    setPixmap(QPixmap::fromImage(annotationImage));
    syncViewerSizeToImage();
}


void AnnotationSliceViewer::updateFunction() {
    //    bool veryVerbose = false; // this function is not the bottleneck, takes approx 1e-5 secs
//    bool veryVerbose = true;
//    double t=0;
//    if (veryVerbose) { t = utils::tic("PaintStart"); }
    update();
//    if (veryVerboxse) { utils::toc(t, "PaintEnd"); }
}

void AnnotationSliceViewer::togglePaintMode() {
    paintModeIsActive = !paintModeIsActive;
}

void AnnotationSliceViewer::togglePaintBoundaryMode() {
    paintBoundaryModeIsActive = !paintBoundaryModeIsActive;
}

void AnnotationSliceViewer::setPaintId(dataType::SegmentIdType){
    quint32 colorOfClickedSegment = graphBase->pSelectedSegmentationSignal->LUT[labelOfClickedSegmentation];
    unsigned char red, green, blue;
    red = (unsigned char) (colorOfClickedSegment >> 16);
    green = (unsigned char) (colorOfClickedSegment >> 8);
    blue = (unsigned char) (colorOfClickedSegment >> 0);
    cursorColor = QColor(red, green, blue);
    setUpCustomCursor();
}

void AnnotationSliceViewer::getSegmentationLabelIdAtCursor(int x, int y) {
    if (paintModeIsActive) {
        if (graphBase->pSelectedSegmentation != nullptr) {
            int xWorld, yWorld, zWorld;
            getXYZfromPixmapPos(x, y, xWorld, yWorld, zWorld);
            labelOfClickedSegmentation = graphBase->pSelectedSegmentation->GetPixel({xWorld, yWorld, zWorld});
            quint32 colorOfClickedSegment = graphBase->pSelectedSegmentationSignal->LUT[labelOfClickedSegmentation];
            unsigned char red, green, blue;
            red = (unsigned char) (colorOfClickedSegment >> 16);
            green = (unsigned char) (colorOfClickedSegment >> 8);
            blue = (unsigned char) (colorOfClickedSegment >> 0);
//        std::cout << labelOfClickedSegmentation << " " << (int)alpha << " " << (int)red << " " << (int)green << " " << (int)blue << "\n";
            cursorColor = QColor(red, green, blue);
            setUpCustomCursor();
        }
    } else if (paintBoundaryModeIsActive){
        if(pThresholdedBoundaries != nullptr) {
            int xWorld, yWorld, zWorld;
            getXYZfromPixmapPos(x, y, xWorld, yWorld, zWorld);
            labelOfClickedSegmentation = pThresholdedBoundaries->GetPixel({xWorld, yWorld, zWorld});
            quint32 colorOfClickedSegment = pThresholdedBoundariesSignal->LUT[labelOfClickedSegmentation];
            unsigned char red, green, blue;
            red = (unsigned char) (colorOfClickedSegment >> 16);
            green = (unsigned char) (colorOfClickedSegment >> 8);
            blue = (unsigned char) (colorOfClickedSegment >> 0);
//        std::cout << labelOfClickedSegmentation << " " << (int)alpha << " " << (int)red << " " << (int)green << " " << (int)blue << "\n";
            cursorColor = QColor(red, green, blue);
            setUpCustomCursor();
        }
    }
}

AnnotationSliceViewer::~AnnotationSliceViewer() {
    if (ROISelectionRubberBand != nullptr) {
        delete ROISelectionRubberBand;
    }

}
