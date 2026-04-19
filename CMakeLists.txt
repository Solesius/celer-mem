cmake_minimum_required(VERSION 3.25)
project(celer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CELER_ENABLE_COVERAGE "Enable gcov/lcov coverage instrumentation" OFF)
if(CELER_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "CELER_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()

# ── Optional backend toggles (all default ON; auto-disabled if headers missing) ──
option(CELER_BUILD_ROCKSDB "Build with RocksDB backend" ON)
option(CELER_BUILD_QPDF    "Build with QPDF backend"    ON)

find_package(PkgConfig QUIET)

# Probe for RocksDB headers; if not found, silently turn the backend off.
if(CELER_BUILD_ROCKSDB)
    include(CheckIncludeFileCXX)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
    check_include_file_cxx("rocksdb/db.h" CELER_ROCKSDB_HEADER_FOUND)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT CELER_ROCKSDB_HEADER_FOUND)
        message(STATUS "RocksDB headers not found — disabling RocksDB backend")
        set(CELER_BUILD_ROCKSDB OFF)
    endif()
endif()

# Probe for QPDF headers; if not found, silently turn the backend off.
if(CELER_BUILD_QPDF)
    include(CheckIncludeFileCXX)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
    check_include_file_cxx("qpdf/QPDF.hh" CELER_QPDF_HEADER_FOUND)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT CELER_QPDF_HEADER_FOUND)
        message(STATUS "QPDF headers not found — disabling QPDF backend")
        set(CELER_BUILD_QPDF OFF)
    endif()
endif()

# ── Library sources ──
set(CELER_SOURCES
    src/core/dispatch.cpp
    src/core/tree_builder.cpp
    src/core/aws_credentials.cpp
    src/backend/sqlite.cpp
    src/backend/dynamic.cpp
    src/backend/s3.cpp
    src/api/global.cpp
)
if(CELER_BUILD_ROCKSDB)
    list(APPEND CELER_SOURCES src/backend/rocksdb.cpp)
endif()
if(CELER_BUILD_QPDF)
    list(APPEND CELER_SOURCES src/backend/qpdf.cpp)
endif()

add_library(celer STATIC ${CELER_SOURCES})

target_include_directories(celer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(celer PRIVATE -Wall -Wextra -Wpedantic -Werror)

# ── RocksDB dependency (optional) ──
if(CELER_BUILD_ROCKSDB)
    if(PkgConfig_FOUND)
        pkg_check_modules(ROCKSDB IMPORTED_TARGET rocksdb)
        if(ROCKSDB_FOUND)
            target_link_libraries(celer PUBLIC PkgConfig::ROCKSDB)
        else()
            target_link_libraries(celer PUBLIC rocksdb)
        endif()
    else()
        target_link_libraries(celer PUBLIC rocksdb)
    endif()
else()
    target_compile_definitions(celer PUBLIC CELER_FORCE_NO_ROCKSDB=1)
endif()

# ── SQLite3 dependency ──
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 IMPORTED_TARGET sqlite3)
    if(SQLITE3_FOUND)
        target_link_libraries(celer PUBLIC PkgConfig::SQLITE3)
    else()
        target_link_libraries(celer PUBLIC sqlite3)
    endif()
else()
    target_link_libraries(celer PUBLIC sqlite3)
endif()

# ── QPDF dependency (optional) ──
if(CELER_BUILD_QPDF AND PkgConfig_FOUND)
    pkg_check_modules(QPDF IMPORTED_TARGET libqpdf)
    if(QPDF_FOUND)
        target_link_libraries(celer PUBLIC PkgConfig::QPDF)
    else()
        message(STATUS "QPDF not found — disabling QPDF backend")
        set(CELER_BUILD_QPDF OFF)
    endif()
endif()
if(NOT CELER_BUILD_QPDF)
    target_compile_definitions(celer PUBLIC CELER_FORCE_NO_QPDF=1)
endif()

