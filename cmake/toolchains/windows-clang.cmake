# clang-cl: shares sanitizer and warning behaviour with the macOS build (ADR-0002).
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# See windows-msvc.cmake for why these are toolchain-level flags.
set(CMAKE_CXX_FLAGS_INIT "/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor")
