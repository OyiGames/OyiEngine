set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "Do not treat warnings as errors" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Disable building unit tests for benchmark" FORCE)
set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "Do not use bundled gtest" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Packages/benchmark EXCLUDE_FROM_ALL)
set_target_properties(benchmark PROPERTIES
    MAP_IMPORTED_CONFIG_DEBUG RELEASE
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
)