# CopyFilesToBuild.cmake
# Function: Copy specified files to the root of the build directory (${CMAKE_BINARY_DIR})
# Usage:
#   include("${CMAKE_SOURCE_DIR}/cmake/CopyFilesToBuild.cmake")
#   copy_specified_files_to_build(
#       FILES
#           "${CMAKE_SOURCE_DIR}/thirdparty/lib/foo.dll"
#           "${CMAKE_SOURCE_DIR}/thirdparty/lib/bar.dll"
#           "${CMAKE_SOURCE_DIR}/thirdparty/bin/baz.so"
#   )

function(copy_specified_files_to_build)
    cmake_parse_arguments(
        COPY_FILES
        ""          # options
        ""          # oneValueArgs
        "FILES"     # multiValueArgs
        ${ARGN}
    )

    if(NOT COPY_FILES_FILES)
        message(FATAL_ERROR "[copy_specified_files_to_build] No files specified. Use the FILES argument to list files.")
    endif()

    foreach(src_file IN LISTS COPY_FILES_FILES)
        get_filename_component(abs_file "${src_file}" ABSOLUTE)
        if(NOT EXISTS "${abs_file}")
            message(FATAL_ERROR "[copy_specified_files_to_build] File not found: ${abs_file}")
        endif()

        file(COPY "${abs_file}" DESTINATION "${CMAKE_BINARY_DIR}")
        message(STATUS "Copied file: ${abs_file} -> ${CMAKE_BINARY_DIR}")
    endforeach()
endfunction()