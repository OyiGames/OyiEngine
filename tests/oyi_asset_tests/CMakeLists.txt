oyi_add_test(oyi_asset_tests
    SOURCES
        BakeTests.cpp
        BlobTests.cpp
    DEPS oyi::asset oyi::core
)
target_compile_definitions(oyi_asset_tests PRIVATE "OYI_TEST_OUTPUT_DIR=\"${OYI_TEST_OUTPUT_DIR}\"")
