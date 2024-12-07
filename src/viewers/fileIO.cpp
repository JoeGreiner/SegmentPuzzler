#include <src/viewers/fileIO.h>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QInputDialog>
#include <src/viewers/OrthoViewer.h>
#include <src/segment_handling/graph.h>
#include <QColorDialog>
#include <QFileDialog>
#include <src/viewers/itkSignal.h>
#include <itkImage.h>
#include "src/segment_handling/graphBase.h"
#include "fileIO.h"


void writeDataSetToFile(std::string outputPath,
                        std::vector<std::vector<float>> features,
                        std::vector<int> shouldMerge,
                        std::vector<std::string> featureNames,
                        std::vector<std::string> identifier) { // optional


    // if there is a targetLabel
    bool setMergeLabel = !(shouldMerge.empty());


    // output main: [identifier] [features] [shouldMerge, if set]
    // output header: [identifierName] [featureNames]
    std::cout << "Writing dataset to: " << outputPath << std::endl;


    // sanity check: are vector sizes equal?
    if (!identifier.empty()) {
        if (features.size() != identifier.size()) {
            throw (std::logic_error("Provided feature Size and identifier size differ!"));
        }
    }
    if (setMergeLabel) {
        if (features.size() != shouldMerge.size()) {
            std::cout << "Feature Size: " << features.size() << std::endl;
            std::cout << "Target Size: " << shouldMerge.size() << std::endl;
            throw (std::logic_error("Provided feature size and target size differ!"));
        }
    }


    std::string headerPath = outputPath + ".header";
    std::remove(headerPath.c_str());
    std::ofstream headerFile(headerPath);
    headerFile << "NumberEntries: " << features.size() << " NumberFeatures: " << featureNames.size()
               << " LabelsProvided: " << setMergeLabel << std::endl;
    headerFile << "Feature File: " << std::endl;

    // identifier featurename1, featurename2 ...
    headerFile << "Identifier ";
    for (auto &featureName : featureNames) {
        headerFile << featureName << " ";
    }
    if (setMergeLabel) {
        headerFile << std::endl << "Labels File: " << std::endl;
        headerFile << "Identifier ";
        headerFile << "ShouldMerge(1=yes)";
    }
    headerFile.close();

    // identifier feature1 feature2 ...
    // edge: nodeId1 nodeId2 feature1 ...
    std::string outputPathFeatures = outputPath + ".features";
    std::remove(outputPathFeatures.c_str());
    std::ofstream outFileFeatures(outputPathFeatures);
    unsigned long counter = 0;
    for (const auto &featureSet : features) {
        std::string indentifierString = identifier.empty() ? "-1 -1" : (identifier.at(counter));
        outFileFeatures << indentifierString << " ";
        for (const auto &feature : featureSet) {
            outFileFeatures << feature << " ";
        }
        outFileFeatures << "\n";
        counter++;
    }
    outFileFeatures.close();

    // identifier shouldMerge
    // edge: nodeidA nodeidB [+-1]
    if (setMergeLabel) {
        std::string outputPathLabels = outputPath + ".labels";
        std::remove(outputPathLabels.c_str());
        std::ofstream outFileLabels(outputPathLabels);
        for (size_t i = 0; i < features.size(); ++i) {
            std::string indentifierString = identifier.empty() ? "-1 -1" : (identifier.at(i));
            outFileLabels << indentifierString << " ";
            outFileLabels << shouldMerge[i];
            outFileLabels << "\n";
        }
        outFileLabels.close();
    }
};

void createEmptyDataSet(std::string outputPath, std::vector<std::string> featureNames, bool setMergeLabel) {
    // output main: [identifier] [features] [shouldMerge, if set]
    // output header: [identifierName] [featureNames]
    std::cout << "Create empty dataset at: " << outputPath << std::endl;

    std::string headerPath = outputPath + ".header";
    std::remove(headerPath.c_str());
    std::ofstream headerFile(headerPath);
    headerFile << "NumberEntries: " << "0" << " NumberFeatures: " << featureNames.size() << " LabelsProvided: "
               << setMergeLabel << std::endl;
    headerFile << "Feature File: " << std::endl;

    // identifier featurename1, featurename2 ...
    headerFile << "Identifier ";
    for (auto &featureName : featureNames) {
        headerFile << featureName << " ";
    }
    if (setMergeLabel) {
        headerFile << std::endl << "Labels File: " << std::endl;
        headerFile << "Identifier ";
        headerFile << "ShouldMerge(1=yes)";
    }
    headerFile.close();

    // empty feature file
    std::string outputPathFeatures = outputPath + ".features";
    std::remove(outputPathFeatures.c_str());
    std::ofstream outFileFeatures(outputPathFeatures);
    outFileFeatures.close();

    if (setMergeLabel) {
        std::string outputPathLabels = outputPath + ".labels";
        std::remove(outputPathLabels.c_str());
        std::ofstream outFileLabels(outputPathLabels);
        outFileLabels.close();
    }
}


