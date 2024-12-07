#ifndef feature_h
#define feature_h


#include <map>
#include "src/utils/voxel.h"
#include "graphBase.h"
#include "src/utils/voxel.h"
#include "src/utils/PCA.h"

//TODO: Write tests for all features + mergers

// NOTE: Only Node features get merged, edge features get recomputed!

struct Feature {
    std::string filterName;
    std::string signalName;
    std::vector<std::string> featureNames;
    std::vector<float> values;

    virtual void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) = 0;

    virtual void compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned labelB = 0) = 0;

    virtual void merge(std::vector<Voxel> &allVoxels,
                       std::vector<float> &valuesA, unsigned long numberElementsA,
                       std::vector<float> &valuesB, unsigned long numberElementsB) = 0;

    virtual Feature *createNew() = 0;

    virtual ~Feature() = default;


};


class FeatureList {
public:
    static bool GroundTruthLabelComputed;
    static std::vector<std::unique_ptr<Feature>> edgeFeaturesList;
    static std::vector<std::unique_ptr<Feature>> nodeFeaturesList;
    static std::vector<std::unique_ptr<Feature>> unionFeaturesList;
};


// mean of a signal
struct MeanSignal : public Feature, GraphBase {
    //TODO implement featurenames
    // constructor with signalName and filterName
    explicit MeanSignal(std::string signalName);

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void
    compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned int labelB = 0) override;


    void merge(std::vector<Voxel> &,
               std::vector<float> &valuesA, unsigned long numberElementsA,
               std::vector<float> &valuesB, unsigned long numberElementsB) override;

};

// number of voxels of a node/edge
struct NumberOfVoxels : public Feature, GraphBase {
    explicit NumberOfVoxels();

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void
    compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned int labelB = 0) override;


    void merge(std::vector<Voxel> &,
               std::vector<float> &valuesA, unsigned long numberElementsA,
               std::vector<float> &valuesB, unsigned long numberElementsB) override;

};

// ratios eigenvectors
struct PCAValues : public Feature, GraphBase {
    explicit PCAValues();

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned labelB = 0) override;


    void merge(std::vector<Voxel> &,
               std::vector<float> &valuesA, unsigned long numberElementsA,
               std::vector<float> &valuesB, unsigned long numberElementsB) override;
};


// ratios eigenvectors
struct PCARatios : public Feature, GraphBase {
    explicit PCARatios();

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned labelB = 0) override;


    void merge(std::vector<Voxel> &,
               std::vector<float> &valuesA, unsigned long numberElementsA,
               std::vector<float> &valuesB, unsigned long numberElementsB) override;
};

// difference pcaRatios
//struct PCARatioDiffs : public Feature, GraphBase {
//    explicit PCARatioDiffs();
//
//    // create a new instance of the same faeture
//    std::unique_ptr<Feature> createNew() override;
//
//    // compute the feature over a given voxel list and save it into value
//    void compute(std::vector<Voxel> & voxels, unsigned int labelA=0, unsigned labelB=0) override;
//    void compute(std::vector<std::vector<Voxel>*> voxelList, unsigned int labelA=0, unsigned labelB=0) override;
//
//
//    void merge(std::vector<Voxel> &,
//               std::vector<float> &valuesA, unsigned long numberElementsA,
//               std::vector<float> &valuesB, unsigned long numberElementsB) override;
//
//    // norm between two eigenvectors
//    float computeNorm(double vectorsA[3][3],double vectorsB[3][3], int i, int j);
//
//
//};



// mean, moment, quantiles, hist, ...
// probabilities can be handled differently than microscopy data, e.g. distributions/range[0,1],...
struct GeneralStatisticOnProbabilities : public Feature, GraphBase {
    static unsigned long numberBinsHistogram;
    static unsigned long numberQuantiles;
    static unsigned long histogramIndexBegin, histogramIndexEnd;


    //TODO implement featurenames
    // constructor with signalName and filterName
    explicit GeneralStatisticOnProbabilities(std::string signalName);

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned labelB = 0) override;


    void merge(std::vector<Voxel> &voxels,
               std::vector<float> &valuesA, unsigned long numberElementsA,
               std::vector<float> &valuesB, unsigned long numberElementsB) override;

    // compute moments: variance, skewness and kurtosis. needs mean as input
    void computeMoments(const std::vector<float> &intensities, float mean, float &variance, float &skewness,
                        float &kurtosis) const;

    // take a vector, sort it, and compute predefined, hardcoded quantiles
    void sortAndComputeQuantiles(std::vector<float> &intensities, std::vector<float> &quantiles);

    std::vector<float> computeHistogram(const std::vector<float> &intensities, float binSize);
};


// mean GTLabel
struct GroundTruthLabel : public Feature, GraphBase {
    //TODO implement featurenames
    unsigned int mostProminentGroundTruthLabel;
    double mostProminentGroundTruthLabelPercentage;


    // constructor with signalName and filterName
    GroundTruthLabel();

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned labelB = 0) override;


    void merge(std::vector<Voxel> &allVoxels,
               std::vector<float> &, unsigned long,
               std::vector<float> &, unsigned long) override;

};


struct LabelOverlap : public Feature, GraphBase {
    //TODO implement featurenames
    long int corrospondingLabelWithMostOverlap;
    double overlapPercentage;
    dataType::SegmentsImageType::Pointer pToOtherLabelImagePointer;

    void setPToOtherLabelImagePointer(dataType::SegmentsImageType::Pointer pToOtherLabelImagePointer);

    // constructor with signalName and filterName
    LabelOverlap();

    // create a new instance of the same faeture
    Feature *createNew() override;

    // compute the feature over a given voxel list and save it into value
    void compute(std::vector<Voxel> &voxels, unsigned int labelA = 0, unsigned labelB = 0) override;

    void compute(std::vector<std::vector<Voxel> *> voxelList, unsigned int labelA = 0, unsigned labelB = 0) override;


    void merge(std::vector<Voxel> &allVoxels,
               std::vector<float> &, unsigned long,
               std::vector<float> &, unsigned long) override;

};

#endif /* feature_h */
