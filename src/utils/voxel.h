#ifndef voxel_h
#define voxel_h

#include <iostream>

struct Voxel {
    int x, y, z;

    // constructor for coordinates + label
    Voxel(int xIn, int yIn, int zIn) :
            x(xIn),
            y(yIn),
            z(zIn) {};

    // print voxel data
    void print();

    Voxel() : x(0), y(0), z(0) {};

    // relational comparison with another voxel
    bool operator<(const Voxel &b) const;

    // is equal comparison with another voxel
    bool operator==(const Voxel &b) const;
};

#endif /* voxel_h */
