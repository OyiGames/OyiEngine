oyi_add_module(transform
    PUBLIC_HEADERS
        include/oyi_transform/Affine.h
        include/oyi_transform/Components.h
        include/oyi_transform/TransformSystems.h
    COMPONENT_HEADERS
        include/oyi_transform/Components.h
    SOURCES
        src/oyi_transform/Affine.cpp
        src/oyi_transform/TransformSystems.cpp
    DEPS oyi::ecs oyi::core
)
