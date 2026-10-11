# Chain-loaded by the vcpkg toolchain (VCPKG_CHAINLOAD_TOOLCHAIN_FILE in CMakePresets.json).
# The language standard and ABI-sensitive settings live only here (ADR-0002).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources are UTF-8, __cplusplus reports the real value, strict standard conformance.
# These are toolchain-level flags rather than target options because CMake's
# verify_interface_header_sets targets only inherit toolchain-level flags.
set(CMAKE_CXX_FLAGS_INIT "/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor")
