add_subdirectory(fixture)

oyi_add_test(oyi_ecs_tests
    SOURCES
        AccessCheckTests.cpp
        ChunkLayoutTests.cpp
        ChunkParallelTests.cpp
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
        SerializationTests.cpp
        SnapshotTests.cpp
        WorldTests.cpp
    DEPS oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
