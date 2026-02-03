project(OyiGraphic)

file(GLOB_RECURSE OYI_GRAPHIC_SRC CONFIGURE_DEPENDS
    "include/Graphic/RenderGraph/*.h"
    "src/RenderGraph/*.cpp"
)

add_library(OyiGraphic SHARED ${OYI_GRAPHIC_SRC})
target_compile_definitions(OyiGraphic PRIVATE OYI_GRAPHIC_BUILD_DLL)
setup_output_directories(OyiGraphic)

target_include_directories(OyiGraphic PUBLIC include)

target_link_libraries(OyiGraphic PRIVATE SDL3::SDL3)
target_link_libraries(OyiGraphic PRIVATE fmt::fmt)
target_link_libraries(OyiGraphic PRIVATE NRI_Shared)