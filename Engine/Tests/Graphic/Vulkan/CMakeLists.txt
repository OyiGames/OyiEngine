add_executable(TEST_Vulkan
    VulkanContextTests.cpp
)

setup_output_directories(TEST_Vulkan)

target_include_directories(TEST_Vulkan PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Packages/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/Engine/Modules/Graphic/src
)

target_link_libraries(TEST_Vulkan PRIVATE
    OyiGraphic
    Vulkan::Vulkan
    OyiVMA
    SDL3::SDL3
    gtest_main
)

add_test(NAME TEST_Vulkan COMMAND TEST_Vulkan)
