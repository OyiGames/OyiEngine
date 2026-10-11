# Build-time reflection generation (ADR-0011).
#
# Headers listed in oyi_add_module(... COMPONENT_HEADERS ...) are parsed with libclang by
# tools/codegen and turned into <oyi_mod/X.gen.h> plus X.schema.json under
# ${CMAKE_CURRENT_BINARY_DIR}/generated/include, which joins the module's public header set.
# `cmake --build --target codegen` runs only the generators (CMake 3.31 CODEGEN).
#
# Requirements, checked at configure time:
#   - Python 3.11+ with the `clang` bindings:  python -m pip install -r tools/codegen/requirements.txt
#   - a libclang shared library at least as new as the host standard library expects (MSVC's STL
#     refuses older clangs). Found automatically in the Visual Studio LLVM component, Xcode or
#     Homebrew LLVM; override with -DOYI_LIBCLANG=<path> or the OYI_LIBCLANG environment variable.

# Prefer the interpreter first on PATH (the one `pip install` targets) over the newest one that the
# Windows registry knows about.
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_REGISTRY LAST)
set(Python3_FIND_FRAMEWORK LAST)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(OYI_CODEGEN_DIR "${PROJECT_SOURCE_DIR}/tools/codegen")
file(GLOB OYI_CODEGEN_SOURCES CONFIGURE_DEPENDS "${OYI_CODEGEN_DIR}/oyi_codegen/*.py")

# --- libclang ---------------------------------------------------------------------------------
if(DEFINED ENV{OYI_LIBCLANG} AND NOT OYI_LIBCLANG)
    set(OYI_LIBCLANG "$ENV{OYI_LIBCLANG}" CACHE FILEPATH "libclang shared library used by tools/codegen")
endif()
if(NOT OYI_LIBCLANG)
    set(_oyi_libclang_hints)
    if(WIN32)
        if(DEFINED ENV{VCINSTALLDIR})
            list(APPEND _oyi_libclang_hints "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin")
        endif()
        list(APPEND _oyi_libclang_hints "$ENV{ProgramFiles}/LLVM/bin")
        set(_oyi_libclang_names libclang.dll)
    elseif(APPLE)
        execute_process(
            COMMAND xcrun --find clang
            OUTPUT_VARIABLE _oyi_xcode_clang
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_oyi_xcode_clang)
            cmake_path(GET _oyi_xcode_clang PARENT_PATH _oyi_xcode_bin)
            list(APPEND _oyi_libclang_hints "${_oyi_xcode_bin}/../lib")
        endif()
        list(APPEND _oyi_libclang_hints /opt/homebrew/opt/llvm/lib /usr/local/opt/llvm/lib)
        set(_oyi_libclang_names libclang.dylib)
    else()
        file(GLOB _oyi_llvm_lib_dirs /usr/lib/llvm-*/lib)
        list(APPEND _oyi_libclang_hints ${_oyi_llvm_lib_dirs} /usr/lib /usr/local/lib)
        set(_oyi_libclang_names libclang.so libclang-20.so libclang-19.so libclang.so.1)
    endif()
    find_file(OYI_LIBCLANG
        NAMES ${_oyi_libclang_names}
        HINTS ${_oyi_libclang_hints}
        DOC "libclang shared library used by tools/codegen"
        NO_DEFAULT_PATH
    )
endif()
if(NOT OYI_LIBCLANG)
    message(FATAL_ERROR
        "oyi_codegen: libclang not found. Install the LLVM component of Visual Studio, Xcode or "
        "Homebrew llvm, or pass -DOYI_LIBCLANG=<path to libclang shared library>.")
endif()
message(STATUS "oyi_codegen: libclang = ${OYI_LIBCLANG}")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import clang.cindex"
    RESULT_VARIABLE _oyi_bindings_rc
    OUTPUT_QUIET
    ERROR_QUIET
)
if(NOT _oyi_bindings_rc EQUAL 0)
    message(FATAL_ERROR
        "oyi_codegen: python bindings for libclang are missing. Run:\n"
        "  \"${Python3_EXECUTABLE}\" -m pip install -r \"${OYI_CODEGEN_DIR}/requirements.txt\"")
endif()

# --- clang arguments that mirror the real compiler ----------------------------------------------
set(OYI_CODEGEN_CLANG_ARGS)
if(MSVC)
    math(EXPR _oyi_msvc_major "${MSVC_VERSION} / 100")
    math(EXPR _oyi_msvc_minor "${MSVC_VERSION} % 100")
    list(APPEND OYI_CODEGEN_CLANG_ARGS
        "--target=x86_64-pc-windows-msvc"
        "-fms-compatibility-version=${_oyi_msvc_major}.${_oyi_msvc_minor}"
    )
    # The standard library location comes from the vcvars environment seen at configure time.
    # Passing it explicitly keeps generation working from IDEs that do not inherit that environment.
    set(_oyi_msvc_include "$ENV{INCLUDE}")
    foreach(_dir IN LISTS _oyi_msvc_include)
        list(APPEND OYI_CODEGEN_CLANG_ARGS "-isystem" "${_dir}")
    endforeach()
endif()
if(APPLE AND CMAKE_OSX_SYSROOT)
    list(APPEND OYI_CODEGEN_CLANG_ARGS "-isysroot" "${CMAKE_OSX_SYSROOT}")
endif()

# oyi_target_codegen(<target> INCLUDE_ROOT <dir> HEADERS <include-relative header>...)
# Called by oyi_add_module; HEADERS are paths such as oyi_transform/Components.h.
function(oyi_target_codegen target)
    cmake_parse_arguments(ARG "" "INCLUDE_ROOT" "HEADERS" ${ARGN})
    set(gen_root "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
    set(outputs)
    foreach(header IN LISTS ARG_HEADERS)
        cmake_path(REMOVE_EXTENSION header LAST_ONLY OUTPUT_VARIABLE stem)
        set(gen_h "${gen_root}/${stem}.gen.h")
        set(gen_json "${gen_root}/${stem}.schema.json")
        set(depfile "${CMAKE_CURRENT_BINARY_DIR}/generated/${stem}.d")
        add_custom_command(
            OUTPUT "${gen_h}" "${gen_json}"
            COMMAND "${Python3_EXECUTABLE}" -m oyi_codegen generate "${header}"
                --include-root "${ARG_INCLUDE_ROOT}"
                --out-dir "${gen_root}"
                --depfile "${depfile}"
                --libclang "${OYI_LIBCLANG}"
                -- ${OYI_CODEGEN_CLANG_ARGS}
                "-I$<JOIN:$<REMOVE_DUPLICATES:$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>>,;-I>"
                "$<$<BOOL:$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>,;-D>>"
            WORKING_DIRECTORY "${OYI_CODEGEN_DIR}"
            DEPFILE "${depfile}"
            DEPENDS "${ARG_INCLUDE_ROOT}/${header}" ${OYI_CODEGEN_SOURCES}
            COMMAND_EXPAND_LISTS
            CODEGEN
            COMMENT "oyi_codegen: ${header}"
            VERBATIM
        )
        list(APPEND outputs "${gen_h}")
    endforeach()
    target_sources(${target} PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${gen_root}"
        FILES ${outputs}
    )
endfunction()
