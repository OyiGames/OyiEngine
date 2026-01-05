# Always build googlemock (it will include googletest)
add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Packages/googletest EXCLUDE_FROM_ALL)

# Create interface target for the engine to use
add_library(gtest_lib INTERFACE)
target_link_libraries(gtest_lib INTERFACE
    gtest
    gtest_main
    gmock
    gmock_main
)

target_include_directories(gtest_lib INTERFACE
    ${CMAKE_SOURCE_DIR}/Engine/Packages/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/Engine/Packages/googletest/googlemock/include
)

# Map Debug build to Release
foreach(target gtest gtest_main gmock gmock_main)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES
            MAP_IMPORTED_CONFIG_DEBUG RELEASE
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
        )
    endif()
endforeach()