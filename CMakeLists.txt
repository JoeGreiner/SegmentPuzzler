cmake_minimum_required(VERSION 3.10.0)
project(SegmentPuzzler LANGUAGES CXX VERSION 0.2.0)
string(TOLOWER "${PROJECT_NAME}" PROJECT_NAME_LOWER)
set(CMAKE_CXX_STANDARD 14)
set(BUILD_SHARED_LIBS ON)

message(STATUS "Building SegmentPuzzler Version ${PROJECT_VERSION}")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
            "None Debug Release RelWithDebInfo MinSizeRel"
            FORCE)
endif ()

option(DEPLOY "Enable deployment settings" TRUE)
option(LINK_TIME_OPTIMIZATION "Enable Link Time Optimization" TRUE)
option(BUILD_TESTING "Enable testing" FALSE)
option(RUN_WITH_ASAN "Enable Address Sanitizer" FALSE)
option(RUN_WITH_TSAN "Enable Thread Sanitizer" FALSE)
option(ADD_DEBUG_SYMBOL_O0 "Add debug symbols and O0" FALSE)
option(USE_OMP "Enable OpenMP" FALSE)


set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
if (UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif ()

MESSAGE(STATUS "CMAKE_BUILD_TYPE: " ${CMAKE_BUILD_TYPE})
MESSAGE(STATUS "DEPLOY: " ${DEPLOY})
MESSAGE(STATUS "LINK_TIME_OPTIMIZATION: " ${LINK_TIME_OPTIMIZATION})
MESSAGE(STATUS "BUILD_TESTING: " ${BUILD_TESTING})
MESSAGE(STATUS "UNIX: " ${UNIX})
MESSAGE(STATUS "LINUX: " ${LINUX})
MESSAGE(STATUS "APPLE: " ${APPLE})
MESSAGE(STATUS "MSVC: " ${MSVC})
MESSAGE(STATUS "RUN_WITH_ASAN: " ${RUN_WITH_ASAN})
MESSAGE(STATUS "RUN_WITH_TSAN: " ${RUN_WITH_TSAN})
MESSAGE(STATUS "ADD_DEBUG_SYMBOL_O0: " ${ADD_DEBUG_SYMBOL_O0})
MESSAGE(STATUS "USE_OMP: " ${USE_OMP})

### Apple compile options
if (APPLE AND DEPLOY)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum OS X deployment version" FORCE)
    set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR})
    set(prefix "${PROJECT_NAME}.app/Contents")
    set(INSTALL_RUNTIME_DIR "${prefix}/MacOS")
    set(INSTALL_CMAKE_DIR "${prefix}/Resources")
#    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I/usr/local/include")

    set(MACOSX_BUNDLE_ICON_FILE "SegmentPuzzler.icns")
    set(APP_ICON "${CMAKE_SOURCE_DIR}/resources/images/SegmentPuzzler.icns")
    MESSAGE(STATUS "Using APP_ICON: " ${APP_ICON})
    set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif ()

### MSVC compile options
if (MSVC)
    MESSAGE(STATUS "Using MSVC")
    if (CMAKE_BUILD_TYPE MATCHES RELEASE)
        add_compile_options(/Ox)
    endif ()
    add_compile_options(/F 8388608)
    if (USE_OMP)
        add_compile_options(/openmp)
    endif ()
endif ()

### Linux compile options
if (UNIX)
    if (USE_OMP)
        add_compile_options(-fopenmp)
    endif ()
endif ()

if (UNIX AND RELEASE)
    add_compile_options(-Ofast)
    if (LINK_TIME_OPTIMIZATION)
        add_compile_options(-flto)
    endif ()
endif ()

if (UNIX AND DEBUG)
    add_compile_options(-O2 -g3)
endif ()

