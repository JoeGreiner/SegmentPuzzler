#include "SliceViewerITKSignal.h"
#include <itkImageRegionConstIteratorWithIndex.h>
#include "src/utils/AppLogger.h"


SliceViewerITKSignal::SliceViewerITKSignal(
        itkSignalBase *pSignalIn, int sliceIndex, int sliceAxis, bool verbose) :
        sliceIndex(sliceIndex), sliceAxis(sliceAxis), verbose{verbose} {

    pSignal = pSignalIn;
    calculateImageSize();

    isActive = true;

    predictedSliceIndex = UINT_MAX;
    predictedSliceAxis = UINT_MAX;
//    predictedSliceQImage = {};

    initializeBuffer();
}


void
SliceViewerITKSignal::initializeBuffer() {
    int currentSliceHeight = getCurrentSliceHeight();
    int currentSliceWidth = getCurrentSliceWidth();
    SP_LOG_DEBUG("viewer.render",
                 QStringLiteral("Initializing slice buffer axis=%1 size=%2x%3")
                     .arg(sliceAxis)
                     .arg(currentSliceHeight)
                     .arg(currentSliceWidth));
    currentSignalSliceBuffer = std::make_shared<std::vector<quint32>>();
    predictedSignalSliceBuffer = std::make_shared<std::vector<quint32>>();
}

void
SliceViewerITKSignal::calculateImageSize() {
    dimX = pSignal->getDimX();
    dimY = pSignal->getDimY();
    dimZ = pSignal->getDimZ();
}


bool
SliceViewerITKSignal::getIsActive() {
    isActive = pSignal->getIsActive(); // gets the current isActive from the itk signal
    return isActive;
}


void SliceViewerITKSignal::calculateSliceQImages(bool reuseRawSlice) {
    visibleSliceQImage = {};
    if (pSignal->supportsVisibleLabelRendering()) {
        // Filled labels are sampled when the visible region and device scale are known.
        sliceQImage = {};
        return;
    }
    sliceQImage = pSignal->calculateSliceQImage(sliceIndex, sliceAxis, currentSignalSliceBuffer.get(), reuseRawSlice);
}


QImage SliceViewerITKSignal::predictSliceQImage(unsigned int predictedSliceIndexIn) {
    predictedSliceIndex = predictedSliceIndexIn;
    predictedSliceAxis = sliceAxis;
    return pSignal->calculateSliceQImage(predictedSliceIndexIn, sliceAxis, predictedSignalSliceBuffer.get());
}


void SliceViewerITKSignal::setSliceIndex(unsigned int proposedSliceIndex) {
    if (isValidSliceIndex(proposedSliceIndex)) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.render",
                         QStringLiteral("SliceViewerITKSignal setting sliceIndex=%1")
                             .arg(proposedSliceIndex));
        }
        sliceIndex = proposedSliceIndex;
// TODO: Remove this or make a flag, this seems buggy in some cases, therefore ive disabled it
        if (false && predictedSliceQImage.valid() && (predictedSliceAxis == sliceAxis) &&
            (predictedSliceIndex == sliceIndex)) { //predictedSliceQImage.valid() &&
            if (verbose) {
                SP_LOG_DEBUG("viewer.render",
                             QStringLiteral("SliceViewerITKSignal using predicted sliceIndex=%1")
                                 .arg(proposedSliceIndex));
            }
            sliceQImage = predictedSliceQImage.get();
        } else {
            if (verbose) {
                SP_LOG_DEBUG("viewer.render",
                             QStringLiteral("SliceViewerITKSignal calculating sliceIndex=%1")
                                 .arg(proposedSliceIndex));
            }
            calculateSliceQImages();
        }
    }
}

void SliceViewerITKSignal::prepareNextSliceIndexAsync(unsigned int proposedSliceIndex) {
    if (isValidSliceIndex(proposedSliceIndex)) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.render",
                         QStringLiteral("SliceViewerITKSignal preparing sliceIndex=%1")
                             .arg(proposedSliceIndex));
        }
        predictedSliceQImage = std::async(std::launch::async,
                                          &SliceViewerITKSignal::predictSliceQImage,
                                          this,
                                          proposedSliceIndex);
    }
}


void SliceViewerITKSignal::setSliceAxis(int proposedSliceAxis) {
    if (proposedSliceAxis <= 2 && proposedSliceAxis >= 0) {
        if (verbose) {
            SP_LOG_DEBUG("viewer.render",
                         QStringLiteral("SliceViewerITKSignal setting sliceAxis=%1")
                             .arg(proposedSliceAxis));
        }
        sliceAxis = proposedSliceAxis;
        calculateSliceQImages();
    } else {
        throw std::logic_error("sliceAxis not implemented!");
    }
}


