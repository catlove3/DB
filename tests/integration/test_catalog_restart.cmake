if(NOT DEFINED TEST_PROGRAM OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TEST_PROGRAM and TEST_ROOT are required")
endif()

# Each phase runs in a fresh process. Only prepare initializes the data directory.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef test_suffix)
set(test_directory "${TEST_ROOT}/${test_suffix}")
file(MAKE_DIRECTORY "${test_directory}")
foreach(phase prepare verify_and_replace verify_final)
    execute_process(
        COMMAND "${TEST_PROGRAM}" "${phase}" "${test_directory}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
        TIMEOUT 20
    )
    if(NOT "${result}" STREQUAL "0")
        file(REMOVE_RECURSE "${test_directory}")
        message(FATAL_ERROR "Catalog restart phase ${phase} failed (${result})\n${output}${error_output}")
    endif()
endforeach()
file(REMOVE_RECURSE "${test_directory}")
