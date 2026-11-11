get_filename_component(output_dir "${OUTPUT_ICNS}" DIRECTORY)
set(iconset "${output_dir}/SegmentPuzzler.iconset")
file(MAKE_DIRECTORY "${iconset}")

foreach(size IN ITEMS 16 32 128 256 512)
    foreach(scale IN ITEMS 1 2)
        math(EXPR pixels "${size} * ${scale}")
        set(suffix "")
        if(scale EQUAL 2)
            set(suffix "@2x")
        endif()
        execute_process(
            COMMAND sips -z "${pixels}" "${pixels}" "${ICON_PNG}"
                --out "${iconset}/icon_${size}x${size}${suffix}.png"
            RESULT_VARIABLE result OUTPUT_QUIET)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Failed to generate the ${pixels}px macOS icon: ${result}")
        endif()
    endforeach()
endforeach()

execute_process(
    COMMAND iconutil -c icns -o "${OUTPUT_ICNS}" "${iconset}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to assemble the macOS icon: ${result}")
endif()
