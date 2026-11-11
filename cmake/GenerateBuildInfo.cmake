set(GIT_DESCRIPTION "unknown")
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --show-toplevel
            OUTPUT_VARIABLE git_root OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    get_filename_component(git_root "${git_root}" REALPATH)
    get_filename_component(source_root "${SOURCE_DIR}" REALPATH)
    # A source archive inside another repository must not inherit its Git identity.
    if (git_root STREQUAL source_root)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --always --long
                OUTPUT_VARIABLE description OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE describe_result ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
                status --porcelain --untracked-files=normal --ignore-submodules=none
                OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE status_result ERROR_QUIET)
        if (describe_result EQUAL 0 AND status_result EQUAL 0)
            set(GIT_DESCRIPTION "${description}")
            if (NOT changes STREQUAL "")
                string(APPEND GIT_DESCRIPTION "-dirty")
            endif ()
        endif ()
    endif ()
endif ()

string(REPLACE "\\" "\\\\" GIT_DESCRIPTION "${GIT_DESCRIPTION}")
string(REPLACE "\"" "\\\"" GIT_DESCRIPTION "${GIT_DESCRIPTION}")
# configure_file preserves the timestamp when the build information is unchanged.
configure_file("${SOURCE_DIR}/cmake/BuildInfo.h.in" "${OUTPUT_FILE}" @ONLY)
