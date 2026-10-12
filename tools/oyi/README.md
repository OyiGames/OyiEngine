# oyi

中文版：[README.zh-CN.md](README.zh-CN.md)

The single command-line entry point (ADR-0016). Every subcommand takes `--json` (one JSON object
on stdout, failures as `{"error":{"code":..,"message":..}}`) and `--quiet`. Exit codes: 0 ok,
1 the command ran and failed, 2 usage, 3 internal error.

| Command | What it does |
|---|---|
| `oyi --version [--json]` | Engine version, protocol version and the schema hash of this build (every engine module that declares components). |
| `oyi world inspect <file.oyiw> [--json]` | Header, component table and archetypes of a world saved with `World::Save`; no registry needed. |
| `oyi asset inspect <file.oyib> [--json]` | Header of a baked blob after verifying magic, format, size and content hash. |
| `oyi schema validate <header.h> [--json]` | Runs tools/codegen's linter on a component header. Finds the generator, Python, libclang and the compiler arguments of this checkout; `OYI_CODEGEN_DIR`, `OYI_PYTHON`, `OYI_LIBCLANG` and `OYI_CODEGEN_ARGS_FILE` override. |

Planned (ADR-0016): `world query | diff | snapshot | restore`, `sched graph --dot | validate`,
`replay`, `net`, `serve --rpc`, `asset dump | diff | bake | check`.