if (RUN_WITH_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
    MESSAGE(STATUS "Running with Address Sanitizer")
endif ()

if (RUN_WITH_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    MESSAGE(STATUS "Running with Thread Sanitizer")
endif ()

if (ADD_DEBUG_SYMBOL_O0)
    add_compile_options(-g -O0)
    MESSAGE(STATUS "Adding debug symbols and O0")
endif ()

find_package(Qt5 COMPONENTS Core Gui Widgets QUIET)
if (NOT Qt5Widgets_FOUND)
    message(STATUS "Qt5 not found. Please install Qt5 and set Qt5_DIR to the path to Qt5Config.cmake")
    set(Qt5_DIR "" CACHE PATH "Path to QT, e.g.  /home/user/Qt/5.14.2/gcc_64/lib/cmake/Qt5/")
    if (NOT EXISTS "${Qt5_DIR}")
        message(FATAL_ERROR "Qt5_DIR is not set or invalid. Please provide the path to -DQt5_DIR=<path>, e.g.\n"
                "Linux: cmake -DQt5_DIR=/home/user/Qt/5.14.2/gcc_64/lib/cmake/Qt5/ build/\n"
                "Windows: cmake -DQt5_DIR=C:/Qt/5.14.2/msvc2017_64/lib/cmake/Qt5/ build/\n"
                "Mac: cmake -DQt5_DIR=/Users/user/Qt/5.14.2/clang_64/lib/cmake/Qt5/ build/")
    endif ()
endif ()

find_package(Qt5 COMPONENTS Widgets Network)

find_package(ITK REQUIRED QUIET)
if (NOT ITK_FOUND)
    message(STATUS "ITK not found. Please install ITK and set ITK_DIR to the path to ITKConfig.cmake")
    set(ITK_DIR "" CACHE PATH "Path to ITK, e.g.  /home/user/ITK/build/")
    if (NOT EXISTS "${ITK_DIR}")
        message(FATAL_ERROR "ITK_DIR is not set or invalid. Please provide the path to -DITK_DIR=<path>, e.g.\n"
                "Linux: cmake -DITK_DIR=/home/user/ITK/build/ build/\n"
                "Windows: cmake -DITK_DIR=C:/ITK/build/ build/\n"
                "Mac: cmake -DITK_DIR=/Users/user/ITK/build/ build/")
    endif ()
endif ()
include(${ITK_USE_FILE})

if (MSVC) # not needed for Mac
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND)
        # look for ssl installation in qt, can be installed with build tools -> openssl -> binaries
#            set(OPENSSL_ROOT_DIR "${Qt5_DIR}/../../../../../Tools/OpenSSLv3/Win_x64")
#            set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
#            set(OPENSSL_BIN_DIR "${OPENSSL_ROOT_DIR}/bin")
#            set(OPENSSL_LIBRARIES "${OPENSSL_ROOT_DIR}/lib/libssl.lib;${OPENSSL_ROOT_DIR}/lib/libcrypto.lib")


            # for https://slproweb.com/download/Win32OpenSSL-1_1_1w.exe installer
            set(OPENSSL_ROOT_DIR "C:/Program Files (x86)/OpenSSL-Win32")
            set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
            set(OPENSSL_LIBRARIES "${OPENSSL_ROOT_DIR}/lib/libssl.lib;${OPENSSL_ROOT_DIR}/lib/libcrypto.lib")
            set(OPENSSL_BIN_DIR "${OPENSSL_ROOT_DIR}/bin")

            message(STATUS "  Includes Guess: ${OPENSSL_INCLUDE_DIR}")
            message(STATUS "  Libraries Guess: ${OPENSSL_LIBRARIES}")
            message(STATUS "  Binaries Guess: ${OPENSSL_BIN_DIR}")


        # Locate OpenSSL again
        find_package(OpenSSL)

        if (NOT OpenSSL_FOUND)
            message(STATUS "OpenSSL not found! SSL/TLS support is required for downloading sample files")
        else()
            message(STATUS "OpenSSL found:")
            message(STATUS "  Includes: ${OPENSSL_INCLUDE_DIR}")
            message(STATUS "  Libraries: ${OPENSSL_LIBRARIES}")
            message(STATUS "  Binaries: ${OPENSSL_BIN_DIR}")
        endif ()
    endif ()
endif()


MESSAGE(STATUS "Qt5_DIR: " ${Qt5_DIR})
MESSAGE(STATUS "ITK_USE_FILE: " ${ITK_USE_FILE})
MESSAGE(STATUS "Qt5Widgets_INCLUDE_DIRS:" ${Qt5Widgets_INCLUDE_DIRS})



# print compile options
MESSAGE(STATUS "CMAKE_CXX_FLAGS: " ${CMAKE_CXX_FLAGS})
MESSAGE(STATUS "CMAKE_CXX_FLAGS_RELEASE: " ${CMAKE_CXX_FLAGS_RELEASE})
MESSAGE(STATUS "CMAKE_CXX_FLAGS_DEBUG: " ${CMAKE_CXX_FLAGS_DEBUG})
MESSAGE(STATUS "CMAKE_CXX_FLAGS_RELWITHDEBINFO: " ${CMAKE_CXX_FLAGS_RELWITHDEBINFO})
MESSAGE(STATUS "CMAKE_CXX_FLAGS_MINSIZEREL: " ${CMAKE_CXX_FLAGS_MINSIZEREL})


# the order of src file matters! forward declarations!
file(GLOB_RECURSE SRCS
        "src/qtUtils/*.cpp"
        "src/controllers/*.cpp"
        )

set(SRC_VIEWERS
        src/viewers/fileIO.cpp
        src/viewers/itkSignal.cpp
        src/viewers/SliceViewerITKSignal.cpp
        src/viewers/SliceViewer.cpp
        src/viewers/AnnotationSliceViewer.cpp
        src/viewers/OrthoViewer.cpp
        )

set(SRC_UTILS
        src/utils/roi.cpp
        src/utils/voxel.cpp
        src/utils/utils.cpp
        src/utils/PCA.cxx
        )

set(SRC_SEGMENTHANDLERS
        src/segment_handling/graph.cpp
        src/segment_handling/graphBase.cpp
        src/segment_handling/initialEdge.cpp
        src/segment_handling/baseEdge.cpp
        src/segment_handling/workingEdge.cpp
        src/segment_handling/baseNode.cpp
        src/segment_handling/initialNode.cpp
        src/segment_handling/workingNode.cpp
        src/segment_handling/feature.cpp
        src/segment_handling/SegmentManager.cpp
        )

set(QT_RESOURCES
        resources/icon.qrc
        qdarkstyle/dark/style.qrc
        )
set(SRC_CORRECT_ORDER ${SRC_UTILS} ${SRC_SEGMENTHANDLERS} ${SRC_VIEWERS} ${SRCS} ${QT_RESOURCES})




if (APPLE)
    MESSAGE(STATUS "Using Bundle for MacOS")
    add_executable(SegmentPuzzler MACOSX_BUNDLE main.cpp mainWindow.cpp ${SRC_CORRECT_ORDER} ${APP_ICON})
    set_target_properties(SegmentPuzzler PROPERTIES
            MACOSX_BUNDLE_ICON_FILE ${MACOSX_BUNDLE_ICON_FILE}
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/Info.plist.in
            RESOURCE "${APP_ICON}"
            )
else ()
    add_executable(SegmentPuzzler main.cpp mainWindow.cpp ${SRC_CORRECT_ORDER})
endif ()


target_link_libraries(SegmentPuzzler ${ITK_LIBRARIES} Qt5::Widgets Qt5::Network ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})

