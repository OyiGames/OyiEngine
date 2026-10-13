# 第三方依赖

English: [THIRD_PARTY.md](THIRD_PARTY.md)

所有依赖通过 `vcpkg.json`（manifest 模式、固定 baseline）引入；同一 PR 内在此表加一行。Apache-2.0 组件的 NOTICE 文件随发布包分发。

| 名称 | 来源 | 许可 | 用途 | 状态 |
|---|---|---|---|---|
| Catch2 | vcpkg | BSL-1.0 | 单元测试 | 使用中 |
| Jolt Physics | vcpkg（5.6.0） | MIT | `oyi_physics` 里的刚体（`PhysicsWorld`、push / step / pull system）；Jolt 的 job 跑在引擎 JobSystem 上 | 使用中 |
| Tracy | vcpkg（0.14.1） | BSD-3-Clause | `OYI_PROFILE` 下的 profiler zone（`oyi_core/Profile.h`） | 使用中 |
| mimalloc | vcpkg（3.5.3） | MIT | `SystemAllocator` 的堆（`OYI_USE_MIMALLOC`，默认开） | 使用中 |
| GLM | vcpkg（1.0.3） | MIT | gameplay 与工具的标量数学（`oyi_transform/Glm.h`） | 使用中 |
| Google Highway | vcpkg | Apache-2.0 | 批量 SIMD | 计划 |
| FlatBuffers | vcpkg（25.12.19） | Apache-2.0 | 线上协议（`oyi_rpc`）、描述类资产 | 使用中 |
| Luau | vcpkg（0.741） | MIT | 脚本（`oyi_script`） | 使用中 |
| Dear ImGui / ImPlot | vcpkg | MIT | 工具 UI | 计划 |
| Google Benchmark | vcpkg | Apache-2.0 | 基准 | 计划 |
| Crashpad | vcpkg | Apache-2.0 | 崩溃上报 | 计划 |
| xxHash | vcpkg | BSD-2-Clause | 哈希 | 计划 |
| yojimbo / GameNetworkingSockets | 评估中 | BSD-3-Clause | 传输层 | 评估 |
