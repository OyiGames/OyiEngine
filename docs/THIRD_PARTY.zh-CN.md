# 第三方依赖

English: [THIRD_PARTY.md](THIRD_PARTY.md)

所有依赖通过 `vcpkg.json`（manifest 模式、固定 baseline）引入；同一 PR 内在此表加一行。Apache-2.0 组件的 NOTICE 文件随发布包分发。

| 名称 | 来源 | 许可 | 用途 | 状态 |
|---|---|---|---|---|
| Catch2 | vcpkg | BSL-1.0 | 单元测试 | 使用中 |
| Jolt Physics | vcpkg | MIT | 物理 | 计划 |
| Tracy | vcpkg | BSD-3-Clause | Profiler | 计划 |
| mimalloc | vcpkg | MIT | 全局分配器 | 计划 |
| GLM | vcpkg | MIT | 标量数学 | 计划 |
| Google Highway | vcpkg | Apache-2.0 | 批量 SIMD | 计划 |
| FlatBuffers | vcpkg | Apache-2.0 | 序列化 / 协议 | 计划 |
| Luau | vcpkg（0.741） | MIT | 脚本（`oyi_script`） | 使用中 |
| Dear ImGui / ImPlot | vcpkg | MIT | 工具 UI | 计划 |
| Google Benchmark | vcpkg | Apache-2.0 | 基准 | 计划 |
| Crashpad | vcpkg | Apache-2.0 | 崩溃上报 | 计划 |
| xxHash | vcpkg | BSD-2-Clause | 哈希 | 计划 |
| yojimbo / GameNetworkingSockets | 评估中 | BSD-3-Clause | 传输层 | 评估 |
