oyi_add_test(oyi_net_tests
    SOURCES
        PredictionTests.cpp
        ReplicationTests.cpp
        ThinClientTests.cpp
    DEPS oyi::net oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
