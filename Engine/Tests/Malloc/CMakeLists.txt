project(Test_Malloc)

file(GLOB_RECURSE src CONFIGURE_DEPENDS "*.cpp" "*.h")

add_executable(Test_Malloc ${src})
setup_output_directories(Test_Malloc)

target_include_directories(Test_Malloc PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/benchmark/include)
target_include_directories(Test_Malloc PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/mimalloc/include)

target_link_libraries(Test_Malloc PRIVATE mimalloc-static)
target_link_libraries(Test_Malloc PRIVATE benchmark)