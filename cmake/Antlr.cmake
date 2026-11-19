# Pinned, vendored dependencies: configuration and builds do not download files.
set(IDBS_ANTLR_VERSION "4.13.2")
set(IDBS_ANTLR_JAR "${PROJECT_SOURCE_DIR}/tools/antlr-${IDBS_ANTLR_VERSION}-complete.jar")
set(IDBS_ANTLR_ROOT "${PROJECT_SOURCE_DIR}/third_party/antlr4_runtime")

file(SHA256 "${IDBS_ANTLR_JAR}" IDBS_ANTLR_JAR_SHA256)
if(NOT IDBS_ANTLR_JAR_SHA256 STREQUAL "eae2dfa119a64327444672aff63e9ec35a20180dc5b8090b7a6ab85125df4d76")
    message(FATAL_ERROR "ANTLR tool checksum mismatch; restore tools/antlr-4.13.2-complete.jar")
endif()
file(STRINGS "${IDBS_ANTLR_ROOT}/VERSION" IDBS_ANTLR_RUNTIME_VERSION)
if(NOT IDBS_ANTLR_RUNTIME_VERSION STREQUAL IDBS_ANTLR_VERSION)
    message(FATAL_ERROR "ANTLR generator and C++ runtime versions must match")
endif()

find_package(Java 11 QUIET COMPONENTS Runtime)
if(NOT Java_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/tools/.java/bin/java")
    set(Java_JAVA_EXECUTABLE "${PROJECT_SOURCE_DIR}/tools/.java/bin/java"
        CACHE FILEPATH "Java executable for ANTLR generation" FORCE)
    find_package(Java 11 QUIET COMPONENTS Runtime)
endif()
if(NOT Java_FOUND)
    message(FATAL_ERROR "ANTLR requires Java 11+. Install a JRE or set Java_JAVA_EXECUTABLE; see tools/README.md")
endif()

# Upstream's tests fetch GoogleTest. Disable them and build only the static runtime.
set(ANTLR_BUILD_CPP_TESTS OFF)
set(ANTLR_BUILD_SHARED OFF)
set(ANTLR_BUILD_STATIC ON)
set(ANTLR4_INSTALL OFF)
set(WITH_DEMO OFF)
add_subdirectory("${IDBS_ANTLR_ROOT}" "${PROJECT_BINARY_DIR}/third_party/antlr4_runtime" EXCLUDE_FROM_ALL)
get_target_property(IDBS_ANTLR_INCLUDE_DIRS antlr4_static INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET antlr4_static PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${IDBS_ANTLR_INCLUDE_DIRS}")

set(IDBS_SQL_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated/sql")
set(IDBS_SQL_GENERATED_SOURCES
    "${IDBS_SQL_GENERATED_DIR}/SQLLexer.cpp"
    "${IDBS_SQL_GENERATED_DIR}/SQLParser.cpp"
    "${IDBS_SQL_GENERATED_DIR}/SQLBaseVisitor.cpp"
    "${IDBS_SQL_GENERATED_DIR}/SQLVisitor.cpp"
)
set(IDBS_SQL_GENERATED_HEADERS
    "${IDBS_SQL_GENERATED_DIR}/SQLLexer.h"
    "${IDBS_SQL_GENERATED_DIR}/SQLParser.h"
    "${IDBS_SQL_GENERATED_DIR}/SQLBaseVisitor.h"
    "${IDBS_SQL_GENERATED_DIR}/SQLVisitor.h"
)
add_custom_command(
    OUTPUT ${IDBS_SQL_GENERATED_SOURCES} ${IDBS_SQL_GENERATED_HEADERS}
    BYPRODUCTS
        "${IDBS_SQL_GENERATED_DIR}/SQL.interp"
        "${IDBS_SQL_GENERATED_DIR}/SQL.tokens"
        "${IDBS_SQL_GENERATED_DIR}/SQLLexer.interp"
        "${IDBS_SQL_GENERATED_DIR}/SQLLexer.tokens"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${IDBS_SQL_GENERATED_DIR}"
    COMMAND "${Java_JAVA_EXECUTABLE}" -jar "${IDBS_ANTLR_JAR}"
            -Dlanguage=Cpp -visitor -no-listener -Werror
            -o "${IDBS_SQL_GENERATED_DIR}" SQL.g4
    DEPENDS "${PROJECT_SOURCE_DIR}/grammar/SQL.g4" "${IDBS_ANTLR_JAR}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/grammar"
    COMMENT "Generating SQL lexer, parser and visitors with ANTLR ${IDBS_ANTLR_VERSION}"
    VERBATIM
)
add_custom_target(generate_sql_parser DEPENDS ${IDBS_SQL_GENERATED_SOURCES} ${IDBS_SQL_GENERATED_HEADERS})
add_library(db_sql_grammar STATIC ${IDBS_SQL_GENERATED_SOURCES} ${IDBS_SQL_GENERATED_HEADERS})
add_dependencies(db_sql_grammar generate_sql_parser)
target_include_directories(db_sql_grammar SYSTEM PUBLIC "${IDBS_SQL_GENERATED_DIR}")
target_link_libraries(db_sql_grammar PUBLIC antlr4_static)
target_compile_features(db_sql_grammar PUBLIC cxx_std_17)
set_target_properties(db_sql_grammar PROPERTIES CXX_EXTENSIONS OFF)

# Keep dependency/generated warnings separate, but instrument the entire parser stack.
if(IDBS_ENABLE_SANITIZERS)
    foreach(target antlr4_static db_sql_grammar)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} INTERFACE -fsanitize=address,undefined)
    endforeach()
endif()
