foreach(workers IN ITEMS 1 4 7)
    execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "OMP_NUM_THREADS=${workers}"
                "ITK_GLOBAL_DEFAULT_NUMBER_OF_THREADS=${workers}"
                "VTK_SMP_MAX_THREADS=${workers}"
                "${PROGRAM}" --fingerprint
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors
            TIMEOUT 120)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "${PROGRAM} failed with ${workers} workers: ${errors}")
    endif ()
    if (workers EQUAL 1)
        set(reference "${output}")
    elseif (NOT output STREQUAL reference)
        message(FATAL_ERROR "${PROGRAM} produced different results with ${workers} workers.")
    endif ()
endforeach()
