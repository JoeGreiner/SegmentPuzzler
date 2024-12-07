#include "PCA.h"


pcaMatrix::pcaMatrix(double A[3][3]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            smat[i][j] = A[i][j];
        }
    }
}

pcaMatrix pcaMatrix::operator*(pcaMatrix B) {
// only quadratic
    pcaMatrix resultMat;
    resultMat.smat[0][0] = smat[0][0] * B.smat[0][0] + smat[0][1] * B.smat[1][0] + smat[0][2] * B.smat[2][0];
    resultMat.smat[0][1] = smat[0][0] * B.smat[0][1] + smat[0][1] * B.smat[1][1] + smat[0][2] * B.smat[2][1];
    resultMat.smat[0][2] = smat[0][0] * B.smat[0][2] + smat[0][1] * B.smat[1][2] + smat[0][2] * B.smat[2][2];
    resultMat.smat[1][0] = smat[1][0] * B.smat[0][0] + smat[1][1] * B.smat[1][0] + smat[1][2] * B.smat[2][0];
    resultMat.smat[1][1] = smat[1][0] * B.smat[0][1] + smat[1][1] * B.smat[1][1] + smat[1][2] * B.smat[2][1];
    resultMat.smat[1][2] = smat[1][0] * B.smat[0][2] + smat[1][1] * B.smat[1][2] + smat[1][2] * B.smat[2][2];
    resultMat.smat[2][0] = smat[2][0] * B.smat[0][0] + smat[2][1] * B.smat[1][0] + smat[2][2] * B.smat[2][0];
    resultMat.smat[2][1] = smat[2][0] * B.smat[0][1] + smat[2][1] * B.smat[1][1] + smat[2][2] * B.smat[2][1];
    resultMat.smat[2][2] = smat[2][0] * B.smat[0][2] + smat[2][1] * B.smat[1][2] + smat[2][2] * B.smat[2][2];
    return resultMat;
}


PCA calcEigenJacobi(double cov[3][3]) {
    // works for symmetric and real 3x3 matrices
    double eigenVectors[3][3];

    eigenVectors[0][0] = 1;
    eigenVectors[0][1] = 0;
    eigenVectors[0][2] = 0;
    eigenVectors[1][0] = 0;
    eigenVectors[1][1] = 1;
    eigenVectors[1][2] = 0;
    eigenVectors[2][0] = 0;
    eigenVectors[2][1] = 0;
    eigenVectors[2][2] = 1;
    pcaMatrix eigenVectorsMat(eigenVectors);
    pcaMatrix covmat(cov);

//  printf("Covariance matrix: \n");
//  for(int i=0; i<3; i++){        printf("%f %f %f\n",covmat.smat[i][0],covmat.smat[i][1],covmat.smat[i][2]);    }

    int type, ixCol, ixRow;
    double residual = 50;
    int maxIterations = 2000;
    int iteration = 0;
    double atol = 1e-8;
    while (residual > atol && iteration < maxIterations) {

        // find largest off diagonal value in the lower half
        residual = std::fabs(covmat.smat[1][0]) + std::fabs(covmat.smat[2][0]) + std::fabs(covmat.smat[2][1]);
        //printf("Jacobi-Residual: %f \n", residual);

        if ((std::fabs(covmat.smat[1][0]) > std::fabs(covmat.smat[2][0])) &&
            (std::fabs(covmat.smat[1][0]) > std::fabs(covmat.smat[2][1]))) {
            type = 1;
            ixCol = 1;
            ixRow = 0;
        } else if ((std::fabs(covmat.smat[2][0]) > std::fabs(covmat.smat[1][0])) &&
                   (std::fabs(covmat.smat[2][0]) > std::fabs(covmat.smat[2][1]))) {
            type = 2;
            ixCol = 2;
            ixRow = 0;
        } else {
            type = 3;
            ixCol = 2;
            ixRow = 1;
        }

        //printf("Targeting index type %i \n", type);
        // classical definitions for jacobi iteration
        double t, s, c;
        t = covmat.smat[ixRow][ixCol] / (covmat.smat[ixCol][ixCol] - covmat.smat[ixRow][ixRow]);
        c = 1 / sqrt(t * t + 1);
        s = t * c;
        //printf("t: %f c: %f s:%f \n", t,c,s);

        // create rotation matrix
        double R[3][3];
        if (type == 1) {
            R[0][0] = c;
            R[0][1] = s;
            R[0][2] = 0;
            R[1][0] = -s;
            R[1][1] = c;
            R[1][2] = 0;
            R[2][0] = 0;
            R[2][1] = 0;
            R[2][2] = 1;
        } else if (type == 2) {
            R[0][0] = c;
            R[0][1] = 0;
            R[0][2] = s;
            R[1][0] = 0;
            R[1][1] = 1;
            R[1][2] = 0;
            R[2][0] = -s;
            R[2][1] = 0;
            R[2][2] = c;
        } else {
            R[0][0] = 1;
            R[0][1] = 0;
            R[0][2] = 0;
            R[1][0] = 0;
            R[1][1] = c;
            R[1][2] = s;
            R[2][0] = 0;
            R[2][1] = -s;
            R[2][2] = c;
        }

        double RT[3][3]; // transpose
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                RT[i][j] = R[j][i];
            }
        }

        pcaMatrix RTmat(RT);
        pcaMatrix Rmat(R);
        covmat = (RTmat * covmat) * R;
        eigenVectorsMat = eigenVectorsMat * Rmat; // Eigenvectors = JacobiRot_1 * JacobiRot_2 * JacobiRot_3 (...)
        iteration++;
    }
