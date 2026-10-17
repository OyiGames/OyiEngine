add_subdirectory(fixture)

oyi_add_test(oyi_ecs_tests
    SOURCES
        AccessCheckTests.cpp
        BuilderTests.cpp
        ChunkLayoutTests.cpp
        ChunkParallelTests.cpp
        CommandBufferTests.cpp
        DownclockTests.cpp
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
        WorldKindTests.cpp
        WorldTests.cpp
    DEPS oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
target_compile_definitions(oyi_ecs_tests PRIVATE "OYI_TEST_OUTPUT_DIR=\"${OYI_TEST_OUTPUT_DIR}\"")
