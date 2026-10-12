# Project-wide compile settings, attached to every module as INTERFACE targets.
# Together with cmake/toolchains/*.cmake this is the only CMake file allowed to contain
# compiler conditions.
#
#   oyi::build_settings  - engine modules: warnings as errors, no RTTI
#   oyi::test_settings   - test and tool executables: same warnings, RTTI kept (Catch2 needs it)
#
# Flags the compiler itself needs to parse our sources (/utf-8, /Zc:__cplusplus, ...) are NOT here:
# CMake's verify_interface_header_sets targets do not inherit INTERFACE usage requirements, so those
# flags live in the toolchain files as CMAKE_CXX_FLAGS_INIT.

add_library(oyi_build_settings INTERFACE)
add_library(oyi::build_settings ALIAS oyi_build_settings)

add_library(oyi_test_settings INTERFACE)
add_library(oyi::test_settings ALIAS oyi_test_settings)

set(_oyi_msvc_warnings
    /W4 /WX
    /wd4324 # structure was padded due to alignment specifier: alignment is intentional
)
set(_oyi_clang_gnu_warnings
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
)

foreach(_t IN ITEMS oyi_build_settings oyi_test_settings)
    target_compile_options(${_t} INTERFACE
        "$<$<CXX_COMPILER_ID:MSVC>:${_oyi_msvc_warnings}>"
        "$<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:$<$<CXX_COMPILER_ID:Clang>:${_oyi_msvc_warnings}>>"
        "$<$<CXX_COMPILER_FRONTEND_VARIANT:GNU>:${_oyi_clang_gnu_warnings}>"
    )
    target_compile_definitions(${_t} INTERFACE
        "$<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN>"
    )
endforeach()

# Exported once for every module that links it privately (their link interface carries
# $<LINK_ONLY:oyi::build_settings>), so the SDK export set stays well formed.
install(TARGETS oyi_build_settings EXPORT oyi-targets)

# Engine core builds without RTTI. The no-exceptions policy is enforced by review and clang-tidy
# until the _HAS_EXCEPTIONS/Catch2 interaction has been validated.
target_compile_options(oyi_build_settings INTERFACE
    "$<$<CXX_COMPILER_ID:MSVC>:/GR->"
    "$<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:$<$<CXX_COMPILER_ID:Clang>:/GR->>"
    "$<$<CXX_COMPILER_FRONTEND_VARIANT:GNU>:-fno-rtti>"
)