//
//  printf("Eigenvectors:\n");
//  for(int i=0; i<3; i++){ printf("%f %f %f\n",eigenVectorsMat.smat[i][0],eigenVectorsMat.smat[i][1],eigenVectorsMat.smat[i][2]);}
//
//  printf("Eigenvalues:\n");
//  printf("l1: %f l2: %f l3: %f\n",covmat.smat[0][0],covmat.smat[1][1],covmat.smat[2][2]);


    double sortedEigenVectors[3][3];
    double sortedEigenValues[3][3];

    for (int i = 0; i < 3; i++) {
        double maxEValue = 0;
        unsigned int index = 0;
        for (unsigned int j = 0; j < 3; j++) {
            if (covmat.smat[j][j] > maxEValue) {
                maxEValue = covmat.smat[j][j];
                index = j;
            }
        }
        sortedEigenValues[i][i] = maxEValue;
        covmat.smat[index][index] = 0;
        sortedEigenVectors[0][i] = eigenVectorsMat.smat[0][index];
        sortedEigenVectors[1][i] = eigenVectorsMat.smat[1][index];
        sortedEigenVectors[2][i] = eigenVectorsMat.smat[2][index];
    }





//  printf("Jacobi-Residual: %f \n", residual);
//
//  printf("Sorted Eigenvectors:\n");
//  for(int i=0; i<3; i++){ printf("%f %f %f\n",sortedEigenVectors[i][0],sortedEigenVectors[i][1],sortedEigenVectors[i][2]);}
//
//  printf("Sorted Eigenvalues:\n");
//  printf("l1: %f l2: %f l3: %f\n", sortedEigenValues[0][0], sortedEigenValues[1][1], sortedEigenValues[2][2]);


    //PCA((eigenvalues,eigenvectors))
    return PCA(sortedEigenValues, sortedEigenVectors);
}


PCA calcPCA(std::vector<Voxel> &voxels) {

    double cov[3][3];

    // normalization
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            cov[i][j] = 0;
        }
    }

    unsigned long long numberVoxels = voxels.size();

    double xMean = 0, yMean = 0, zMean = 0;
    for (auto &voxel : voxels) {
        xMean += voxel.x;
        yMean += voxel.y;
        zMean += voxel.z;
    }
    xMean = xMean / numberVoxels;
    yMean = yMean / numberVoxels;
    zMean = zMean / numberVoxels;

    for (unsigned long i = 0; i < numberVoxels; i++) {
        cov[0][0] += (voxels.at(i).x - xMean) * (voxels.at(i).x - xMean);
        cov[0][1] += (voxels.at(i).x - xMean) * (voxels.at(i).y - yMean);
        cov[0][2] += (voxels.at(i).x - xMean) * (voxels.at(i).z - zMean);
        cov[1][1] += (voxels.at(i).y - yMean) * (voxels.at(i).y - yMean);
        cov[1][2] += (voxels.at(i).y - yMean) * (voxels.at(i).z - zMean);
        cov[2][2] += (voxels.at(i).z - zMean) * (voxels.at(i).z - zMean);
    }

    // symmetry
    cov[1][0] = cov[0][1];
    cov[2][0] = cov[0][2];
    cov[2][1] = cov[1][2];

    // normalization
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            cov[i][j] = cov[i][j] / numberVoxels;
        }
    }

    PCA myPCA = calcEigenJacobi(cov);
    return myPCA;
}

PCA calcPCA(const std::vector<std::vector<Voxel> *> voxelList) {

    double cov[3][3];

    // normalization
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            cov[i][j] = 0;
        }
    }


    unsigned long long numberVoxels = 0;
    for (auto &listEntry : voxelList) {
        numberVoxels += listEntry->size();
    }

    double xMean = 0, yMean = 0, zMean = 0;
    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            xMean += voxel.x;
            yMean += voxel.y;
            zMean += voxel.z;
        }
    }
    xMean = xMean / numberVoxels;
    yMean = yMean / numberVoxels;
    zMean = zMean / numberVoxels;

    for (auto &listEntry : voxelList) {
        for (auto &voxel : *listEntry) {
            cov[0][0] += (voxel.x - xMean) * (voxel.x - xMean);
            cov[0][1] += (voxel.x - xMean) * (voxel.y - yMean);
            cov[0][2] += (voxel.x - xMean) * (voxel.z - zMean);
            cov[1][1] += (voxel.y - yMean) * (voxel.y - yMean);
            cov[1][2] += (voxel.y - yMean) * (voxel.z - zMean);
            cov[2][2] += (voxel.z - zMean) * (voxel.z - zMean);
        }
    }

    // symmetry
    cov[1][0] = cov[0][1];
    cov[2][0] = cov[0][2];
    cov[2][1] = cov[1][2];

    // normalization
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            cov[i][j] = cov[i][j] / numberVoxels;
        }
    }

    PCA myPCA = calcEigenJacobi(cov);
    return myPCA;
}

