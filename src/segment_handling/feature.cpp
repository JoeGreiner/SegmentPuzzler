
#include "feature.h"
#include "graph.h"

// define static members

std::vector<std::unique_ptr<Feature>> FeatureList::edgeFeaturesList;
std::vector<std::unique_ptr<Feature>> FeatureList::unionFeaturesList;
std::vector<std::unique_ptr<Feature>> FeatureList::nodeFeaturesList;
bool FeatureList::GroundTruthLabelComputed = false;

unsigned long GeneralStatisticOnProbabilities::numberBinsHistogram;
unsigned long GeneralStatisticOnProbabilities::numberQuantiles;
unsigned long GeneralStatisticOnProbabilities::histogramIndexBegin;
unsigned long GeneralStatisticOnProbabilities::histogramIndexEnd;

// ======================
// ---- mean signal -----
// ======================

Feature *MeanSignal::createNew() {
    return (new MeanSignal(this->signalName));
}

MeanSignal::MeanSignal(std::string signalName) {
    this->filterName = "MeanSignal";
    this->signalName = signalName;
    this->featureNames = {"Mean_" + signalName};
    values.resize(1);
}

void MeanSignal::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    float summedSignal = 0;

    for (auto &voxel : voxels) {
        GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
        summedSignal += signalList[signalName]->GetPixel(index);
    }

    values[0] = (summedSignal / float(voxels.size()));
}

void MeanSignal::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    float summedSignal = 0;
    float numberVoxels = 0;
    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
            summedSignal += signalList[signalName]->GetPixel(index);
        }
        numberVoxels += listEntry->size();
    }

    values[0] = summedSignal / numberVoxels;
}


void MeanSignal::merge(std::vector<Voxel> &,
                       std::vector<float> &valuesA, unsigned long numberElementsA,
                       std::vector<float> &valuesB, unsigned long numberElementsB) {
    if (!values.empty()) { values.clear(); }
    values.push_back((valuesA[0] * numberElementsA + valuesB[0] * numberElementsB) /
                     float(numberElementsA + numberElementsB));
}



// ========================
// ---- overlap with Label -----
// ========================


LabelOverlap::LabelOverlap() {
    pToOtherLabelImagePointer = nullptr;
    filterName = "labelOverlap";
    signalName = "otherLabel";
    this->featureNames = {"LabelOverlap"};
    corrospondingLabelWithMostOverlap = -1;
    overlapPercentage = -1;
}

void LabelOverlap::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    std::map<unsigned int, unsigned int> labelHistogram;
    unsigned int label;
    // calculate hist
    for (auto &voxel : voxels) {
        GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
        label = pToOtherLabelImagePointer->GetPixel(index);
        if (labelHistogram.count(label)) {
            labelHistogram[label]++;
        } else {
            labelHistogram[label] = 1;
        }
    }
    // get largest bin
    unsigned int maxLabel = 0;
    unsigned int maxLabelCount = 0;
    for (auto &bin : labelHistogram) {
        if (bin.second > maxLabelCount) {
            maxLabelCount = bin.second;
            maxLabel = bin.first;
        }
    }

    // caclulate mean/percentage of voxels that are maxLabel
    corrospondingLabelWithMostOverlap = maxLabel;
    overlapPercentage = (float) maxLabelCount / (float) voxels.size();
    values.push_back(corrospondingLabelWithMostOverlap);
}

