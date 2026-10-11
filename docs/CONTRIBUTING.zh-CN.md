# 贡献指南

English: [CONTRIBUTING.md](CONTRIBUTING.md)

本文是给人读的摘要。被强制执行的真相是 `.clang-format`、`.clang-tidy`、`scripts/check_sources.py` 和 CI 工作流；两者不一致时以工具为准并修正本文。

## 语言政策

| 位置 | 语言 |
|---|---|
| 标识符、注释、日志与断言文案、提交信息、CMake、脚本、CI | 只用英文（源码文件必须是 ASCII；CI 检查） |
| 仓库文档（`*.md`） | 中英两份：`<name>.md`（英文，为准）与 `<name>.zh-CN.md`。同一 PR 内一起更新；CI 检查成对存在 |
| Issue 与 PR | 英文优先；讨论可用中文 |
| 面向用户的字符串 | 不写死；本地化表落地后统一走表 |

## 仓库里不放本机信息

不出现绝对路径、盘符、机器名、用户名、本机工具位置、AI 助手配置。CI 拒绝被跟踪文本文件里的盘符路径。本机覆盖项写进 git 忽略的文件：`CMakeUserPresets.json`、`.env.local`。

## 代码

- C++23。引擎核心无 RTTI、不抛异常；错误用 `std::expected<T, Error>`；有返回值的函数一律 `[[nodiscard]]`。
- 命名：类型与函数 `PascalCase`；参数与局部变量 `camelCase`；数据成员 `mName`；静态 `sName`；常量 `kName`；宏 `OYI_UPPER_SNAKE`；命名空间 `oyi::module`。
- 文件：一文件一主类型，`PascalCase.h` / `PascalCase.cpp`；目录小写。
- include：一律尖括号 `#include <oyi_mod/File.h>`；`.cpp` 第一行 include 自己的头；顶层 include 目录带 `oyi_` 前缀。
- 组件（POD）用 `<oyi_core/Annotations.h>` 的 `OYI_COMPONENT` / `OYI_STRUCT` 声明，并在 CMake 的 `COMPONENT_HEADERS` 里列出：无虚函数、无指针、定宽整数、显式 padding、字段全部值初始化。`tools/codegen` 在构建期强制这些规则并生成反射表。
- job 内：禁 `std::function`、直接 TLS、阻塞 I/O、全局 RNG、真实时间。
- 文档注释用 `///`，一句话说清做什么、何时失败。
- `TODO` 必须带 issue 号：`// TODO(#123): ...`。

## 测试

- 新代码附测试；AI 生成的代码必须附测试。
- Catch2 用例名是英文句子，标签 `[module][feature]`。
- 偶发失败的测试当天隔离。

## Git

- trunk-based：`main` 受保护、永远绿；短命分支 `feat/`、`fix/`、`chore/`、`docs/`、`ci/`、`build/`，kebab-case；PR squash 合并。
- 提交信息用 Conventional Commits（`feat(ecs): ...`、`fix(job): ...`），祈使语气，正文 72 列换行。与 AI 助手共同完成的提交带 `Co-Authored-By` 尾注。
- 一个 PR 一个主题；填写模板。
- 不提交生成物与构建产物（`out/`、`generated/`）。
- 依赖只通过 `vcpkg.json` 引入，并登记到 `docs/THIRD_PARTY.md`。

## 工具

| 检查 | 工具 |
|---|---|
| 格式 | `clang-format` 19，`--dry-run -Werror` |
| 静态分析 | `clang-tidy`（命名、bugprone、performance、modernize、readability） |
| Python 脚本 | `ruff check` 与 `ruff format --check` |
| 规范 | `python scripts/check_sources.py` |
| 组件规则 | 构建期由 `tools/codegen` 检查；golden 测试在 `tests/oyi_codegen_tests` |
| 头文件自包含 | `cmake --build --preset <preset>-verify-headers` |
| 内存与 UB | 每次 PR 跑 ASan + UBSan 预设 |
