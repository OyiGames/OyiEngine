add_executable(TEST_RenderGraph
    RenderGraphTests.cpp
)

setup_output_directories(TEST_RenderGraph)

target_include_directories(TEST_RenderGraph PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Packages/googletest/googletest/include
)

target_link_libraries(TEST_RenderGraph PRIVATE
    OyiGraphic
    gtest
    gtest_main
)

target_link_libraries(TEST_RenderGraph PRIVATE fmt::fmt)

add_test(NAME TEST_RenderGraph COMMAND TEST_RenderGraph)