# ── Google Test ──
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# ── AWS SDK for C++ (S3 + STS) ──
option(CELER_BUILD_S3 "Build with AWS S3 support" ON)
if(CELER_BUILD_S3)
    set(BUILD_ONLY "s3;sts" CACHE STRING "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(AUTORUN_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(MINIMIZE_SIZE ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        awssdk
        GIT_REPOSITORY https://github.com/aws/aws-sdk-cpp.git
        GIT_TAG        1.11.370
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(awssdk)

    target_link_libraries(celer PUBLIC aws-cpp-sdk-s3 aws-cpp-sdk-sts aws-cpp-sdk-core)
endif()

# ── Tests ──
option(CELER_BUILD_TESTS "Build unit tests" ON)
if(CELER_BUILD_TESTS)
    enable_testing()

    function(celer_add_gtest target source label)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE celer GTest::gtest_main)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        gtest_discover_tests(${target}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "${label}"
        )
    endfunction()

    celer_add_gtest(celer_tests tests/main.cpp unit)
    celer_add_gtest(celer_integration tests/integration.cpp integration)
    celer_add_gtest(celer_sqlite_tests tests/test_sqlite.cpp sqlite)
    celer_add_gtest(celer_async_tests tests/test_async.cpp async)
    celer_add_gtest(celer_qpdf_tests tests/test_qpdf.cpp qpdf)
    celer_add_gtest(celer_materialization_tests tests/test_materialization.cpp materialization)
    celer_add_gtest(celer_core_coverage_tests tests/test_core_coverage.cpp unit)

    # option(LOCAL_DEV_INTEGRATION_SUITE "Build local-dev integration tests (S3 integ, requires AWS creds)" OFF)
    # if(LOCAL_DEV_INTEGRATION_SUITE)
    #     add_executable(celer_s3_integ tests/test_s3_integ.cpp)
    #     target_link_libraries(celer_s3_integ PRIVATE celer GTest::gtest)
    #     target_compile_options(celer_s3_integ PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    #     add_test(NAME s3_integ COMMAND celer_s3_integ)
    # endif()

    if(CELER_ENABLE_COVERAGE)
        find_program(LCOV_EXECUTABLE lcov)
        find_program(GENHTML_EXECUTABLE genhtml)
        if(NOT LCOV_EXECUTABLE OR NOT GENHTML_EXECUTABLE)
            message(FATAL_ERROR "CELER_ENABLE_COVERAGE requires lcov and genhtml")
        endif()

        set(CELER_COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${CELER_COVERAGE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CELER_COVERAGE_DIR}
            COMMAND ${LCOV_EXECUTABLE} --directory ${CMAKE_BINARY_DIR} --zerocounters
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${LCOV_EXECUTABLE}
                --capture
                --directory ${CMAKE_BINARY_DIR}
                --output-file ${CELER_COVERAGE_DIR}/coverage.info
                --ignore-errors mismatch,negative,source
            COMMAND ${LCOV_EXECUTABLE}
                --remove ${CELER_COVERAGE_DIR}/coverage.info
                /usr/*
                ${CMAKE_BINARY_DIR}/_deps/*
                ${CMAKE_SOURCE_DIR}/tests/*
                --output-file ${CELER_COVERAGE_DIR}/coverage.filtered.info
                --ignore-errors unused
            COMMAND ${GENHTML_EXECUTABLE}
                ${CELER_COVERAGE_DIR}/coverage.filtered.info
                --output-directory ${CELER_COVERAGE_DIR}/html
                --legend
                --title "celer-mem coverage"
            DEPENDS
                celer_tests
                celer_integration
                celer_sqlite_tests
                celer_async_tests
                celer_qpdf_tests
                celer_materialization_tests
                celer_core_coverage_tests
            USES_TERMINAL
        )
    endif()
endif()

# ── Examples ──
option(CELER_BUILD_EXAMPLES "Build example programs" OFF)
if(CELER_BUILD_EXAMPLES)
    file(GLOB EXAMPLE_SOURCES examples/*.cpp)
    foreach(src ${EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(example_${name} ${src})
        target_link_libraries(example_${name} PRIVATE celer)
    endforeach()
endif()

# ── Install ──
include(GNUInstallDirs)
install(TARGETS celer
    EXPORT celerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/celer
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT celerTargets
    FILE celerConfig.cmake
    NAMESPACE celer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/celer
)

# ── Package config version ──
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/celerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/celerConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/celer
)
