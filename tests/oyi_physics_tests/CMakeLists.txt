oyi_add_test(oyi_physics_tests
    SOURCES
        PhysicsTests.cpp
    DEPS oyi::physics oyi::transform oyi::ecs oyi::job oyi::core
)
