cmake_minimum_required(VERSION 3.25)
project(celer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Library ──
add_library(celer STATIC
    src/core/dispatch.cpp
    src/core/tree_builder.cpp
    src/backend/rocksdb.cpp
    src/backend/dynamic.cpp
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
