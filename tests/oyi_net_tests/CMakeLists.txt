oyi_add_test(oyi_net_tests
    SOURCES
        ReplicationTests.cpp
    DEPS oyi::net oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