void LabelOverlap::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    std::map<unsigned int, unsigned int> labelHistogram;
    unsigned int label;
    float numberVoxels = 0;
    // calculate hist
    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
            label = pToOtherLabelImagePointer->GetPixel(index);
            if (labelHistogram.count(label)) {
                labelHistogram[label]++;
            } else {
                labelHistogram[label] = 1;
            }
        }
        numberVoxels += listEntry->size();
    }
    std::cout << "Number Voxels in Compute Labeloverlap: " << numberVoxels << std::endl;
    // get largest bin
    unsigned int maxLabel = 0;
    unsigned int maxLabelCount = 0;
    for (auto &bin : labelHistogram) {
        if (bin.second > maxLabelCount) {
            maxLabelCount = bin.second;
            maxLabel = bin.first;
        }
    }

    // caclulate mean/percentage of voxels that are maxLabel
    corrospondingLabelWithMostOverlap = maxLabel;
    overlapPercentage = (float) maxLabelCount / numberVoxels;
    values.push_back(corrospondingLabelWithMostOverlap);
}


Feature *LabelOverlap::createNew() {
    auto feature = new LabelOverlap();
    feature->setPToOtherLabelImagePointer(pToOtherLabelImagePointer);
    return (feature);
}

void LabelOverlap::merge(std::vector<Voxel> &allVoxels,
                         std::vector<float> &, unsigned long,
                         std::vector<float> &, unsigned long) {
    if (!values.empty()) { values.clear(); }
    compute(allVoxels);
}

void LabelOverlap::setPToOtherLabelImagePointer(dataType::SegmentsImageType::Pointer pToOtherLabelImagePointer) {
    LabelOverlap::pToOtherLabelImagePointer = pToOtherLabelImagePointer;
}


// ========================
// ---- mean GT Label -----
// ========================

GroundTruthLabel::GroundTruthLabel() {
    filterName = "groundTruthLabel";
    signalName = "groundTruth";
    this->featureNames = {"GTLabel"};
    FeatureList::GroundTruthLabelComputed = true;
}

void GroundTruthLabel::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    std::map<unsigned int, unsigned int> labelHistogram;
    unsigned int label;
    // calculate hist
    for (auto &voxel : voxels) {
        GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
        label = pGroundTruth->GetPixel(index);
        if (labelHistogram.count(label)) {
            labelHistogram[label]++;
        } else {
            labelHistogram[label] = 1;
        }
    }
    // get largest bin
    unsigned int maxLabel = 0;
    unsigned int maxLabelCount = 0;
    for (auto &bin : labelHistogram) {
        if (bin.second > maxLabelCount) {
            maxLabelCount = bin.second;
            maxLabel = bin.first;
        }
    }

    // caclulate mean/percentage of voxels that are maxLabel
    mostProminentGroundTruthLabel = maxLabel;
    mostProminentGroundTruthLabelPercentage = (float) maxLabelCount / (float) voxels.size();
    values.push_back(mostProminentGroundTruthLabel);
}

void GroundTruthLabel::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    std::map<unsigned int, unsigned int> labelHistogram;
    unsigned int label;
    float numberVoxels = 0;
    // calculate hist
    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
            label = pGroundTruth->GetPixel(index);
            if (labelHistogram.count(label)) {
                labelHistogram[label]++;
            } else {
                labelHistogram[label] = 1;
            }
        }
        numberVoxels += listEntry->size();
    }
    // get largest bin
    unsigned int maxLabel = 0;
    unsigned int maxLabelCount = 0;
    for (auto &bin : labelHistogram) {
        if (bin.second > maxLabelCount) {
            maxLabelCount = bin.second;
            maxLabel = bin.first;
        }
    }

    // caclulate mean/percentage of voxels that are maxLabel
    mostProminentGroundTruthLabel = maxLabel;
    mostProminentGroundTruthLabelPercentage = (float) maxLabelCount / numberVoxels;
    values.push_back(mostProminentGroundTruthLabel);
}


Feature *GroundTruthLabel::createNew() {
    return (new GroundTruthLabel());
}

void GroundTruthLabel::merge(std::vector<Voxel> &allVoxels,
                             std::vector<float> &, unsigned long,
                             std::vector<float> &, unsigned long) {
    if (!values.empty()) { values.clear(); }
    compute(allVoxels);
}



// ========================
// ---- General Statistics -----
// ========================


