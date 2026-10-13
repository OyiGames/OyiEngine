# oyi

English: [README.md](README.md)

唯一的命令行入口（ADR-0016）。每个子命令都接受 `--json`（stdout 上一个 JSON 对象，失败为
`{"error":{"code":..,"message":..}}`）与 `--quiet`。退出码：0 成功，1 命令执行了但失败，2 用法错误，3 内部错误。

| 命令 | 作用 |
|---|---|
| `oyi --version [--json]` | 引擎版本、协议版本、本构建的 schema 哈希（所有声明组件的引擎模块）。 |
| `oyi world inspect <file.oyiw> [--json]` | `World::Save` 存下的世界的头、组件表与 archetype；不需要注册表。 |
| `oyi asset inspect <file.oyib> [--json]` | 校验魔数、格式、大小与内容哈希后打印烘焙 blob 的头。 |
| `oyi serve --rpc [--port=N] [--once] [--json]` | 在回环地址上监听编辑器 / 远程 CLI / MCP 共用的通道（ADR-0015）：长度前缀的 FlatBuffers 帧，带协议版本、能力位与鉴权字段的握手，内置 `ping`、`version`、`echo`。`--once` 在第一个客户端离开后退出。 |
| `oyi schema validate <header.h> [--json]` | 对组件头跑 tools/codegen 的检查器。自动找到本仓库的生成器、Python、libclang 与配置时的编译参数；`OYI_CODEGEN_DIR`、`OYI_PYTHON`、`OYI_LIBCLANG`、`OYI_CODEGEN_ARGS_FILE` 可覆盖。 |

计划中（ADR-0016）：`world query | diff | snapshot | restore`、`sched graph --dot | validate`、
`replay`、`net`、`asset dump | diff | bake | check`、架在 `serve --rpc` 之上的 MCP 适配器。
