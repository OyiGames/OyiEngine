# oyi_add_module / oyi_add_executable / oyi_add_test
#
# Module CMakeLists files only list facts (ADR-0002). Layout conventions (ADR-0003):
#   engine/oyi_<name>/include/oyi_<name>/*.h   public headers, #include <oyi_<name>/X.h>
#   engine/oyi_<name>/src/oyi_<name>/*.cpp     implementation; private headers next to it,
#                                              #include <oyi_<name>/detail/X.h>
#   target oyi_<name>, alias oyi::<name>, namespace oyi::<name>
#
# COMPONENT_HEADERS lists public headers (same paths as in PUBLIC_HEADERS) that declare
# OYI_COMPONENT / OYI_STRUCT types; tools/codegen turns each into <oyi_<name>/X.gen.h> (ADR-0011).
# NO_INSTALL keeps a module out of the SDK export (test fixtures).

function(oyi_add_module name)
    cmake_parse_arguments(ARG "NO_INSTALL" "" "SOURCES;PUBLIC_HEADERS;PRIVATE_HEADERS;COMPONENT_HEADERS;DEPS;PRIVATE_DEPS" ${ARGN})
    set(target "oyi_${name}")

    add_library(${target})
    add_library("oyi::${name}" ALIAS ${target})

    target_sources(${target} PRIVATE ${ARG_SOURCES})
    target_sources(${target} PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${ARG_PUBLIC_HEADERS}
    )
    if(ARG_PRIVATE_HEADERS)
        target_sources(${target} PRIVATE
            FILE_SET private_headers TYPE HEADERS
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
            FILES ${ARG_PRIVATE_HEADERS}
        )
    endif()

    target_link_libraries(${target}
        PUBLIC ${ARG_DEPS}
        PRIVATE ${ARG_PRIVATE_DEPS} oyi::build_settings
    )

    set_target_properties(${target} PROPERTIES
        VERIFY_INTERFACE_HEADER_SETS ON
        EXPORT_NAME "${name}"
    )

    if(ARG_COMPONENT_HEADERS)
        set(include_root "${CMAKE_CURRENT_SOURCE_DIR}/include")
        set(relative_headers)
        foreach(header IN LISTS ARG_COMPONENT_HEADERS)
            cmake_path(ABSOLUTE_PATH header BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE absolute)
            cmake_path(RELATIVE_PATH absolute BASE_DIRECTORY "${include_root}" OUTPUT_VARIABLE relative)
            list(APPEND relative_headers "${relative}")
        endforeach()
        oyi_target_codegen(${target} INCLUDE_ROOT "${include_root}" HEADERS ${relative_headers})
    endif()

    if(NOT ARG_NO_INSTALL)
        install(TARGETS ${target} oyi_build_settings
            EXPORT oyi-targets
            FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        )
    endif()
endfunction()

function(oyi_add_executable name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name})
    target_sources(${name} PRIVATE ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} oyi::test_settings)
    install(TARGETS ${name} EXPORT oyi-targets)
endfunction()

function(oyi_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name})
    target_sources(${name} PRIVATE ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} Catch2::Catch2WithMain oyi::test_settings)
    catch_discover_tests(${name})
endfunction()
