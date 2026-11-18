include(FetchContent)
enable_language(C)

function(segmentpuzzler_add_zlib_ng)
    # A shared library isolates internal symbols from ITK's bundled zlib-ng.
    set(BUILD_SHARED_LIBS ON)
    set(BUILD_TESTING OFF)
    set(ZLIB_COMPAT OFF)
    set(ZLIB_ALIASES OFF)
    set(ZLIB_SYMBOL_PREFIX sp_)
    set(WITH_NATIVE_INSTRUCTIONS OFF)
    set(WITH_RUNTIME_CPU_DETECTION ON)
    set(SKIP_INSTALL_ALL ON)
    FetchContent_Declare(zlib_ng
        URL https://codeload.github.com/zlib-ng/zlib-ng/tar.gz/a04670521df1c4274a23b7fc78478509838f74a2
        URL_HASH SHA256=ab2f54a2b422a444ab36df5ca91292c2db145cec51d12908e06c5df7a6bcca49)
    FetchContent_MakeAvailable(zlib_ng)
    set_target_properties(zlib-ng PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        OUTPUT_NAME segmentpuzzler_zlib_ng RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

segmentpuzzler_add_zlib_ng()
