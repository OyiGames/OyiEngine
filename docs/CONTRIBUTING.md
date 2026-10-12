# Contributing

中文版：[CONTRIBUTING.zh-CN.md](CONTRIBUTING.zh-CN.md)

This file is the human-readable summary. The enforced truth is `.clang-format`, `.clang-tidy`,
`scripts/check_sources.py` and the CI workflow; when they disagree with this text, fix the text.

## Language policy

| Where | Language |
|---|---|
| Identifiers, comments, log and assert messages, commit messages, CMake, scripts, CI | English only (source files must be ASCII; CI checks this) |
| Repository documents (`*.md`) | Both languages: `<name>.md` (English, source of truth) and `<name>.zh-CN.md`. Update both in the same PR; CI checks that the pair exists |
| Issues and pull requests | English preferred; Chinese is fine for discussion |
| User-facing strings | Not hard-coded; they go through the localization table once it exists |

## Nothing machine-local in the repository

No absolute paths, drive letters, machine names, user names, local tool locations or AI-assistant
configuration. CI rejects drive-letter paths in tracked text files. Local overrides go into
git-ignored files: `CMakeUserPresets.json`, `.env.local`.

## Code

- C++23. Engine core builds without RTTI and must not throw; errors are `std::expected<T, Error>`;
  every function with a return value is `[[nodiscard]]`.
- Naming: types and functions `PascalCase`; parameters and locals `camelCase`; data members `mName`;
  statics `sName`; constants `kName`; macros `OYI_UPPER_SNAKE`; namespaces `oyi::module`.
- Files: one primary type per file, named `PascalCase.h` / `PascalCase.cpp`; directories lower case.
- Includes: angle brackets only, `#include <oyi_mod/File.h>`; a `.cpp` includes its own header first;
  top-level include directories carry the `oyi_` prefix.
- Components (POD) are declared with `OYI_COMPONENT` / `OYI_STRUCT` from `<oyi_core/Annotations.h>` and
  listed under `COMPONENT_HEADERS` in CMake: no virtual functions, no pointers, fixed-width integers,
  explicit padding, every field value-initialized. `tools/codegen` enforces the rules at build time
  and emits the reflection tables.
- Inside jobs: no `std::function`, no direct TLS, no blocking I/O, no global RNG, no wall-clock time.
- Doc comments use `///`, one sentence on what the function does and when it fails.
- `TODO` must reference an issue: `// TODO(#123): ...`.

## Tests

- New code comes with tests; code produced by an AI agent must come with tests.
- Catch2 test names are English sentences, tagged `[module][feature]`.
- Flaky tests are quarantined the same day.

## Git

- Trunk-based: `main` is protected and always green; short-lived branches `feat/`, `fix/`, `chore/`,
  `docs/`, `ci/`, `build/` in kebab-case; pull requests are squash-merged.
- Commit messages follow Conventional Commits (`feat(ecs): ...`, `fix(job): ...`), imperative mood,
  body wrapped at 72 columns. Commits authored with an AI assistant carry a `Co-Authored-By` trailer.
- One topic per pull request; fill in the template.
- Never commit generated files or build output (`out/`, `generated/`).
- Dependencies are added only through `vcpkg.json` and recorded in `docs/THIRD_PARTY.md`.

## Tooling

| Check | Tool |
|---|---|
| Formatting | `clang-format` 19, `--dry-run -Werror` |
| Static analysis | `clang-tidy` (naming, bugprone, performance, modernize, readability) |
| Python scripts | `ruff check` and `ruff format --check` |
| Conventions | `python scripts/check_sources.py` |
| Component rules | `tools/codegen` during the build; golden tests in `tests/oyi_codegen_tests` |
| Header self-containment | `cmake --build --preset <preset>-verify-headers` |
| Memory and UB | ASan + UBSan preset on every PR |
