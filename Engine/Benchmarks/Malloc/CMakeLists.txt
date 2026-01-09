project(BM_Malloc)

file(GLOB_RECURSE src CONFIGURE_DEPENDS "*.cpp" "*.h")

add_executable(BM_Malloc ${src})
setup_output_directories(BM_Malloc)

target_include_directories(BM_Malloc PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/benchmark/include)
target_include_directories(BM_Malloc PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/mimalloc/include)

target_link_libraries(BM_Malloc PRIVATE mimalloc-static)
target_link_libraries(BM_Malloc PRIVATE benchmark)