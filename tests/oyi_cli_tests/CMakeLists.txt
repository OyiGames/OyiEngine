# The command-line tool, driven end to end (ADR-0016). The world and asset files it inspects are
# written by oyi_ecs_tests and oyi_asset_tests into this directory's parent (OYI_TEST_OUTPUT_DIR),
# so those suites run first.
add_test(NAME oyi_cli
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_cli.py"
        --oyi "$<TARGET_FILE:oyi>"
        --cases "${PROJECT_SOURCE_DIR}/tests/oyi_codegen_tests/cases"
        --work "${OYI_TEST_OUTPUT_DIR}/cli"
)
set_tests_properties(oyi_cli PROPERTIES DEPENDS
    "a saved world describes itself without a registry;a blob is read back through relative pointers where it was loaded")
