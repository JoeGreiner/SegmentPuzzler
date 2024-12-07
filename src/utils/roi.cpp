#include <itkIndex.h>
#include "roi.h"
#include <vector>

void Roi::mergeRoiWith(Roi B) {
    if (B.minX < minX) {
        minX = B.minX;
        minXVoxel = B.minXVoxel;
    }
    if (B.minY < minY) {
        minY = B.minY;
        minYVoxel = B.minYVoxel;
    }
    if (B.minZ < minZ) {
        minZ = B.minZ;
        minZVoxel = B.minZVoxel;
    }
    if (B.maxX > maxX) {
        maxX = B.maxX;
        maxXVoxel = B.maxXVoxel;
    }
    if (B.maxY > maxY) {
        maxY = B.maxY;
        maxYVoxel = B.maxYVoxel;
    }
    if (B.maxZ > maxZ) {
        maxZ = B.maxZ;
        maxZVoxel = B.maxZVoxel;
    }
}

void Roi::updateBoundingRoi(std::vector<Voxel> &voxels) {
    //    reset roi
    reset();
    //    update from voxel list itself
    for (auto &voxel : voxels) {
        if (voxel.x < minX) {
            minX = voxel.x;
            minXVoxel = voxel;
        }
        if (voxel.x > maxX) {
            maxX = voxel.x;
            maxXVoxel = voxel;
        }
        if (voxel.y < minY) {
            minY = voxel.y;
            minYVoxel = voxel;
        }
        if (voxel.y > maxY) {
            maxY = voxel.y;
            maxYVoxel = voxel;
        }
        if (voxel.z < minZ) {
            minZ = voxel.z;
            minZVoxel = voxel;
        }
        if (voxel.z > maxZ) {
            maxZ = voxel.z;
            maxZVoxel = voxel;
        }
    }
}


void Roi::updateBoundingRoi(itk::Index<3> const &index) {
    // attention: if roi is not initialized, a voxel can be min and max voxel at the same time
    // therefore no if/else optimization
    if (index[0] < minX) {
        minX = index[0];
        minXVoxel = Voxel(index[0], index[1], index[2]);
    }
    if (index[0] > maxX) {
        maxX = index[0];
        maxXVoxel = Voxel(index[0], index[1], index[2]);
    }
    if (index[1] < minY) {
        minY = index[1];
        minYVoxel = Voxel(index[0], index[1], index[2]);
    }
    if (index[1] > maxY) {
        maxY = index[1];
        maxYVoxel = Voxel(index[0], index[1], index[2]);
    }
    if (index[2] < minZ) {
        minZ = index[2];
        minZVoxel = Voxel(index[0], index[1], index[2]);
    }
    if (index[2] > maxZ) {
        maxZ = index[2];
        maxZVoxel = Voxel(index[0], index[1], index[2]);
    }
}

void Roi::updateBoundingRoi(Voxel const &voxel) {
    if (voxel.x < minX) {
        minX = voxel.x;
        minXVoxel = voxel;
    }
    if (voxel.x > maxX) {
        maxX = voxel.x;
        maxXVoxel = voxel;
    }
    if (voxel.y < minY) {
        minY = voxel.y;
        minYVoxel = voxel;
    }
    if (voxel.y > maxY) {
        maxY = voxel.y;
        maxYVoxel = voxel;
    }
    if (voxel.z < minZ) {
        minZ = voxel.z;
        minZVoxel = voxel;
    }
    if (voxel.z > maxZ) {
        maxZ = voxel.z;
        maxZVoxel = voxel;
    }
}

Roi::Roi(Voxel voxel) :
        minXVoxel(voxel),
        minYVoxel(voxel),
        minZVoxel(voxel),
        maxXVoxel(voxel),
        maxYVoxel(voxel),
        maxZVoxel(voxel),
        minX(voxel.x),
        minY(voxel.y),
        minZ(voxel.z),
        maxX(voxel.x),
        maxY(voxel.y),
        maxZ(voxel.z) {
}


Roi::Roi() : minX{std::numeric_limits<int>::max()},
             minY{std::numeric_limits<int>::max()},
             minZ{std::numeric_limits<int>::max()},
             maxX{std::numeric_limits<int>::min()},
             maxY{std::numeric_limits<int>::min()},
             maxZ{std::numeric_limits<int>::min()} {
}

void Roi::print() {
    printf("fx: %d tx: %d fy: %d ty: %d fz: %d tz: %d\n", minX, maxX, minY, maxY, minZ, maxZ);
};

void Roi::reset() {
    minX = std::numeric_limits<int>::max();
    minY = std::numeric_limits<int>::max();
    minZ = std::numeric_limits<int>::max();
    maxX = std::numeric_limits<int>::min();
    maxY = std::numeric_limits<int>::min();
    maxZ = std::numeric_limits<int>::min();
}

double Roi::calculateMeanDistanceOfExtremeVoxelToPoint(Voxel otherVoxel) {
    double meanDistance = 0.;
    meanDistance += euclidianDistance(minXVoxel, otherVoxel);
    meanDistance += euclidianDistance(minYVoxel, otherVoxel);
    meanDistance += euclidianDistance(minZVoxel, otherVoxel);
    meanDistance += euclidianDistance(maxXVoxel, otherVoxel);
    meanDistance += euclidianDistance(maxYVoxel, otherVoxel);
    meanDistance += euclidianDistance(maxZVoxel, otherVoxel);
    return meanDistance / 6.;
}

double Roi::euclidianDistance(Voxel A, Voxel B) {
    double tmpX = A.x - B.x;
    double tmpY = A.y - B.y;
    double tmpZ = A.z - B.z;
    return (std::sqrt((tmpX * tmpX) + (tmpY * tmpY) + (tmpZ * tmpZ)));
}
