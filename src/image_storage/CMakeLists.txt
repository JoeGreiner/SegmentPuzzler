include(FetchContent)
enable_language(C)
find_package(Threads REQUIRED)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Offline: -DFETCHCONTENT_SOURCE_DIR_STREAMVBYTE=/path/to/streamvbyte
FetchContent_Declare(streamvbyte
    URL https://codeload.github.com/fast-pack/streamvbyte/tar.gz/089549994faf3657c13e62f6a1a7b2f544308922
    URL_HASH SHA256=d43988746a5200d96d120bdcd6cac28e87ebaf6f0c80204254b4732c7678cafc
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(streamvbyte)

add_library(segmentpuzzler_streamvbyte STATIC
    ${streamvbyte_SOURCE_DIR}/src/streamvbyte_encode.c
    ${streamvbyte_SOURCE_DIR}/src/streamvbyte_decode.c)
target_include_directories(segmentpuzzler_streamvbyte PUBLIC ${streamvbyte_SOURCE_DIR}/include)
set_target_properties(segmentpuzzler_streamvbyte PROPERTIES
    C_STANDARD 99 POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
    target_compile_definitions(segmentpuzzler_streamvbyte PRIVATE __restrict__=__restrict)
endif()

add_library(segmentpuzzler_image_storage INTERFACE)
target_include_directories(segmentpuzzler_image_storage INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_features(segmentpuzzler_image_storage INTERFACE cxx_std_17)
target_link_libraries(segmentpuzzler_image_storage INTERFACE
    segmentpuzzler_streamvbyte ITK::ITKCommonModule Threads::Threads)
