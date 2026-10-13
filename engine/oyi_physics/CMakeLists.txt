find_package(Jolt CONFIG REQUIRED)

# Jolt is a private dependency: the public headers show none of it (ADR-0018).
oyi_add_module(physics
    PUBLIC_HEADERS
        include/oyi_physics/Components.h
        include/oyi_physics/PhysicsSystems.h
        include/oyi_physics/PhysicsWorld.h
    COMPONENT_HEADERS
        include/oyi_physics/Components.h
    PRIVATE_HEADERS
        src/oyi_physics/detail/Jolt.h
        src/oyi_physics/detail/JoltJobSystem.h
    SOURCES
        src/oyi_physics/JoltJobSystem.cpp
        src/oyi_physics/PhysicsSystems.cpp
        src/oyi_physics/PhysicsWorld.cpp
    DEPS oyi::transform oyi::ecs oyi::job oyi::core
    PRIVATE_DEPS Jolt::Jolt
)