GeneralStatisticOnProbabilities::GeneralStatisticOnProbabilities(std::string signalName) {
    this->filterName = "GeneralStatisticOnProbabilities";
    this->featureNames = {"mean_" + signalName, "sum_" + signalName, "min_" + signalName, "max_" + signalName,
                          "variance_" + signalName, "skewness_" + signalName, "kurtosis_" + signalName,
                          "hist_0_" + signalName, "hist_1_" + signalName, "hist_2_" + signalName,
                          "hist_3_" + signalName, "hist_4_" + signalName, "hist_5_" + signalName,
                          "hist_6_" + signalName,
                          "hist_7_" + signalName, "hist_8_" + signalName, "hist_9_" + signalName,
                          "hist_10_" + signalName, "hist_11_" + signalName, "hist_12_" + signalName,
                          "hist_13_" + signalName,
                          "hist_14_" + signalName, "hist_15_" + signalName, "hist_16_" + signalName,
                          "hist_17_" + signalName, "hist_18_" + signalName, "hist_19_" + signalName,
                          "q10_" + signalName, "q25_" + signalName, "q50_" + signalName, "q75_" + signalName,
                          "q90_" + signalName};
    this->signalName = signalName;

}

Feature *GeneralStatisticOnProbabilities::createNew() {
    return (new GeneralStatisticOnProbabilities(this->signalName));
}

void GeneralStatisticOnProbabilities::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    // implemeted:
    // mean, sum, min, max
    // variance, skewness, kurtosis
    // histograms(21 bins)
    // quantiles(0.1 0.25 0.5 0.75 0.9)

    std::vector<float> intensities;

    for (auto &voxel : voxels) {
        GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
        intensities.push_back(signalList[signalName]->GetPixel(index));
    }
    float sum = 0;
    for (auto &voxel : voxels) {
        GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
        sum += signalList[signalName]->GetPixel(index);
    }
    float mean = (sum / float(intensities.size()));
    std::sort(intensities.begin(), intensities.end());
    float min = intensities.front();
    float max = intensities.back();

    std::vector<float> quantiles{};
    sortAndComputeQuantiles(intensities, quantiles);

    std::vector<float> hist = computeHistogram(intensities, 0.05);

    float variance, skewness, kurtosis;
    computeMoments(intensities, mean, variance, skewness, kurtosis);

    values = {mean, sum, min, max, variance, skewness, kurtosis};

    histogramIndexBegin = values.size();
    values.insert(values.end(), hist.begin(), hist.end());
    histogramIndexEnd = values.size() - 1;

    values.insert(values.end(), quantiles.begin(), quantiles.end());

    //TODO remove this if sure
    for (auto &value : values) {
        if (std::isnan(value)) {
            std::cout << "NaN Warning in" << value << std::endl;
        }
    }
}

void GeneralStatisticOnProbabilities::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    // implemeted:
    // mean, sum, min, max
    // variance, skewness, kurtosis
    // histograms(21 bins)
    // quantiles(0.1 0.25 0.5 0.75 0.9)

    std::vector<float> intensities;


    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
            intensities.push_back(signalList[signalName]->GetPixel(index));
        }
    }
    float sum = 0;
    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
            sum += signalList[signalName]->GetPixel(index);
        }
    }
    float mean = (sum / float(intensities.size()));
    std::sort(intensities.begin(), intensities.end());
    float min = intensities.front();
    float max = intensities.back();

    std::vector<float> quantiles{};
    sortAndComputeQuantiles(intensities, quantiles);

    std::vector<float> hist = computeHistogram(intensities, 0.05);

    float variance, skewness, kurtosis;
    computeMoments(intensities, mean, variance, skewness, kurtosis);

    values = {mean, sum, min, max, variance, skewness, kurtosis};

    histogramIndexBegin = values.size();
    values.insert(values.end(), hist.begin(), hist.end());
    histogramIndexEnd = values.size() - 1;

    values.insert(values.end(), quantiles.begin(), quantiles.end());

    //TODO remove this if sure
    for (auto &value : values) {
        if (std::isnan(value)) {
            std::cout << "NaN Warning in" << value << std::endl;
        }
    }
}