if (MSVC)
    file(GLOB OPENSSL_DLLS "${OPENSSL_BIN_DIR}/*.dll")
    if (OPENSSL_DLLS)
        message(STATUS "Copying OpenSSL DLLs from ${OPENSSL_BIN_DIR} to target directory.")
        foreach(DLL ${OPENSSL_DLLS})
            add_custom_command(TARGET SegmentPuzzler POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DLL} $<TARGET_FILE_DIR:SegmentPuzzler>)
        endforeach()
    else()
        message(WARNING "No OpenSSL DLLs found in ${OPENSSL_BIN_DIR}. Ensure OpenSSL is installed and the path is correct.")
    endif()
endif()


# copy the sample data to the build directory
# now done on the fly in the code
#set(SAMPLE_SEGMENTATION_FILE "sampleData/Stack.nrrd")
#configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${SAMPLE_SEGMENTATION_FILE} ${CMAKE_CURRENT_BINARY_DIR}/${SAMPLE_SEGMENTATION_FILE} COPYONLY)

if (BUILD_TESTING)
    add_executable(profile_SliceViewer examples/viewers/profile_sliceviewer.cpp ${SRC_CORRECT_ORDER})
    add_executable(test_SliceViewer examples/viewers/example_sliceviewer.cpp ${SRC_CORRECT_ORDER})
    add_executable(test_OrthoViewer examples/viewers/example_orthoviewer.cpp ${SRC_CORRECT_ORDER})
#    add_executable(test_customwatershed examples/viewers/example_orthoviewer.cpp ${SRC_CORRECT_ORDER})
#    add_executable(test_watershed examples/viewers/example_watershed.cpp ${SRC_CORRECT_ORDER})
#    add_executable(test_watershedControl examples/viewers/example_watershedControl.cpp ${SRC_CORRECT_ORDER})
    target_link_libraries(profile_SliceViewer ${ITK_LIBRARIES} Qt5::Widgets ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})
    target_link_libraries(test_SliceViewer ${ITK_LIBRARIES} Qt5::Widgets ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})
    target_link_libraries(test_OrthoViewer ${ITK_LIBRARIES} Qt5::Widgets ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})
#    target_link_libraries(test_customwatershed ${ITK_LIBRARIES} Qt5::Widgets ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})
#    target_link_libraries(test_watershed ${ITK_LIBRARIES} Qt5::Widgets ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})
#    target_link_libraries(test_watershedControl ${ITK_LIBRARIES} Qt5::Widgets ${HDF5_CXX_LIBRARIES} ${HDF5_LIBRARIES})
endif ()



if (DEPLOY)
    if (APPLE)
        include(cmake/AppleDeploy.cmake)
    elseif (MSVC)
        include(cmake/WindowsDeploy.cmake)
    elseif (LINUX)
        include(cmake/UnixDeploy.cmake)
    endif ()
endif ()
