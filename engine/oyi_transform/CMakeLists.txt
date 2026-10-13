oyi_add_module(transform
    PUBLIC_HEADERS
        include/oyi_transform/Affine.h
        include/oyi_transform/Components.h
        include/oyi_transform/PresentationSystems.h
        include/oyi_transform/TransformSystems.h
        include/oyi_transform/UsdConventions.h
    COMPONENT_HEADERS
        include/oyi_transform/Components.h
    SOURCES
        src/oyi_transform/Affine.cpp
        src/oyi_transform/PresentationSystems.cpp
        src/oyi_transform/TransformSystems.cpp
        src/oyi_transform/UsdConventions.cpp
    DEPS oyi::ecs oyi::core
)
