# Modules in dependency order. Dependencies only point downwards (ADR-0003):
#   platform <- core <- job <- ecs <- transform <- script / physics / net
add_subdirectory(oyi_platform)
add_subdirectory(oyi_core)
add_subdirectory(oyi_job)
add_subdirectory(oyi_ecs)
add_subdirectory(oyi_transform)
add_subdirectory(oyi_script)
