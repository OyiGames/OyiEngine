project(Test_GTL)

file(GLOB_RECURSE src CONFIGURE_DEPENDS "*.cpp" "*.h")

add_executable(Test_GTL ${src})
setup_output_directories(Test_GTL)

target_include_directories(Test_GTL PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/gtl/include)
target_include_directories(Test_GTL PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Packages/benchmark/include)

target_link_libraries(Test_GTL PRIVATE benchmark)