std::vector<float>
GeneralStatisticOnProbabilities::computeHistogram(const std::vector<float> &intensities, float binSize) {
    numberBinsHistogram = (unsigned int) (1 / binSize);
    std::vector<float> hist(numberBinsHistogram, 0);
    for (float intensity : intensities) {
        unsigned long binIndex = (unsigned long) std::floor(intensity / binSize);
        if (binIndex >= numberBinsHistogram) {
            binIndex = numberBinsHistogram - 1;
        }
        hist.at(binIndex) = hist.at(binIndex) + 1;
    }
    return hist;
}

void GeneralStatisticOnProbabilities::sortAndComputeQuantiles(std::vector<float> &intensities,
                                                              std::vector<float> &quantiles) {
    // compute quantiles for 0.1 0.25 0.5 0.75 0.9
    std::vector<float> quantileMarkers;
    quantileMarkers.push_back(0.1);
    quantileMarkers.push_back(0.25);
    quantileMarkers.push_back(0.5);
    quantileMarkers.push_back(0.75);
    quantileMarkers.push_back(0.9);
    numberQuantiles = quantileMarkers.size();
    for (float &i : quantileMarkers) {
        quantiles.push_back(intensities[(intensities.size()) * i]);
    }
}

void

GeneralStatisticOnProbabilities::computeMoments(const std::vector<float> &intensities, float mean, float &variance,
                                                float &skewness, float &kurtosis) const {
    variance = 0;
    skewness = 0;
    kurtosis = 0;

    for (auto &intensity : intensities) {
        float diff = intensity - mean;

        double varianceIncrement = std::pow(diff, 2); // Sum (I-M)^2
        variance += varianceIncrement;

        double skewnessIncrement = varianceIncrement * diff; // Sum (I-M)^3
        skewness += skewnessIncrement;

        double kurtosisIncrement = skewnessIncrement * diff; // Sum (I-M)^4
        kurtosis += kurtosisIncrement;
    }

    float stdDev;
    if (intensities.size() == 1) {
        variance = 0;
        stdDev = 0;
    } else {
        variance = variance / (intensities.size() - 1.); // sample size def
        stdDev = std::sqrt(variance);
    }

    if (stdDev > 0) {
        skewness = static_cast<float>(((1. / intensities.size()) * skewness) / std::pow(variance, 2. / 3.));
        kurtosis = static_cast<float>(((1. / intensities.size()) * kurtosis) / std::pow(variance, 2) - 3.);
    } else {
        skewness = 0;
        kurtosis = 0;
    };
}

void GeneralStatisticOnProbabilities::merge(std::vector<Voxel> &voxels, std::vector<float> &valuesA,
                                            unsigned long numberElementsA,
                                            std::vector<float> &valuesB, unsigned long numberElementsB) {

    if (!values.empty()) { values.clear(); }

    std::vector<float> intensities;
    for (auto &voxel : voxels) {
        GraphBase::FeatureImageType::IndexType index{voxel.x, voxel.y, voxel.z};
        intensities.push_back(signalList[signalName]->GetPixel(index));
    }

    float mean, sum, min, max, variance, skewness, kurtosis;
    mean = (valuesA[0] * numberElementsA + valuesB[0] * numberElementsB) / (numberElementsA + numberElementsB);
    sum = valuesA[1] + valuesB[1];
    min = valuesA[2] < valuesB[2] ? valuesA[2] : valuesB[2];
    max = valuesA[3] > valuesB[3] ? valuesA[3] : valuesB[3];


    if (min < 0 || max > 1) {
        throw (std::logic_error("min of prob cant be negative or greater 1"));
    }

    computeMoments(intensities, mean, variance, skewness, kurtosis);

    std::vector<float> quantiles;
    quantiles.reserve(numberQuantiles);
    sortAndComputeQuantiles(intensities, quantiles);

    std::vector<float> hist(numberBinsHistogram, 0);
    //histogram merge: add bins
    for (unsigned int i = 0; i < histogramIndexEnd - histogramIndexBegin; ++i) {
        hist[i] = valuesA[i] + valuesB[i];
    }

    values = {mean, sum, min, max, variance, skewness, kurtosis};
    values.insert(values.end(), hist.begin(), hist.end());
    values.insert(values.end(), quantiles.begin(), quantiles.end());

}




