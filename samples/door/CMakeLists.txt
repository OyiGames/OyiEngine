# The door: one object end to end (ADR-0024). The components are a tiny codegen module of their
# own; the program is everything else.
oyi_add_module(door_sample NO_INSTALL
    PUBLIC_HEADERS
        include/door/Components.h
    COMPONENT_HEADERS
        include/door/Components.h
    SOURCES
        src/door/Components.cpp
    DEPS oyi::ecs oyi::core
)

oyi_add_executable(door NO_INSTALL
    SOURCES
        src/door/Main.cpp
    DEPS oyi::door_sample oyi::object oyi::ecs oyi::core
)

if(BUILD_TESTING)
    # As a test it must die on a failed check instead of showing the CRT dialog box.
    target_sources(door PRIVATE "${PROJECT_SOURCE_DIR}/tests/support/CrashDialogs.cpp")
    add_test(NAME door_sample COMMAND door)
    set_tests_properties(door_sample PROPERTIES PASS_REGULAR_EXPRESSION "brain: despawning after 1 opening")
endif()
