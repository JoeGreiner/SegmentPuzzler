#ifndef GRAPHBASEDSEGMENTATION_FILEOUTPUT_H
#define GRAPHBASEDSEGMENTATION_FILEOUTPUT_H

#include <string>
#include <vector>
#include <iostream>
#include <qstring.h>
#include <itkImage.h>
#include <itkImageFileReader.h>
#include <itkImageFileWriter.h>

//#include "H5Cpp.h"
//#include <highfive/H5File.hpp>
//#include <highfive/H5DataSet.hpp>
//#include <highfive/H5DataSpace.hpp>
//#include "xtensor/xarray.hpp"
#include <fstream>



void getDimensionAndDataTypeOfFile(QString &fileName, unsigned int &dimensionOut,
                                   itk::ImageIOBase::IOComponentType &dataTypeOut);

template<typename dType>
typename itk::Image<dType, 3>::Pointer ITKImageLoader(QString &fileName) {
    using ImageType = itk::Image<dType, 3>;
    using ReaderType = itk::ImageFileReader<ImageType>;
    std::cout << "ITKImageLoader: Reading " << fileName.toStdString() << "\n";

    typename ReaderType::Pointer reader = ReaderType::New();
    reader->SetFileName(fileName.toStdString());

    typename ImageType::Pointer pImage = reader->GetOutput();
    reader->Update();

    auto spacing = pImage->GetSpacing();
    auto origin = pImage->GetOrigin();
    auto direction = pImage->GetDirection();
    std::cout << "ITKImageLoader: Done!\n";
    std::cout << "ITKImageLoader: Spacing: [" << spacing[0] << ", " << spacing[1] << ", " << spacing[2] << "]\n";
    std::cout << "ITKImageLoader: Origin:  [" << origin[0] << ", " << origin[1] << ", " << origin[2] << "]\n";
    std::cout << "ITKImageLoader: Direction: [["
              << direction[0][0] << ", " << direction[0][1] << ", " << direction[0][2] << "], ["
              << direction[1][0] << ", " << direction[1][1] << ", " << direction[1][2] << "], ["
              << direction[2][0] << ", " << direction[2][1] << ", " << direction[2][2] << "]]\n";

    return pImage;
}

template<typename imageType>
void ITKImageWriter(typename imageType::Pointer pImage, std::string filePathWriter) {
    auto spacing = pImage->GetSpacing();
    auto origin = pImage->GetOrigin();
    auto direction = pImage->GetDirection();
    std::cout << "ITKImageWriter (fileIO): Writing to: " << filePathWriter << "\n";
    std::cout << "ITKImageWriter (fileIO): Spacing: [" << spacing[0] << ", " << spacing[1] << ", " << spacing[2] << "]\n";
    std::cout << "ITKImageWriter (fileIO): Origin:  [" << origin[0] << ", " << origin[1] << ", " << origin[2] << "]\n";
    std::cout << "ITKImageWriter (fileIO): Direction: [["
              << direction[0][0] << ", " << direction[0][1] << ", " << direction[0][2] << "], ["
              << direction[1][0] << ", " << direction[1][1] << ", " << direction[1][2] << "], ["
              << direction[2][0] << ", " << direction[2][1] << ", " << direction[2][2] << "]]\n";
    using WriterType = itk::ImageFileWriter<imageType>;
    typename WriterType::Pointer writer = WriterType::New();
    writer->SetInput(pImage);
    writer->SetFileName(filePathWriter);
    writer->Update();
    std::cout << "ITKImageWriter (fileIO): Done!\n";
}



void writeDataSetToFile(std::string outputPath,
                        std::vector<std::vector<float>> features,
                        std::vector<int> shouldMerge,
                        std::vector<std::string> featureNames,
                        std::vector<std::string> identifier = {});  // if set, indication if edge should be merged or not


//void writeHDF(xt::xarray<unsigned int>& segments);
//template <class dataType> void writeHDF(xt::xarray<dataType>& segments, std::string outputPath);


std::vector<std::vector<float>> readFeaturesFromFile(const std::string &fileName);

std::vector<int> readLabelsFromFile(const std::string &fileName);

void createEmptyDataSet(std::string outputPath, std::vector<std::string> featureNames, bool setMergeLabel);

void appendDataSetToFile(std::vector<std::vector<float>> features,
                         std::string outputPath,
                         std::vector<int> shouldMerge = {},
                         std::vector<std::string> identifier = {},
                         std::vector<std::string> featureNames = {}); // feature names, if not provided, old ones will be used




//template <class dataTypeOut, class dataTypeIn> void writeHDF(xt::xarray<dataTypeIn>& segments, std::string outputPath){
//    auto dim = segments.shape();
//    unsigned short dimX = dim[0];
//    unsigned short dimY = dim[1];
//    unsigned short dimZ = dim[2];
//
//    std::cout << "Writing: " << outputPath << std::endl;
//    if(typeid(dataTypeOut) != typeid(dataTypeIn)){
//        std::cout << "Warning: Converting datatypes while exporting file." <<  std::endl;
//    }
//
////  unsigned short data[dimX][dimY][dimZ];
//    std::vector<std::vector<std::vector<dataTypeOut>>> data(dimX,
//                                                            std::vector<std::vector<dataTypeOut>>
//                                                                    (dimY, std::vector<dataTypeOut>(dimZ,0)));
//
//
////  std::cout << segments(0,0,0) << std::endl;
//    for(unsigned int x=0; x<dimX; x++){
//        for(unsigned int y=0; y<dimY; y++){
//            for(unsigned int z=0; z<dimZ; z++){
//                data[x][y][z] = (dataTypeOut) segments(x,y,z);
//            }
//        }
//    }
//
//    std::remove(outputPath.c_str());
//    HighFive::File file(outputPath, HighFive::File::ReadWrite | HighFive::File::Create | HighFive::File::Truncate);
//
//    // create a dataset of native integer with the size of the vector
//    // 'data'
//    HighFive::DataSet dataset =
//            file.createDataSet<dataTypeOut>("imageData", HighFive::DataSpace::From(data));
//
//    // write our vector of int to the HDF5 dataset
//    dataset.write(data);
//}

#endif //GRAPHBASEDSEGMENTATION_FILEOUTPUT_H
