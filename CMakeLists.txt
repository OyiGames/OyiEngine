cmake_minimum_required(VERSION 3.19)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project(Oyi LANGUAGES CXX)

add_definitions(-D_UNICODE)
add_definitions(-DUNICODE)

if(PROJECT_BINARY_DIR STREQUAL PROJECT_SOURCE_DIR)
    message(WARNING "The Binary directory of CMake cannot be the same as source directory!")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
    add_definitions(-DNOMINMAX -D_USE_MATH_DEFINES)
endif()

if (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

include(CMake/OutputDirectories.cmake)

option(ENGINE_USE_BENCHMARK "Enable Google Benchmark" ON)
option(ENGINE_USE_GTEST "Enable GoogleTest" ON)

include(CMake/Release/mimalloc_release.cmake)
setup_output_directories(mimalloc-static)

if(ENGINE_USE_BENCHMARK)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "Do not treat warnings as errors" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Disable building unit tests for benchmark" FORCE)
    set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "Do not use bundled gtest" FORCE)
    add_subdirectory(Engine/Packages/benchmark EXCLUDE_FROM_ALL)
    set_target_properties(benchmark PROPERTIES
        MAP_IMPORTED_CONFIG_DEBUG RELEASE
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
    )
    setup_output_directories(benchmark)
endif()

if (ENGINE_USE_GTEST)
    include(CMake/Release/gtest_release.cmake)
    setup_output_directories(gtest_lib)
endif()

# if (CMAKE_BUILD_TYPE STREQUAL "Debug")
# else()
# endif()

add_subdirectory(Engine/Tests/Malloc)
add_subdirectory(Engine/Tests/GTL)