// ========================
// ----NumberOfVoxels -----
// ========================


// NumberOfVoxels Feature: Return the number of voxels in a edge/node
NumberOfVoxels::NumberOfVoxels() {
    this->filterName = "NumberOfVoxels";
    this->signalName = "Morphology";
    this->featureNames = {filterName};
    values.resize(1);
}

Feature *NumberOfVoxels::createNew() {
    return (new NumberOfVoxels());
}

void NumberOfVoxels::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    values.at(0) = voxels.size();
}

void NumberOfVoxels::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    float numberVoxels = 0;
    for (auto &listEntry : voxelList) {
        numberVoxels += listEntry->size();
    }

    values.at(0) = numberVoxels;
}

void NumberOfVoxels::merge(std::vector<Voxel> &, std::vector<float> &, unsigned long numberElementsA,
                           std::vector<float> &, unsigned long numberElementsB) {
    values.at(0) = numberElementsA + numberElementsB;

}





// ========================
// ---- PCA Ratios -----
// ========================


// NumberOfVoxels Feature: Return the number of voxels in a edge/node
PCAValues::PCAValues() {
    this->filterName = "PCAValues";
    this->signalName = "Morphology";
    this->featureNames = {"e1", "e2", "e3", "v1x", "v1y", "v1z", "v2x", "v2y", "v2z", "v3x", "v3y", "v3z"};
}

Feature *PCAValues::createNew() {
    return (new PCAValues());
}

void PCAValues::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    PCA segPCA = calcPCA(voxels); // eigenvalues are sorted, sign should be unique

    values.push_back((float) segPCA.eigenValues[0][0]);
    values.push_back((float) segPCA.eigenValues[1][1]);
    values.push_back((float) segPCA.eigenValues[2][2]);
    values.push_back((float) segPCA.eigenVectors[0][0]);
    values.push_back((float) segPCA.eigenVectors[1][0]);
    values.push_back((float) segPCA.eigenVectors[2][0]);
    values.push_back((float) segPCA.eigenVectors[0][1]);
    values.push_back((float) segPCA.eigenVectors[1][1]);
    values.push_back((float) segPCA.eigenVectors[2][1]);
    values.push_back((float) segPCA.eigenVectors[0][2]);
    values.push_back((float) segPCA.eigenVectors[1][2]);
    values.push_back((float) segPCA.eigenVectors[2][2]);
}


void PCAValues::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    PCA segPCA = calcPCA(voxelList); // eigenvalues are sorted, sign should be unique

    values.push_back((float) segPCA.eigenValues[0][0]);
    values.push_back((float) segPCA.eigenValues[1][1]);
    values.push_back((float) segPCA.eigenValues[2][2]);
    values.push_back((float) segPCA.eigenVectors[0][0]);
    values.push_back((float) segPCA.eigenVectors[1][0]);
    values.push_back((float) segPCA.eigenVectors[2][0]);
    values.push_back((float) segPCA.eigenVectors[0][1]);
    values.push_back((float) segPCA.eigenVectors[1][1]);
    values.push_back((float) segPCA.eigenVectors[2][1]);
    values.push_back((float) segPCA.eigenVectors[0][2]);
    values.push_back((float) segPCA.eigenVectors[1][2]);
    values.push_back((float) segPCA.eigenVectors[2][2]);
}


