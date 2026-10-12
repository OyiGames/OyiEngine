find_package(Catch2 CONFIG REQUIRED)
include(Catch)

# Files some suites write for others (the CLI inspects what the engine tests saved).
set(OYI_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test-output")
file(MAKE_DIRECTORY "${OYI_TEST_OUTPUT_DIR}")

add_subdirectory(oyi_platform_tests)
add_subdirectory(oyi_core_tests)
add_subdirectory(oyi_asset_tests)
add_subdirectory(oyi_job_tests)
add_subdirectory(oyi_ecs_tests)
add_subdirectory(oyi_transform_tests)
add_subdirectory(oyi_script_tests)
add_subdirectory(oyi_codegen_tests)
add_subdirectory(oyi_cli_tests)
