oyi_add_module(ecs_fixture NO_INSTALL
    PUBLIC_HEADERS
        include/oyi_ecs_fixture/Components.h
    COMPONENT_HEADERS
        include/oyi_ecs_fixture/Components.h
    SOURCES
        src/oyi_ecs_fixture/Components.cpp
    DEPS oyi::core oyi::ecs
)
