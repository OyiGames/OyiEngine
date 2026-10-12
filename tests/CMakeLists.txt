find_package(Catch2 CONFIG REQUIRED)
include(Catch)

add_subdirectory(oyi_core_tests)
add_subdirectory(oyi_job_tests)
add_subdirectory(oyi_ecs_tests)
add_subdirectory(oyi_transform_tests)
add_subdirectory(oyi_script_tests)
add_subdirectory(oyi_codegen_tests)
