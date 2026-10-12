# oyi_codegen

English: [README.md](README.md)

组件声明的构建期反射生成器（ADR-0011）。用 libclang 解析 C++ 头文件，按 POD 规则校验每个
`OYI_COMPONENT` / `OYI_STRUCT`，然后生成：

- `<oyi_mod/X.gen.h>`：`constexpr oyi::core::ComponentInfo` 表，附带把大小、对齐和每个字段偏移钉死的
  `static_assert`，布局改了却没重新生成时直接编译失败。每个记录还生成一个值初始化的 `kXDefault` 实例
  （`ComponentInfo::mDefault`），存储层把它拷进新组件；每个组件生成 `oyi::core::ComponentTraits<X>` 特化，
  类型化 API 通过它的 `kInfo` 找到表；
- `<oyi_mod/X.schema.json>`：同样的数据，给工具和编辑器用。

`oyi_add_module` 的 `COMPONENT_HEADERS` 里列出的头文件由 CMake 自动跑生成器（见 `cmake/OyiCodegen.cmake`）。
`cmake --build --target codegen` 只跑生成器。

## 前置条件

- Python 3.11+ 与 libclang 绑定：`python -m pip install -r tools/codegen/requirements.txt`
- 一个不低于主机标准库要求版本的 libclang 动态库。CMake 会在 Visual Studio 的 LLVM 组件、Xcode 和
  Homebrew LLVM 里找；可用 `-DOYI_LIBCLANG=<路径>` 或环境变量 `OYI_LIBCLANG` 覆盖。

## 标注

```cpp
#include <oyi_core/Annotations.h>

struct OYI_STRUCT() Vec3 { float mX = 0.0f; float mY = 0.0f; float mZ = 0.0f; };

struct OYI_COMPONENT(Replicated) Position {
    Vec3 mValue{};
    OYI_FIELD(Quantize = 0.01) float mHeight = 0.0f;
};

struct OYI_COMPONENT(Tag) Dead {};
```

| 标注 | 选项 |
|---|---|
| `OYI_COMPONENT(...)` | `Tag`（无数据）、`Replicated`（全部字段同步）、`Chunk`（每 chunk 一份） |
| `OYI_STRUCT()` | 无；标记组件可以内嵌的值类型 |
| `OYI_FIELD(...)` | `Replicated`、`Hidden`、`Quantize = <步长>`（仅 float / double） |

## 规则

| 代码 | 规则 |
|---|---|
| OYI-C001 | 被标注的声明必须是命名空间作用域的 `struct` |
| OYI-C002 | 不允许基类 |
| OYI-C003 | 不允许虚函数、用户声明的构造/析构/赋值 |
| OYI-C004 | 字段类型只允许 `bool`、`float`、`double`、`std::[u]intN_t`、`enum class`、`OYI_STRUCT`/`OYI_COMPONENT` 类型、`oyi::core::Handle<T>`，以及它们的定长数组 |
| OYI-C005 | 每个字段都要有默认成员初始化 |
| OYI-C006 | 不允许隐式 padding；显式写 `std::uint8_t mPad[N] = {};` |
| OYI-C007 | 不允许位域 |
| OYI-C008 | `Tag` 组件不能有字段 |
| OYI-C009 | padding 字段命名 `mPad*`，类型 `std::uint8_t` 或 `std::uint8_t[N]` |
| OYI-C010 | 不允许嵌套类型声明 |
| OYI-C011 | 字段全部 public |
| OYI-C012 | 未知或格式错误的选项 |
| OYI-C013 | `Quantize` 只能用于 `float` / `double` |
| OYI-C016 | 不允许多维数组 |
| OYI-C017 | 枚举必须是带定宽底层类型的 `enum class` |
| OYI-P001 | clang 解析头文件失败；此时单独报告，不再运行其它规则 |

## 命令行

```bash
python -m oyi_codegen validate path/to/Header.h -- -I<include dir> ...
python -m oyi_codegen generate oyi_mod/Header.h --include-root <dir> --out-dir <dir> --depfile <file> -- <clang args>
```

诊断格式 `file:line:col: error: [OYI-Cnnn] message`。退出码：0 成功，1 有错误，2 用法错误。

## 测试

`tests/oyi_codegen_tests`：一个走真实 CMake 流水线的夹具模块，加上 `cases/` 下的 golden 用例
（合法头对应 `expected/<Name>.json`，非法头对应 `expected/<Name>.errors.txt`）。有意改动输出后用
`run_golden.py --update` 重新录制。
