oyi_add_test(oyi_script_tests
    SOURCES
        BindingTests.cpp
        ScriptVmTests.cpp
    DEPS oyi::script oyi::ecs oyi::ecs_fixture oyi::job oyi::core
)