void appendDataSetToFile(std::vector<std::vector<float>> features,
                         std::string outputPath,
                         std::vector<int> shouldMerge, // optional: ok
                         std::vector<std::string> identifier, // optional: ok
                         std::vector<std::string> featureNames) { // optional

    bool setMergeLabel = !(shouldMerge.empty());

    // output main: [identifier] [features] [shouldMerge, if set]
    // output header: [identifierName] [featureNames]
    std::cout << "Append dataset to: " << outputPath << std::endl;


    // check if header is the same
    std::string headerPath = outputPath + ".header";
    std::ifstream headerFile(headerPath);

    if (!headerFile) {
        std::cerr << "Cannot open the File : " << headerPath << std::endl;
        throw (std::logic_error("cant open file"));
    }

    std::string headerLineFile = "", headerLineDataSet = "", tmpLine;

    // read header file
    std::getline(headerFile, tmpLine); // NumberEntries: 7470 NumberFeatures: 39 LabelsProvided: 1
    unsigned long numberFeaturesFile, numberEntriesFile, labelProvidedFile;
    unsigned long numberFeatures = featureNames.size();
    sscanf(tmpLine.c_str(), "%*s %lu %*s %lu %*s %lu", &numberEntriesFile, &numberFeaturesFile, &labelProvidedFile);
    std::cout << "Entries: " << numberEntriesFile << " Features: " << numberFeaturesFile << " LabelsProvided: "
              << labelProvidedFile << std::endl;

    if (!featureNames.empty()) {
        if (numberFeatures != numberFeaturesFile) {
            std::cout << numberFeatures << " vs. " << numberFeaturesFile << std::endl;
            throw (std::logic_error("Trying to append dataset failed! - NumberFeatures differs"));
        }
    }

    if (setMergeLabel != bool(labelProvidedFile)) {
        throw (std::logic_error("Trying to append dataset failed! - labelProvided differs"));
    }

    std::getline(headerFile, tmpLine); // Feature File:
    headerLineFile += tmpLine;
    std::string featureNamesFile;
    std::getline(headerFile, featureNamesFile); // identifier feature1 feature2 ....
    headerLineFile += featureNamesFile;
    if (setMergeLabel) {
        std::getline(headerFile, tmpLine); // Labels File:
        headerLineFile += tmpLine;
        std::getline(headerFile, tmpLine); // Identifier ShouldMerge(1=yes)
        headerLineFile += tmpLine;
    }

    headerLineDataSet += "Feature File: ";

    if (!featureNames.empty()) {
        headerLineDataSet += "Identifier ";
        for (auto &featureName : featureNames) {
            headerLineDataSet += featureName + " ";
        }
    } else {
        headerLineDataSet += featureNamesFile;
    }

    if (setMergeLabel) {
        headerLineDataSet += "Labels File: ";
        headerLineDataSet += "Identifier ";
        headerLineDataSet += "ShouldMerge(1=yes)";
    }

    if (headerLineDataSet != headerLineFile) {
        std::cout << headerLineDataSet << std::endl;
        std::cout << headerLineFile << std::endl;
        throw (std::logic_error("Trying to append dataset failed! - FeatureNames are different!"));
    }
    headerFile.close();


    std::string outputPathFeatures = outputPath + ".features";
    std::ofstream outFile(outputPathFeatures, std::ofstream::out | std::ofstream::app);
    unsigned long counter = 0;
    for (const auto &featureSet : features) {
        std::string identifierString = identifier.empty() ? "-1 -1" : identifier.at(counter);
        outFile << identifierString << " ";
        for (const auto &feature : featureSet) {
            outFile << feature << " ";
        }
        outFile << "\n";
        counter++;
    }
    outFile.close();


    if (setMergeLabel) {
        std::string outputPathLabels = outputPath + ".labels";
        std::ofstream outFileLabels(outputPathLabels, std::ofstream::out | std::ofstream::app);
        for (size_t i = 0; i < features.size(); ++i) {
            std::string identifierString = identifier.empty() ? "-1 -1" : identifier.at(i);
            outFileLabels << identifierString << " ";
            outFileLabels << shouldMerge[i];
            outFileLabels << "\n";
        }
        outFileLabels.close();
    }

    // write header with merged dataset
    std::string outPathHeader = outputPath + ".header";
    std::remove(outPathHeader.c_str());
    std::ofstream headerOutFile(outPathHeader);
    headerOutFile << "NumberEntries: " << features.size() + numberEntriesFile << " NumberFeatures: "
                  << numberFeaturesFile << " LabelsProvided: " << setMergeLabel << std::endl;
    headerOutFile << "Feature File: " << std::endl;

    // identifier featurename1, featurename2 ...
    if (!featureNames.empty()) {
        for (auto &featureName : featureNames) {
            headerOutFile << featureName << " ";
        }
    } else {
        headerOutFile << featureNamesFile;
    }
    if (setMergeLabel) {
        headerOutFile << std::endl << "Labels File: " << std::endl;
        headerOutFile << "Identifier ";
        headerOutFile << "ShouldMerge(1=yes)";
    }
    headerOutFile.close();
};


