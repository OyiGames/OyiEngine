# The object layer (ADR-0024): prototypes, the factory and the lifecycle hook above the kernel.
oyi_add_module(object
    PUBLIC_HEADERS
        include/oyi_object/Components.h
        include/oyi_object/ObjectFactory.h
        include/oyi_object/ObjectLayer.h
        include/oyi_object/Prototype.h
        include/oyi_object/PrototypeBlob.h
    COMPONENT_HEADERS
        include/oyi_object/Components.h
    SOURCES
        src/oyi_object/ObjectFactory.cpp
        src/oyi_object/ObjectLayer.cpp
        src/oyi_object/Prototype.cpp
        src/oyi_object/PrototypeBlob.cpp
    DEPS oyi::ecs oyi::asset oyi::core
)
