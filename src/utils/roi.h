#ifndef roi_hpp
#define roi_hpp

#include "voxel.h"
#include "itkIndex.h"
#include <vector>

struct Roi {

    // voxels (coordinate triple) that are responsible for given extreme values
    Voxel minXVoxel, minYVoxel, minZVoxel, maxXVoxel, maxYVoxel, maxZVoxel;
    int minX, minY, minZ, maxX, maxY, maxZ;

    // merge to rois and get a updated roi
    void mergeRoiWith(Roi B);

    // initialize ROI with a single voxel
    explicit Roi(Voxel voxel);

    Roi();

    // check if roi changed when adding a voxel
    void updateBoundingRoi(std::vector<Voxel> &voxels);

    void updateBoundingRoi(const Voxel &voxel);

    void updateBoundingRoi(itk::Index<3> const &index);


    double calculateMeanDistanceOfExtremeVoxelToPoint(Voxel pointToCalculateDistanceTo);

    // print roi
    void print();

    // reset roi
    void reset();

    double euclidianDistance(Voxel A, Voxel B);


};


#endif /* roi_hpp */
