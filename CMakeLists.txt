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

add_library(db_common STATIC src/common/cli.cpp src/common/error.cpp)
target_include_directories(db_common PUBLIC "${PROJECT_SOURCE_DIR}/include")
idbs_configure_target(db_common)

add_library(db_storage STATIC
    src/storage/file_manager.cpp
    src/storage/buffer_pool.cpp
)
target_include_directories(db_storage PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(db_storage PUBLIC db_common)
idbs_configure_target(db_storage)

add_library(db_record STATIC src/record/record.cpp)
target_include_directories(db_record PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(db_record PUBLIC db_storage)
idbs_configure_target(db_record)

add_executable(mydb src/main.cpp)
target_link_libraries(mydb PRIVATE db_common db_storage)
idbs_configure_target(mydb)

# Add index/catalog/parser/execution libraries when they have real sources.
# SQL.g4 is a reference input; the starter does not link ANTLR yet.
include(CTest)
if(BUILD_TESTING)
    add_executable(unit_file_manager tests/unit/test_file_manager.cpp)
    target_link_libraries(unit_file_manager PRIVATE db_storage)
    idbs_configure_target(unit_file_manager)
    add_test(NAME unit_file_manager COMMAND unit_file_manager)
    set_tests_properties(unit_file_manager PROPERTIES TIMEOUT 30)

    # Linux fault injection wraps calls made by the static storage library.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(unit_storage_failures tests/unit/test_storage_failures.cpp)
        target_link_libraries(unit_storage_failures PRIVATE db_storage)
        idbs_configure_target(unit_storage_failures)
        target_link_options(unit_storage_failures PRIVATE
            -Wl,--wrap=close -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=unlink)
        add_test(NAME unit_storage_failures COMMAND unit_storage_failures)
        set_tests_properties(unit_storage_failures PROPERTIES TIMEOUT 30)
    endif()

    add_executable(unit_buffer_pool tests/unit/test_buffer_pool.cpp)
    target_link_libraries(unit_buffer_pool PRIVATE db_storage)
    idbs_configure_target(unit_buffer_pool)
    add_test(NAME unit_buffer_pool COMMAND unit_buffer_pool)
    set_tests_properties(unit_buffer_pool PROPERTIES TIMEOUT 30)

    add_executable(unit_record_page tests/unit/test_record_page.cpp)
    target_link_libraries(unit_record_page PRIVATE db_record)
    idbs_configure_target(unit_record_page)
    add_test(NAME unit_record_page COMMAND unit_record_page)
    set_tests_properties(unit_record_page PROPERTIES TIMEOUT 30)

    add_executable(unit_record_file tests/unit/test_record_file.cpp)
    target_link_libraries(unit_record_file PRIVATE db_record)
    idbs_configure_target(unit_record_file)
    add_test(NAME unit_record_file COMMAND unit_record_file)
    set_tests_properties(unit_record_file PROPERTIES TIMEOUT 30)

    add_executable(unit_row_codec tests/unit/test_row_codec.cpp)
    target_link_libraries(unit_row_codec PRIVATE db_record)
    idbs_configure_target(unit_row_codec)
    add_test(NAME unit_row_codec COMMAND unit_row_codec)
    set_tests_properties(unit_row_codec PROPERTIES TIMEOUT 30)

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
