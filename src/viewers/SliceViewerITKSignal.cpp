#include "SliceViewerITKSignal.h"
#include <itkImageRegionConstIteratorWithIndex.h>


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
    std::cout << "Initialize Buffer: " << std::to_string(sliceAxis) << " with ";
    std::cout << std::to_string(currentSliceHeight) << " x " << std::to_string(currentSliceWidth) << std::endl;
    size_t bufferLength = (getCurrentSliceWidth() * getCurrentSliceHeight());
    currentSignalSliceBuffer = std::make_shared<std::vector<quint32>>(bufferLength, 0);
    predictedSignalSliceBuffer = std::make_shared<std::vector<quint32>>(bufferLength, 0);
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


void SliceViewerITKSignal::calculateSliceQImages() {
    sliceQImage = pSignal->calculateSliceQImage(sliceIndex, sliceAxis, currentSignalSliceBuffer.get());
}


QImage SliceViewerITKSignal::predictSliceQImage(unsigned int predictedSliceIndexIn) {
    predictedSliceIndex = predictedSliceIndexIn;
    predictedSliceAxis = sliceAxis;
    return pSignal->calculateSliceQImage(predictedSliceIndexIn, sliceAxis, predictedSignalSliceBuffer.get());
}


void SliceViewerITKSignal::setSliceIndex(unsigned int proposedSliceIndex) {
    if (isValidSliceIndex(proposedSliceIndex)) {
        if (verbose) { std::cout << "SliceViewerITKSignal: Setting sliceIndex: " << proposedSliceIndex << std::endl; }
        sliceIndex = proposedSliceIndex;
// TODO: Remove this or make a flag, this seems buggy in some cases, therefore ive disabled it
        if (false && predictedSliceQImage.valid() && (predictedSliceAxis == sliceAxis) &&
            (predictedSliceIndex == sliceIndex)) { //predictedSliceQImage.valid() &&
            if (verbose) {
                std::cout << "SliceViewerITKSignal: Using predicted sliceindex: " << proposedSliceIndex << std::endl;
            }
            sliceQImage = predictedSliceQImage.get();
        } else {
            if (verbose) {
                std::cout << "SliceViewerITKSignal: Calculating sliceindex: " << proposedSliceIndex << std::endl;
            }
            calculateSliceQImages();
        }
    }
}

void SliceViewerITKSignal::prepareNextSliceIndexAsync(unsigned int proposedSliceIndex) {
    if (isValidSliceIndex(proposedSliceIndex)) {
        if (verbose) { std::cout << "SliceViewerITKSignal: Preparing sliceIndex: " << proposedSliceIndex << std::endl; }
        predictedSliceQImage = std::async(std::launch::async,
                                          &SliceViewerITKSignal::predictSliceQImage,
                                          this,
                                          proposedSliceIndex);
    }
}


void SliceViewerITKSignal::setSliceAxis(int proposedSliceAxis) {
    if (proposedSliceAxis <= 2 && proposedSliceAxis >= 0) {
        if (verbose) { std::cout << "SliceViewerITKSignal: sliceAxis: " << proposedSliceAxis << std::endl; }
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
    return &sliceQImage;
}

QString SliceViewerITKSignal::getName() {
    return pSignal->name;;
}

QString SliceViewerITKSignal::getNumberOfXYZAsString(int x, int y, int z) {
    return pSignal->getNumberOfXYZAsString(x, y, z);
}

