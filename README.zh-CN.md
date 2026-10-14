# OyiEngine

现代 C++23 游戏引擎内核：Entity-Component 对象模型 + 局部 ECS 优化，CLI-first，优先面向 Windows / macOS / iOS（Android、Linux 后置）。

English: [README.md](README.md)

- 贡献指南与规范：[docs/CONTRIBUTING.zh-CN.md](docs/CONTRIBUTING.zh-CN.md)
- 第三方依赖：[docs/THIRD_PARTY.zh-CN.md](docs/THIRD_PARTY.zh-CN.md)

## 构建

前置：CMake 3.31+、Ninja、一份 vcpkg 克隆并设置 `VCPKG_ROOT`、Python 3.11+ 并执行 `python -m pip install -r tools/codegen/requirements.txt`、主机工具链自带的 libclang（Visual Studio 的"C++ Clang 工具"组件、Xcode 或 Homebrew LLVM，见 [tools/codegen/README.zh-CN.md](tools/codegen/README.zh-CN.md)）；Windows 上需要在 Visual Studio 开发环境（`vcvars64.bat`）里运行。所有命令无状态，不要 `cd` 进构建目录。

克隆时加 `--recurse-submodules`（或之后执行 `git submodule update --init`）：线上协议 schema 在 `protocol/` 子模块里。vcpkg 的克隆放在仓库之外；仓库自己的 `vcpkg/` 目录放的是 overlay 端口，不是 vcpkg 本身。

```bash
cmake --workflow --preset windows-msvc   # configure + Debug 构建 + 头文件自包含检查 + 测试
cmake --preset macos && cmake --build --preset macos-debug && ctest --preset macos-debug
```

预设：`windows-msvc`、`windows-clang`、`macos`、`macos-asan`、`macos-tidy`、`ios`。产物在 `out/build/<preset>/`，其中含 `compile_commands.json`。本机覆盖项（例如另一个 `VCPKG_ROOT`）写在 git 忽略的 `CMakeUserPresets.json` 里。

## 目录

```text
engine/oyi_<mod>/include/oyi_<mod>/*.h   公共头   #include <oyi_mod/X.h>
engine/oyi_<mod>/src/oyi_<mod>/*.cpp     实现与私有头
tools/oyi/                               单一命令行入口
tools/codegen/                           组件反射生成器（Python + libclang）
tests/oyi_<mod>_tests/                   Catch2 测试
cmake/                                   OyiModule.cmake、OyiBuildSettings.cmake、toolchains/
scripts/                                 CI 跑的规范检查
docs/                                    CONTRIBUTING、THIRD_PARTY（英文 + zh-CN）
```

模块依赖只许向下：`platform <- core <- job <- ecs <- transform <- script / physics / net`。

## 许可

MIT
