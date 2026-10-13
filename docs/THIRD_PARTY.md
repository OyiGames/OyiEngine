# Third-party dependencies

中文版：[THIRD_PARTY.zh-CN.md](THIRD_PARTY.zh-CN.md)

All dependencies are added through `vcpkg.json` (manifest mode, pinned baseline). Add a row here in the
same pull request. NOTICE files of Apache-2.0 components ship with release packages.

| Name | Source | License | Purpose | Status |
|---|---|---|---|---|
| Catch2 | vcpkg | BSL-1.0 | Unit tests | In use |
| Jolt Physics | vcpkg (5.6.0) | MIT | Rigid bodies behind `oyi_physics` (`PhysicsWorld`, push / step / pull systems); Jolt's jobs run on the engine JobSystem | In use |
| Tracy | vcpkg (0.14.1) | BSD-3-Clause | Profiler zones behind `OYI_PROFILE` (`oyi_core/Profile.h`) | In use |
| mimalloc | vcpkg (3.5.3) | MIT | `SystemAllocator` heap (`OYI_USE_MIMALLOC`, default on) | In use |
| GLM | vcpkg (1.0.3) | MIT | Scalar math for gameplay and tools (`oyi_transform/Glm.h`) | In use |
| Google Highway | vcpkg | Apache-2.0 | Batch SIMD | Planned |
| FlatBuffers | vcpkg (25.12.19) | Apache-2.0 | Wire protocol (`oyi_rpc`), descriptive assets | In use |
| Luau | vcpkg (0.741) | MIT | Scripting (`oyi_script`) | In use |
| Dear ImGui / ImPlot | vcpkg | MIT | Tool UI | Planned |
| Google Benchmark | vcpkg | Apache-2.0 | Benchmarks | Planned |
| Crashpad | vcpkg | Apache-2.0 | Crash reporting | Planned |
| xxHash | vcpkg | BSD-2-Clause | Hashing | Planned |
| yojimbo / GameNetworkingSockets | under evaluation | BSD-3-Clause | Transport layer | Evaluating |
