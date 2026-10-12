"""oyi_codegen: reflection and schema generator for OyiEngine components (ADR-0011).

Parses C++ headers with libclang, validates component declarations against the POD rules
and emits `<name>.gen.h` (constexpr ComponentInfo tables) plus `<name>.schema.json`.
The same program doubles as the linter behind `oyi schema validate`.
"""

__version__ = "0.1.0"
