oyi_add_module(codegen_sample NO_INSTALL
    PUBLIC_HEADERS
        include/oyi_codegen_sample/Components.h
    COMPONENT_HEADERS
        include/oyi_codegen_sample/Components.h
    SOURCES
        src/oyi_codegen_sample/Components.cpp
    DEPS oyi::core
)
