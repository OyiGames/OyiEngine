cmake_minimum_required(VERSION 3.31)

project(OyiEngine
    VERSION 0.1.0
    DESCRIPTION "Oyi game engine kernel: Entity-Component object model with local ECS optimization"
    LANGUAGES CXX
)

# The language standard, warnings and platform conditions are deliberately not set here.
# They live in CMakePresets.json and cmake/toolchains/*.cmake (ADR-0002).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)
include(OyiBuildSettings)
include(OyiModule)

option(OYI_BUILD_TOOLS "Build the oyi command-line tools" ON)

add_subdirectory(engine)

if(OYI_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# SDK export: the editor repository consumes the engine through find_package(oyi) (ADR-0015).
install(EXPORT oyi-targets
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/oyi"
    NAMESPACE oyi::
    FILE oyi-config.cmake
)
