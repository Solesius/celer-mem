cmake_minimum_required(VERSION 3.25)
project(celer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Library ──
add_library(celer STATIC
    src/core/dispatch.cpp
    src/core/tree_builder.cpp
    src/core/aws_credentials.cpp
    src/backend/rocksdb.cpp
    src/backend/sqlite.cpp
    src/backend/dynamic.cpp
    src/backend/s3.cpp
    src/api/global.cpp
)

target_include_directories(celer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(celer PRIVATE -Wall -Wextra -Wpedantic -Werror)

# ── RocksDB dependency ──
find_package(PkgConfig QUIET)
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

# ── Google Test ──
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

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

    add_executable(celer_tests tests/main.cpp)
    target_link_libraries(celer_tests PRIVATE celer)
    target_compile_options(celer_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME unit COMMAND celer_tests)

    add_executable(celer_integration tests/integration.cpp)
    target_link_libraries(celer_integration PRIVATE celer)
    target_compile_options(celer_integration PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME integration COMMAND celer_integration)

    add_executable(celer_sqlite_tests tests/test_sqlite.cpp)
    target_link_libraries(celer_sqlite_tests PRIVATE celer)
    target_compile_options(celer_sqlite_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME sqlite COMMAND celer_sqlite_tests)

    add_executable(celer_async_tests tests/test_async.cpp)
    target_link_libraries(celer_async_tests PRIVATE celer GTest::gtest_main)
    target_compile_options(celer_async_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME async COMMAND celer_async_tests)

    add_executable(celer_necto_tests tests/test_necto.cpp)
    target_link_libraries(celer_necto_tests PRIVATE celer GTest::gtest_main)
    target_compile_options(celer_necto_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME necto COMMAND celer_necto_tests)

    # option(LOCAL_DEV_INTEGRATION_SUITE "Build local-dev integration tests (S3 integ, requires AWS creds)" OFF)
    # if(LOCAL_DEV_INTEGRATION_SUITE)
    #     add_executable(celer_s3_integ tests/test_s3_integ.cpp)
    #     target_link_libraries(celer_s3_integ PRIVATE celer GTest::gtest)
    #     target_compile_options(celer_s3_integ PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    #     add_test(NAME s3_integ COMMAND celer_s3_integ)
    # endif()
endif()

# ── nlohmann/json (for LLM swarm example) ──
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ── Examples ──
option(CELER_BUILD_EXAMPLES "Build example programs" OFF)
if(CELER_BUILD_EXAMPLES)
    find_package(CURL REQUIRED)

    file(GLOB EXAMPLE_SOURCES examples/*.cpp)
    foreach(src ${EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(example_${name} ${src})
        target_link_libraries(example_${name} PRIVATE celer)
    endforeach()

    # LLM swarm example needs libcurl + nlohmann/json
    if(TARGET example_08_llm_swarm)
        target_link_libraries(example_08_llm_swarm PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
    endif()
endif()

# ── Visualizations ──
option(CELER_BUILD_VIZ "Build visualization tools" OFF)
if(CELER_BUILD_VIZ)
    file(GLOB VIZ_SOURCES viz/*.cpp)
    foreach(src ${VIZ_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(viz_${name} ${src})
        target_link_libraries(viz_${name} PRIVATE celer)
        target_compile_options(viz_${name} PRIVATE -Wall -Wextra -Wpedantic)
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
