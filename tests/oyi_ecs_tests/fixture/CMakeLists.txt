oyi_add_module(ecs_fixture NO_INSTALL
    PUBLIC_HEADERS
        include/oyi_ecs_fixture/Components.h
    COMPONENT_HEADERS
        include/oyi_ecs_fixture/Components.h
    SOURCES
        src/oyi_ecs_fixture/Components.cpp
    DEPS oyi::core oyi::ecs
)
# Where the generated files of the fixture live, for tests that read them (the .d.luau check).
target_compile_definitions(oyi_ecs_fixture INTERFACE
    "OYI_ECS_FIXTURE_GENERATED_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/generated/include\""
)
