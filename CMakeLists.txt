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

target_include_directories(celer PUBLIC include)
target_compile_options(celer PRIVATE -Wall -Wextra -Wpedantic -Werror)

# ── Tests ──
option(CELER_BUILD_TESTS "Build unit tests" ON)
if(CELER_BUILD_TESTS)
    enable_testing()
    add_executable(celer_tests
        tests/main.cpp
    )
    target_link_libraries(celer_tests PRIVATE celer)
    target_compile_options(celer_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME celer_tests COMMAND celer_tests)
endif()
