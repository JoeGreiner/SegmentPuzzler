#ifndef GRAPHBASEDSEGMENTATION_PCA_H
#define GRAPHBASEDSEGMENTATION_PCA_H

#include <iostream>
#include <cmath>
#include <vector>
#include "voxel.h"

struct PCA {
    double eigenValues[3][3];
    double eigenVectors[3][3];

    PCA() {};

    PCA(double A[3][3], double B[3][3]) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                eigenValues[i][j] = A[i][j];
                eigenVectors[i][j] = B[i][j];
            }
        }
    }
};

struct pcaMatrix {
    double smat[3][3];

    pcaMatrix(double A[3][3]);

    pcaMatrix operator*(pcaMatrix B);

    pcaMatrix() {}
};


PCA calcEigenJacobi(double cov[3][3]);

PCA calcPCA(std::vector<Voxel> &voxels);

PCA calcPCA(const std::vector<std::vector<Voxel> *> voxelList);


#endif //GRAPHBASEDSEGMENTATION_PCA_H