bool SliceViewerITKSignal::isValidSliceIndex(unsigned int proposedSliceIndex) {
    return proposedSliceIndex < slice_geometry::sliceLimit(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

int SliceViewerITKSignal::getCurrentSliceWidth() {
    return slice_geometry::sliceWidth(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

int SliceViewerITKSignal::getCurrentSliceHeight() {
    return slice_geometry::sliceHeight(sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

unsigned long SliceViewerITKSignal::getDimX() {
    return dimX;
}

unsigned long SliceViewerITKSignal::getDimY() {
    return dimY;
}

unsigned long SliceViewerITKSignal::getDimZ() {
    return dimZ;
}

unsigned long SliceViewerITKSignal::getPixMapIndex(itk::Index<3> coords) {
    return slice_geometry::pixmapIndex(coords, sliceAxis, slice_geometry::makeDimensions(dimX, dimY, dimZ));
}

QImage *SliceViewerITKSignal::getAddressSliceQImage() {
    if (sliceQImage.isNull())
        sliceQImage = pSignal->calculateSliceQImage(sliceIndex, sliceAxis, currentSignalSliceBuffer.get());
    return &sliceQImage;
}

bool SliceViewerITKSignal::drawVisibleSlice(QPainter &painter, const QRect &targetRect,
                                           const QRectF &visibleRect) {
    const auto transform = painter.deviceTransform();
    if (!pSignal->supportsVisibleLabelRendering() || transform.type() > QTransform::TxScale ||
        transform.m11() <= 0 || transform.m22() <= 0 ||
        painter.testRenderHint(QPainter::SmoothPixmapTransform)) return false;
    const auto deviceTarget = transform.mapRect(QRectF(targetRect));
    const auto deviceRect = deviceTarget.intersected(transform.mapRect(visibleRect)).toAlignedRect();
    if (deviceRect.isEmpty()) return true;
    const int width = getCurrentSliceWidth(), height = getCurrentSliceHeight();
    if (qint64(deviceRect.width()) * deviceRect.height() >= qint64(width) * height ||
        width >= 0xffffff || height >= 0xffffff) return false;
    const auto relativeTarget = deviceTarget.translated(-deviceRect.topLeft());
    if (visibleSliceQImage.size() != deviceRect.size() || visibleTargetRect != relativeTarget) {
        // Let Qt select the source indices, preserving its nearest-neighbour rounding
        // at fractional zoom/DPI. ARGB32 matches the full slice's raster path.
        const auto coordinates = [](int sourceSize, int targetSize, qreal offset, qreal extent, bool vertical) {
            QImage source(vertical ? 1 : sourceSize, vertical ? sourceSize : 1, QImage::Format_ARGB32);
            QImage target(vertical ? 1 : targetSize, vertical ? targetSize : 1, QImage::Format_ARGB32);
            for (int i = 0; i < sourceSize; ++i)
                source.setPixel(vertical ? 0 : i, vertical ? i : 0, 0xff000000U | (i + 1));
            target.fill(0);
            QPainter mapping(&target);
            mapping.setCompositionMode(QPainter::CompositionMode_Source);
            mapping.drawImage(vertical ? QRectF(0, offset, 1, extent) : QRectF(offset, 0, extent, 1), source);
            mapping.end();
            std::vector<int> indices(targetSize);
            for (int i = 0; i < targetSize; ++i)
                indices[i] = int(target.pixel(vertical ? 0 : i, vertical ? i : 0) & 0xffffffU) - 1;
            return indices;
        };
        const auto columns = coordinates(width, deviceRect.width(), relativeTarget.x(), relativeTarget.width(), false);
        const auto rows = coordinates(height, deviceRect.height(), relativeTarget.y(), relativeTarget.height(), true);
        visibleSliceQImage = pSignal->calculateVisibleLabelImage(sliceIndex, sliceAxis, columns, rows);
        visibleTargetRect = relativeTarget;
    }
    painter.drawImage(transform.inverted().mapRect(QRectF(deviceRect)), visibleSliceQImage);
    return true;
}

QString SliceViewerITKSignal::getName() {
    return pSignal->name;;
}

QString SliceViewerITKSignal::getNumberOfXYZAsString(int x, int y, int z) {
    return pSignal->getNumberOfXYZAsString(x, y, z);
}
