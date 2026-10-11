# OyiEngine

A modern C++23 game engine kernel: an Entity-Component object model with local ECS optimization,
CLI-first, targeting Windows / macOS / iOS first (Android and Linux later).

中文版：[README.zh-CN.md](README.zh-CN.md)

- Contributing and conventions: [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)
- Third-party dependencies: [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)

## Build

Prerequisites: CMake 3.31+, Ninja, a vcpkg checkout with `VCPKG_ROOT` set, and on Windows a
Visual Studio developer environment (`vcvars64.bat`). All commands are stateless; never `cd` into the
build directory.

```bash
cmake --workflow --preset windows-msvc   # configure + Debug build + header self-containment check + tests
cmake --preset macos && cmake --build --preset macos-debug && ctest --preset macos-debug
```

Presets: `windows-msvc`, `windows-clang`, `macos`, `macos-asan`, `macos-tidy`, `ios`. Build output goes to
`out/build/<preset>/`, which also contains `compile_commands.json`. Local overrides (for example a
different `VCPKG_ROOT`) belong in `CMakeUserPresets.json`, which is git-ignored.

## Layout

```text
engine/oyi_<mod>/include/oyi_<mod>/*.h   public headers   #include <oyi_mod/X.h>
engine/oyi_<mod>/src/oyi_<mod>/*.cpp     implementation and private headers
tools/oyi/                               the single command-line entry point
tests/oyi_<mod>_tests/                   Catch2 tests
cmake/                                   OyiModule.cmake, OyiBuildSettings.cmake, toolchains/
scripts/                                 convention checks run by CI
docs/                                    CONTRIBUTING, THIRD_PARTY (English + zh-CN)
```

Module dependencies only point downwards: `platform <- core <- job <- ecs <- transform <- script / physics / net`.

## License

MIT