void PCAValues::merge(std::vector<Voxel> &allVoxels, std::vector<float> &, unsigned long,
                      std::vector<float> &, unsigned long) {
    if (!values.empty()) { values.clear(); }
    compute(allVoxels);
}



// ========================
// ---- PCA Ratios -----
// ========================


// NumberOfVoxels Feature: Return the number of voxels in a edge/node
PCARatios::PCARatios() {
    this->filterName = "PCARatios";
    this->signalName = "Morphology";
    this->featureNames = {"l1/l2", "l1/l3", "l2/l3"};
}

Feature *PCARatios::createNew() {
    return (new PCARatios());
}

void PCARatios::compute(std::vector<Voxel> &voxels, unsigned int, unsigned int) {
    PCA segPCA = calcPCA(voxels); // eigenvalues are sorted, sign should be unique

    double n2, n3;
    float r1, r2, r3; // avoid division by 0
    n2 = segPCA.eigenValues[1][1] == 0 ? 1e-3 : segPCA.eigenValues[1][1];
    n3 = segPCA.eigenValues[2][2] == 0 ? 1e-3 : segPCA.eigenValues[2][2];
    r1 = (float) (segPCA.eigenValues[0][0] / n2);
    r2 = (float) (segPCA.eigenValues[0][0] / n3);
    r3 = (float) (segPCA.eigenValues[1][1] / n3);

    if (std::isnan(r1)) {
        std::cout << segPCA.eigenValues[0][0] << std::endl;
        throw std::logic_error("NaN error!");
    }

    if (std::isnan(r2)) {
        std::cout << segPCA.eigenValues[1][1] << std::endl;
        throw std::logic_error("NaN error!");
    }

    if (std::isnan(r3)) {
        std::cout << segPCA.eigenValues[2][2] << std::endl;
        throw std::logic_error("NaN error!");
    }

    values.push_back(r1);
    values.push_back(r2);
    values.push_back(r3);
//  values.push_back(computeNorm(segPCA.eigenVectors,0,1));
//  values.push_back(computeNorm(segPCA.eigenVectors,0,2));
//  values.push_back(computeNorm(segPCA.eigenVectors,1,2));
}

void PCARatios::compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int, unsigned int) {
    PCA segPCA = calcPCA(voxelList); // eigenvalues are sorted, sign should be unique

    double n2, n3;
    float r1, r2, r3; // avoid division by 0
    n2 = segPCA.eigenValues[1][1] == 0 ? 1e-3 : segPCA.eigenValues[1][1];
    n3 = segPCA.eigenValues[2][2] == 0 ? 1e-3 : segPCA.eigenValues[2][2];
    r1 = (float) (segPCA.eigenValues[0][0] / n2);
    r2 = (float) (segPCA.eigenValues[0][0] / n3);
    r3 = (float) (segPCA.eigenValues[1][1] / n3);

    if (std::isnan(r1)) {
        std::cout << segPCA.eigenValues[0][0] << std::endl;
        throw std::logic_error("NaN error!");
    }

    if (std::isnan(r2)) {
        std::cout << segPCA.eigenValues[1][1] << std::endl;
        throw std::logic_error("NaN error!");
    }

    if (std::isnan(r3)) {
        std::cout << segPCA.eigenValues[2][2] << std::endl;
        throw std::logic_error("NaN error!");
    }

    values.push_back(r1);
    values.push_back(r2);
    values.push_back(r3);
//  values.push_back(computeNorm(segPCA.eigenVectors,0,1));
//  values.push_back(computeNorm(segPCA.eigenVectors,0,2));
//  values.push_back(computeNorm(segPCA.eigenVectors,1,2));
}


void PCARatios::merge(std::vector<Voxel> &allVoxels, std::vector<float> &, unsigned long,
                      std::vector<float> &, unsigned long) {
    if (!values.empty()) { values.clear(); }
    compute(allVoxels);
}




