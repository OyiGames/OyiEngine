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
    add_compile_options(/Zc:preprocessor)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

include(CMake/OutputDirectories.cmake)
include(cmake/CopySpecifiedFilesToBuild.cmake)

include(CMake/Release/fmt_release.cmake)
setup_output_directories(fmt)

include(CMake/Release/mimalloc_release.cmake)
setup_output_directories(mimalloc-static)

include(CMake/Release/benchmark_release.cmake)
setup_output_directories(benchmark)

include(CMake/Release/gtest_release.cmake)
setup_output_directories(gtest_lib)

include(CMake/Release/sdl_release.cmake)
setup_output_directories(SDL3-shared)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
include(CMake/Debug/nri_debug.cmake)
setup_output_directories(NRI)
else()
include(CMake/Release/nri_release.cmake)
setup_output_directories(NRI)
endif()

add_subdirectory(Engine/Benchmarks/Malloc)
add_subdirectory(Engine/Benchmarks/GTL)
# add_subdirectory(Engine/Modules/Graphic)
# add_subdirectory(Engine/Tests/Graphic/RDG)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set_property(TARGET 
    fmt    
    benchmark
    gtest
    gtest_main
    mimalloc-static
    SDL3-shared
    SDL_uclibc
    PROPERTY FOLDER "Packages"
)

set_property(TARGET 
    BM_GTL
    BM_Malloc
    PROPERTY FOLDER "Benchmarks"
)

# set_property(TARGET 
#     TEST_RenderGraph
#     PROPERTY FOLDER "Tests"
# )