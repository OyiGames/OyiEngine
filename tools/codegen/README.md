# oyi_codegen

中文版：[README.zh-CN.md](README.zh-CN.md)

Build-time reflection generator for component declarations (ADR-0011). It parses C++ headers with
libclang, validates every `OYI_COMPONENT` / `OYI_STRUCT` against the POD rules, and emits:

- `<oyi_mod/X.gen.h>`: `constexpr oyi::core::ComponentInfo` tables plus `static_assert`s that pin
  size, alignment and every field offset, so a layout change without regeneration fails to compile;
- `<oyi_mod/X.schema.json>`: the same data for tools and the editor.

CMake runs it automatically for headers listed under `COMPONENT_HEADERS` in `oyi_add_module`
(see `cmake/OyiCodegen.cmake`). `cmake --build --target codegen` runs only the generators.

## Requirements

- Python 3.11+ and the libclang bindings: `python -m pip install -r tools/codegen/requirements.txt`
- A libclang shared library that is at least as new as the host standard library expects. CMake
  looks in the Visual Studio LLVM component, Xcode and Homebrew LLVM; override with
  `-DOYI_LIBCLANG=<path>` or the `OYI_LIBCLANG` environment variable.

## Annotations

```cpp
#include <oyi_core/Annotations.h>

struct OYI_STRUCT() Vec3 { float mX = 0.0f; float mY = 0.0f; float mZ = 0.0f; };

struct OYI_COMPONENT(Replicated) Position {
    Vec3 mValue{};
    OYI_FIELD(Quantize = 0.01) float mHeight = 0.0f;
};

struct OYI_COMPONENT(Tag) Dead {};
```

| Annotation | Options |
|---|---|
| `OYI_COMPONENT(...)` | `Tag` (no data), `Replicated` (all fields), `Chunk` (one value per chunk) |
| `OYI_STRUCT()` | none; marks a value type that components may embed |
| `OYI_FIELD(...)` | `Replicated`, `Hidden`, `Quantize = <step>` (float and double only) |

## Rules

| Code | Rule |
|---|---|
| OYI-C001 | annotated declaration must be a `struct` at namespace scope |
| OYI-C002 | no base classes |
| OYI-C003 | no virtual functions, no user-declared constructors, destructor or assignment |
| OYI-C004 | field types: `bool`, `float`, `double`, `std::[u]intN_t`, `enum class`, `OYI_STRUCT`/`OYI_COMPONENT` types, `oyi::core::Handle<T>`, and fixed-size arrays of those |
| OYI-C005 | every field has a default member initializer |
| OYI-C006 | no implicit padding; spell it out as `std::uint8_t mPad[N] = {};` |
| OYI-C007 | no bit-fields |
| OYI-C008 | a `Tag` component has no fields |
| OYI-C009 | padding fields are named `mPad*` and are `std::uint8_t` or `std::uint8_t[N]` |
| OYI-C010 | no nested type declarations |
| OYI-C011 | all fields public |
| OYI-C012 | unknown or malformed option |
| OYI-C013 | `Quantize` only on `float` / `double` |
| OYI-C016 | no multi-dimensional arrays |
| OYI-C017 | enums must be `enum class` with a fixed-width underlying type |
| OYI-P001 | clang could not parse the header; reported on its own, no other rule runs |

## Command line

```bash
python -m oyi_codegen validate path/to/Header.h -- -I<include dir> ...
python -m oyi_codegen generate oyi_mod/Header.h --include-root <dir> --out-dir <dir> --depfile <file> -- <clang args>
```

Diagnostics are printed as `file:line:col: error: [OYI-Cnnn] message`. Exit codes: 0 ok, 1 errors, 2 usage.

## Tests

`tests/oyi_codegen_tests`: a fixture module built through the real CMake pipeline plus golden cases
in `cases/` (`expected/<Name>.json` for valid headers, `expected/<Name>.errors.txt` for invalid ones).
To re-record after an intentional change, run `run_golden.py` with `--update`.
