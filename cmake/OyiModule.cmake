# oyi_add_module / oyi_add_executable / oyi_add_test
#
# Module CMakeLists files only list facts (ADR-0002). Layout conventions (ADR-0003):
#   engine/oyi_<name>/include/oyi_<name>/*.h   public headers, #include <oyi_<name>/X.h>
#   engine/oyi_<name>/src/oyi_<name>/*.cpp     implementation; private headers next to it,
#                                              #include <oyi_<name>/detail/X.h>
#   target oyi_<name>, alias oyi::<name>, namespace oyi::<name>

function(oyi_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_HEADERS;PRIVATE_HEADERS;DEPS;PRIVATE_DEPS" ${ARGN})
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

    install(TARGETS ${target} oyi_build_settings
        EXPORT oyi-targets
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
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