std::vector<int> readLabelsFromFile(const std::string &fileName) {
    std::cout << std::endl << "-----------------------" << std::endl;
    std::cout << "Reading Labels from: " << fileName << std::endl;


    std::ifstream inFile(fileName.c_str());
    if (!inFile) {
        std::cerr << "Cannot open the File : " << fileName << std::endl;
        throw (std::logic_error("cant open file"));
    }

    std::string headerPath = fileName.substr(0, fileName.length() - 6) + "header";
    std::cout << "Reading header file: " << headerPath << std::endl;
    std::ifstream headerFile(headerPath);

    if (!headerFile) {
        std::cerr << "Cannot open the File : " << headerPath << std::endl;
        throw (std::logic_error("cant open file"));
    }

    std::string tmpLine;
    std::getline(headerFile, tmpLine);
    unsigned long numberFeaturesFile, numberEntriesFile, labelProvidedFile;
    sscanf(tmpLine.c_str(), "%*s %lu %*s %lu %*s %lu", &numberEntriesFile, &numberFeaturesFile, &labelProvidedFile);
    std::cout << "Entries: " << numberEntriesFile << " Features: " << numberFeaturesFile << " LabelsProvided: "
              << labelProvidedFile << std::endl;
    if (!bool(labelProvidedFile)) {
        throw (std::logic_error("labels were not provided with dataset!"));
    }

    std::vector<int> labels(numberEntriesFile, 0);
    for (unsigned int row = 0; row < numberEntriesFile; ++row) {
        std::getline(inFile, tmpLine);
        sscanf(tmpLine.c_str(), "%*d %*d %d", &labels[row]);
    }

    inFile.close();
    std::cout << "-----------------------" << std::endl << std::endl;
    return labels;
}

std::vector<std::vector<float>> readFeaturesFromFile(const std::string &fileName) {
    std::cout << std::endl << "-----------------------" << std::endl;
    std::cout << "Reading Features from: " << fileName << std::endl;

    std::ifstream inFile(fileName.c_str());
    if (!inFile) {
        std::cerr << "Cannot open the File : " << fileName << std::endl;
        throw (std::logic_error("cant open file"));
    }

    std::string headerPath = fileName.substr(0, fileName.length() - 8) + "header";;
    std::cout << "Reading header file: " << headerPath << std::endl;
    std::ifstream headerFile(headerPath);

    if (!headerFile) {
        std::cerr << "Cannot open the File : " << headerPath << std::endl;
        throw (std::logic_error("cant open file"));
    }

    std::string tmpLine;
    std::getline(headerFile, tmpLine);
    unsigned long numberFeaturesFile, numberEntriesFile, labelProvidedFile;
    sscanf(tmpLine.c_str(), "%*s %lu %*s %lu %*s %lu", &numberEntriesFile, &numberFeaturesFile, &labelProvidedFile);
    std::cout << "Entries: " << numberEntriesFile << " Features: " << numberFeaturesFile << " LabelsProvided: "
              << labelProvidedFile << std::endl;
    if (!bool(labelProvidedFile)) {
        throw (std::logic_error("labels were not provided with dataset!"));
    }

    float dummyVarForIdentifier;
    std::vector<std::vector<float>> features(numberEntriesFile, std::vector<float>(numberFeaturesFile, 0));
    for (unsigned int row = 0; row < numberEntriesFile; ++row) {
        inFile >> dummyVarForIdentifier;
        inFile >> dummyVarForIdentifier;
        for (unsigned int col = 0; col < numberFeaturesFile; ++col) {
            inFile >> features[row][col];
        }
    }

    inFile.close();
    std::cout << "-----------------------" << std::endl << std::endl;
    return features;
}

void getDimensionAndDataTypeOfFile(QString &fileName, unsigned int &dimensionOut,
                                                  itk::ImageIOBase::IOComponentType &dataTypeOut) {
    char *fileNameC = new char[fileName.length() + 1];
    strcpy(fileNameC, fileName.toStdString().c_str());

    itk::ImageIOBase::Pointer imageIO =
            itk::ImageIOFactory::CreateImageIO(
                    fileNameC,
                    itk::ImageIOFactory::ReadMode);

    imageIO->SetFileName(fileNameC);
    imageIO->ReadImageInformation();

    dataTypeOut = imageIO->GetComponentType();
    dimensionOut = imageIO->GetNumberOfDimensions();

    delete[] fileNameC;
}