oyi_add_test(oyi_script_tests
    SOURCES
        BindingTests.cpp
        ScriptManagedTests.cpp
        ScriptVmTests.cpp
    DEPS oyi::script oyi::object oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
