# Modules in dependency order. Dependencies only point downwards (ADR-0003):
#   platform <- core <- job <- ecs <- transform <- script / physics / net (net needs ecs only)
#   object sits beside transform: the object layer of ADR-0024 needs ecs only.
#   asset sits beside job: it needs core only; rpc needs core and platform.
add_subdirectory(oyi_platform)
add_subdirectory(oyi_core)
add_subdirectory(oyi_asset)
add_subdirectory(oyi_rpc)
add_subdirectory(oyi_job)
add_subdirectory(oyi_ecs)
add_subdirectory(oyi_transform)
add_subdirectory(oyi_object)
add_subdirectory(oyi_physics)
add_subdirectory(oyi_net)
add_subdirectory(oyi_script)
