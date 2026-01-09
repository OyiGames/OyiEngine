project(BM_GTL)

file(GLOB_RECURSE src CONFIGURE_DEPENDS "*.cpp" "*.h")

add_executable(BM_GTL ${src})
setup_output_directories(BM_GTL)

target_include_directories(BM_GTL PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/gtl/include)
target_include_directories(BM_GTL PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/benchmark/include)

target_link_libraries(BM_GTL PRIVATE benchmark)