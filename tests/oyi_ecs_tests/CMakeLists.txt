add_subdirectory(fixture)

oyi_add_test(oyi_ecs_tests
    SOURCES
        ChunkLayoutTests.cpp
        ComponentMaskTests.cpp
        EntityRegistryTests.cpp
        WorldTests.cpp
    DEPS oyi::ecs oyi::ecs_fixture oyi::core
)
