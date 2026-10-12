cmake_minimum_required(VERSION 3.21)
project(IDBS VERSION 0.1.0 LANGUAGES CXX)

# Set these per target so third-party dependencies can use their own warnings.
function(idbs_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(db_common STATIC src/common/cli.cpp)
target_include_directories(db_common PUBLIC "${PROJECT_SOURCE_DIR}/include")
idbs_configure_target(db_common)

add_executable(mydb src/main.cpp)
target_link_libraries(mydb PRIVATE db_common)
idbs_configure_target(mydb)

# Add storage/record/index/catalog/parser/execution libraries when they have
# real sources. SQL.g4 is a reference input; the starter does not link ANTLR yet.
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME cli_integration
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/test_cli.py"
                    "$<TARGET_FILE:mydb>"
        )
        set_tests_properties(cli_integration PROPERTIES TIMEOUT 30)
    else()
        message(STATUS "Python3 not found: CLI integration tests are unavailable")
    endif()
endif()