// ========================
// ---- PCA Differences -----
// ========================


// NumberOfVoxels Feature: Return the number of voxels in a edge/node
//PCARatioDiffs::PCARatioDiffs() {
//    this->filterName = "PCARatioDiffs";
//    this->signalName = "Morphology";
//    this->featureNames = {"d_l1/l2", "d_l1/l3", "d_l2/l3", "n(v1a,v1b)", "n(v2a,v2b)", "n(v3a,v3b)"};
//
//    bool NodePCACalculated=false;
//    for(auto& feature: FeatureList::nodeFeaturesList){
//        if(feature->filterName == "PCARatios"){
//            NodePCACalculated = true;
//        }
//    }
//    if(!NodePCACalculated){
//        throw std::logic_error("PCAVectors has to be added to node features that differences can be calculated!");
//    }
//}
//
//std::unique_ptr<Feature> PCARatioDiffs::createNew() {
//    return std::unique_ptr<Feature>(new PCARatioDiffs());
//}
//
//void PCARatioDiffs::compute(std::vector<Voxel> &, unsigned int labelA, unsigned int labelB) {
////  std::cout << "A: " << labelA << " B: " << labelB << std::endl;
//    std::vector<float> ratioNodeA = GraphBase::pGraph->nodes[labelA]->getNodeFeatureByName("PCARatios","Morphology");
//    std::vector<float> ratioNodeB = GraphBase::pGraph->nodes[labelB]->getNodeFeatureByName("PCARatios","Morphology");
//    std::vector<float> valuesNodeA = GraphBase::pGraph->nodes[labelA]->getNodeFeatureByName("PCAValues","Morphology");
//    std::vector<float> valuesNodeB = GraphBase::pGraph->nodes[labelA]->getNodeFeatureByName("PCAValues","Morphology");
//
//    double A_eigenvectors[3][3];
//  A_eigenvectors[0][0] = valuesNodeA[3];
//  A_eigenvectors[1][0] = valuesNodeA[4];
//  A_eigenvectors[2][0] = valuesNodeA[5];
//  A_eigenvectors[0][1] = valuesNodeA[6];
//  A_eigenvectors[1][1] = valuesNodeA[7];
//  A_eigenvectors[2][1] = valuesNodeA[8];
//  A_eigenvectors[0][2] = valuesNodeA[9];
//  A_eigenvectors[1][2] = valuesNodeA[10];
//  A_eigenvectors[2][2] = valuesNodeA[11];
//
//  double B_eigenvectors[3][3];
//  B_eigenvectors[0][0] = valuesNodeB[3];
//  B_eigenvectors[1][0] = valuesNodeB[4];
//  B_eigenvectors[2][0] = valuesNodeB[5];
//  B_eigenvectors[0][1] = valuesNodeB[6];
//  B_eigenvectors[1][1] = valuesNodeB[7];
//  B_eigenvectors[2][1] = valuesNodeB[8];
//  B_eigenvectors[0][2] = valuesNodeB[9];
//  B_eigenvectors[1][2] = valuesNodeB[10];
//  B_eigenvectors[2][2] = valuesNodeB[11];
//
//
////
//    values.push_back(std::fabs(ratioNodeA[0] - ratioNodeB[0]));
//    values.push_back(std::fabs(ratioNodeA[1] - ratioNodeB[1]));
//    values.push_back(std::fabs(ratioNodeA[2] - ratioNodeB[2]));
//    values.push_back(computeNorm(A_eigenvectors, B_eigenvectors, 0, 0));
//    values.push_back(computeNorm(A_eigenvectors, B_eigenvectors, 1, 1));
//    values.push_back(computeNorm(A_eigenvectors, B_eigenvectors, 2, 2));
//}
//
//
//void PCARatioDiffs::compute(std::vector<std::vector<Voxel>*>, unsigned int labelA, unsigned labelB) {
////  std::cout << "A: " << labelA << " B: " << labelB << std::endl;
//    std::vector<float> ratioNodeA = GraphBase::pGraph->nodes[labelA]->getNodeFeatureByName("PCARatios","Morphology");
//    std::vector<float> ratioNodeB = GraphBase::pGraph->nodes[labelB]->getNodeFeatureByName("PCARatios","Morphology");
//    std::vector<float> valuesNodeA = GraphBase::pGraph->nodes[labelA]->getNodeFeatureByName("PCAValues","Morphology");
//    std::vector<float> valuesNodeB = GraphBase::pGraph->nodes[labelA]->getNodeFeatureByName("PCAValues","Morphology");
//
//    double A_eigenvectors[3][3];
//    A_eigenvectors[0][0] = valuesNodeA[3];
//    A_eigenvectors[1][0] = valuesNodeA[4];
//    A_eigenvectors[2][0] = valuesNodeA[5];
//    A_eigenvectors[0][1] = valuesNodeA[6];
//    A_eigenvectors[1][1] = valuesNodeA[7];
//    A_eigenvectors[2][1] = valuesNodeA[8];
//    A_eigenvectors[0][2] = valuesNodeA[9];
//    A_eigenvectors[1][2] = valuesNodeA[10];
//    A_eigenvectors[2][2] = valuesNodeA[11];
//
//    double B_eigenvectors[3][3];
//    B_eigenvectors[0][0] = valuesNodeB[3];
//    B_eigenvectors[1][0] = valuesNodeB[4];
//    B_eigenvectors[2][0] = valuesNodeB[5];
//    B_eigenvectors[0][1] = valuesNodeB[6];
//    B_eigenvectors[1][1] = valuesNodeB[7];
//    B_eigenvectors[2][1] = valuesNodeB[8];
//    B_eigenvectors[0][2] = valuesNodeB[9];
//    B_eigenvectors[1][2] = valuesNodeB[10];
//    B_eigenvectors[2][2] = valuesNodeB[11];
//
//
////
//    values.push_back(std::fabs(ratioNodeA[0] - ratioNodeB[0]));
//    values.push_back(std::fabs(ratioNodeA[1] - ratioNodeB[1]));
//    values.push_back(std::fabs(ratioNodeA[2] - ratioNodeB[2]));
//    values.push_back(computeNorm(A_eigenvectors, B_eigenvectors, 0, 0));
//    values.push_back(computeNorm(A_eigenvectors, B_eigenvectors, 1, 1));
//    values.push_back(computeNorm(A_eigenvectors, B_eigenvectors, 2, 2));
//}
//
//
//
//void PCARatioDiffs::merge(std::vector<Voxel> &, std::vector<float> &, unsigned long,
//                      std::vector<float> &, unsigned long) {
//    throw  std::logic_error("PCADiff is only implemented for edges, were merge should never be called!");
//
//}
//
//float PCARatioDiffs::computeNorm(double vectorsA[3][3], double vectorsB[3][3], int i, int j){
//  // computes norm with eigenvector index i vs eigenvector index j
//  float norm=0;
//  for(int row=0; row<3; row++){
//    norm += (vectorsA[row][i] - vectorsB[row][j])   *  (vectorsA[row][i] - vectorsB[row][j]);
//  }
//  norm = std::sqrt(norm);
//  if(std::isnan(norm)){
//    std::cout << ("NaN error in PCA!");
//    std::cout << "Vector1: " << vectorsA[0][i] << " " << vectorsA[1][i] << " " << vectorsA[2][i] <<  std::endl;
//    std::cout << "Vector2: " << vectorsB[0][j] << " " << vectorsB[1][j] << " " << vectorsB[2][j] <<  std::endl;
//    throw std::logic_error("NaN error!");
//  }
//
//  return norm;
//}
//