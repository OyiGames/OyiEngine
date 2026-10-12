add_subdirectory(fixture)

oyi_add_test(oyi_ecs_tests
    SOURCES
        ChunkLayoutTests.cpp
        CommandBufferTests.cpp
        ComponentMaskTests.cpp
        EntityRegistryTests.cpp
        FirewallTests.cpp
        MainThreadSystemTests.cpp
        ManagedTests.cpp
        MessageTests.cpp
        QueryTests.cpp
        ResourceTests.cpp
        ScheduleTests.cpp
        SnapshotTests.cpp
        WorldTests.cpp
    DEPS oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
