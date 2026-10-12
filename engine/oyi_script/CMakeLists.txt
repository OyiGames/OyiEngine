find_package(unofficial-luau CONFIG REQUIRED)

oyi_add_module(script
    PUBLIC_HEADERS
        include/oyi_script/ScriptVm.h
    PRIVATE_HEADERS
        src/oyi_script/detail/Bindings.h
    SOURCES
        src/oyi_script/Bindings.cpp
        src/oyi_script/ScriptVm.cpp
    DEPS oyi::ecs oyi::core
    PRIVATE_DEPS unofficial::luau::Luau.VM unofficial::luau::Luau.Compiler
)

# The oyi library's type definitions for luau-analyze and editors (ADR-0012).
install(FILES luau/oyi.d.luau DESTINATION "${CMAKE_INSTALL_DATADIR}/oyi/luau")
