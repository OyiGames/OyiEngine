oyi_add_module(ecs
    PUBLIC_HEADERS
        include/oyi_ecs/Archetype.h
        include/oyi_ecs/ChunkLayout.h
        include/oyi_ecs/ChunkView.h
        include/oyi_ecs/ComponentMask.h
        include/oyi_ecs/ComponentRegistry.h
        include/oyi_ecs/Entity.h
        include/oyi_ecs/EntityRegistry.h
        include/oyi_ecs/World.h
    SOURCES
        src/oyi_ecs/Archetype.cpp
        src/oyi_ecs/ChunkLayout.cpp
        src/oyi_ecs/ComponentRegistry.cpp
        src/oyi_ecs/EntityRegistry.cpp
        src/oyi_ecs/World.cpp
    DEPS oyi::core oyi::job
)
