# Modules in dependency order. Dependencies only point downwards (ADR-0003):
#   platform <- core <- job <- ecs <- transform <- script / physics / net
#   asset sits beside job: it needs core only.
add_subdirectory(oyi_platform)
add_subdirectory(oyi_core)
add_subdirectory(oyi_asset)
add_subdirectory(oyi_job)
add_subdirectory(oyi_ecs)
add_subdirectory(oyi_transform)
add_subdirectory(oyi_script)
