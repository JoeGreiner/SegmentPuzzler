#ifndef SEGMENTCOUPLER_UTILS_H
#define SEGMENTCOUPLER_UTILS_H

#include <iostream>
#include <src/file_definitions/dataTypes.h>
#include <unordered_map>
#include <QString>

class InitialNode;

namespace utils {
    using EdgePairIdType = dataType::EdgePairIdType;
    using SegmentIdType = dataType::SegmentIdType;

    bool check_if_file_exists(QString path_to_file);

    double tic(std::string description = "");

    void toc(double tic, std::string description);

    EdgePairIdType switchOrderOfPair(EdgePairIdType pairIn);

    std::tuple<long, long, long, long, long, long> calculateBoundingBoxForLabel(
            typename dataType::SegmentsImageType::Pointer segmentationImage,
            dataType::SegmentIdType labelValue);


    template<typename keyType, typename targetType>
    std::vector<keyType> getKeyVecOfSharedPtrMap(
            std::unordered_map<keyType, std::shared_ptr<targetType>> &map) {
        std::vector<keyType> vec;
        vec.reserve(map.size());
        for (auto element : map) {
            vec.push_back(element.first);
        }
        return vec;
    }


    template<typename keyType, typename targetType>
    std::vector<targetType *> getTargetPointersVecOfSharedPtrMap(
            std::unordered_map<keyType, std::shared_ptr<targetType>> &map) {

        std::vector<targetType *> vec;
        vec.reserve(map.size());

        for (auto element : map) {
            vec.push_back(element.second.get());
        }

        return vec;
    }

    SegmentIdType getOtherLabelOfPair(EdgePairIdType pair, SegmentIdType ownLabel);
}


#endif //SEGMENTCOUPLER_UTILS_H
