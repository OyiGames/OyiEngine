project(OyiGraphic)

add_library(OyiVMA STATIC
    "src/Vulkan/VmaImpl.cpp"
)
target_include_directories(OyiVMA PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine/Packages/VulkanMemoryAllocator/include
)
target_link_libraries(OyiVMA PUBLIC Vulkan::Vulkan)

file(GLOB_RECURSE OYI_GRAPHIC_SRC CONFIGURE_DEPENDS
    "include/Graphic/RenderGraph/*.h"
    "include/Graphic/RHI/*.h"
    "src/RenderGraph/*.cpp"
)

if (OYI_GRAPHIC_VULKAN)
file(GLOB_RECURSE OYI_GRAPHIC_VK_SRC CONFIGURE_DEPENDS
    "src/Vulkan/*.h"
    "src/Vulkan/*.cpp"
)
list(APPEND OYI_GRAPHIC_SRC ${OYI_GRAPHIC_VK_SRC})
endif()

add_library(OyiGraphic SHARED ${OYI_GRAPHIC_SRC})
target_compile_definitions(OyiGraphic PRIVATE OYI_GRAPHIC_BUILD_DLL)
setup_output_directories(OyiGraphic)

target_include_directories(OyiGraphic PUBLIC include)

if (OYI_GRAPHIC_VULKAN)
target_link_libraries(OyiGraphic PRIVATE 
    Vulkan::Vulkan
    OyiVMA
)
endif()

target_link_libraries(OyiGraphic PRIVATE SDL3::SDL3)