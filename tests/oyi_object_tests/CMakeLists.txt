oyi_add_test(oyi_object_tests
    SOURCES
        ObjectTests.cpp
        PrototypeBlobTests.cpp
    DEPS oyi::object oyi::asset oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
