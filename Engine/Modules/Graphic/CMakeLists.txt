project(OyiGraphic)

file(GLOB_RECURSE src CONFIGURE_DEPENDS "src/*.cpp" "include/*.h" "include/*.hpp")

add_library(OyiGraphic SHARED ${src})
setup_output_directories(OyiGraphic)

target_include_directories(OyiGraphic PRIVATE include)