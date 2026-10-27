cmake_minimum_required(VERSION 3.21)
project(IDBS VERSION 0.1.0 LANGUAGES CXX)

option(IDBS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(IDBS_ENABLE_SANITIZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "IDBS_ENABLE_SANITIZERS requires GCC or Clang")
endif()

# Set these per target so third-party dependencies can use their own warnings.
function(idbs_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(IDBS_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(db_common STATIC src/common/cli.cpp)
target_include_directories(db_common PUBLIC "${PROJECT_SOURCE_DIR}/include")
idbs_configure_target(db_common)

add_library(db_storage STATIC
    src/storage/file_manager.cpp
    src/storage/buffer_pool.cpp
)
target_include_directories(db_storage PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(db_storage PUBLIC db_common)
idbs_configure_target(db_storage)

add_executable(mydb src/main.cpp)
target_link_libraries(mydb PRIVATE db_common db_storage)
idbs_configure_target(mydb)

# Add record/index/catalog/parser/execution libraries when they have real
# sources. SQL.g4 is a reference input; the starter does not link ANTLR yet.
include(CTest)
if(BUILD_TESTING)
    add_executable(unit_file_manager tests/unit/test_file_manager.cpp)
    target_link_libraries(unit_file_manager PRIVATE db_storage)
    idbs_configure_target(unit_file_manager)
    add_test(NAME unit_file_manager COMMAND unit_file_manager)
    set_tests_properties(unit_file_manager PROPERTIES TIMEOUT 30)

    add_executable(unit_buffer_pool tests/unit/test_buffer_pool.cpp)
    target_link_libraries(unit_buffer_pool PRIVATE db_storage)
    idbs_configure_target(unit_buffer_pool)
    add_test(NAME unit_buffer_pool COMMAND unit_buffer_pool)
    set_tests_properties(unit_buffer_pool PROPERTIES TIMEOUT 30)

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
