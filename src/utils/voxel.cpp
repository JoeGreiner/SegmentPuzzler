#include "voxel.h"

void Voxel::print() {
    std::cout << "x: " << x << std::endl;
    std::cout << "y: " << y << std::endl;
    std::cout << "z: " << z << std::endl;
}

bool Voxel::operator<(const Voxel &b) const {
    if (x < b.x) {
        return true;
    } else if ((x == b.x) && (y < b.y)) {
        return true;
    } else return (x == b.x) && (y == b.y) && (z < b.z);
}

bool Voxel::operator==(const Voxel &b) const {
    return ((x == b.x) && (y == b.y) && (z == b.z));
}


