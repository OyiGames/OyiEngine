"""libclang loading and parsing. Everything that touches clang.cindex directly lives here or in analyze.py."""

from __future__ import annotations

import os
import sys

from oyi_codegen.model import Diagnostic

_loaded_library: str | None = None


def load_libclang(path: str | None) -> None:
    """Point the python bindings at a libclang shared library.

    Order: explicit argument, OYI_LIBCLANG environment variable, whatever the bindings find on their own.
    The library must be at least as new as the host standard library expects (MSVC's STL rejects
    older clangs with STL1000), which is why the host toolchain's libclang is used instead of a wheel.
    """
    global _loaded_library
    import clang.cindex as ci

    chosen = path or os.environ.get("OYI_LIBCLANG")
    if chosen:
        if _loaded_library is not None and _loaded_library != chosen:
            raise RuntimeError("libclang already loaded from a different path")
        if _loaded_library is None:
            ci.Config.set_library_file(chosen)
    _loaded_library = chosen or ""


def parse(path: str, clang_args: list[str]):
    """Parse one header as C++23 with OYI_CODEGEN defined. Returns (translation_unit, diagnostics)."""
    import clang.cindex as ci

    args = ["-x", "c++", "-std=c++23", "-DOYI_CODEGEN=1", "-Wno-pragma-once-outside-header", *clang_args]
    index = ci.Index.create()
    try:
        tu = index.parse(path, args=args, options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
    except ci.TranslationUnitLoadError as exc:
        return None, [Diagnostic(path, 0, 0, "OYI-P001", f"clang failed to load {path}: {exc}")]

    diagnostics: list[Diagnostic] = []
    for diag in tu.diagnostics:
        if diag.severity < ci.Diagnostic.Error:
            continue
        loc = diag.location
        file_name = loc.file.name if loc.file else path
        diagnostics.append(Diagnostic(file_name, loc.line, loc.column, "OYI-P001", f"clang: {diag.spelling}"))
    return tu, diagnostics


def dependencies(tu, main_path: str) -> list[str]:
    """Absolute paths of the main file and everything it includes, for the depfile."""
    seen: dict[str, None] = {os.path.abspath(main_path): None}
    for inclusion in tu.get_includes():
        seen.setdefault(os.path.abspath(inclusion.include.name), None)
    return list(seen)


def python_bindings_version() -> str:
    import clang.cindex as ci

    module = sys.modules.get("clang")
    version = getattr(module, "__version__", None) if module else None
    return version or getattr(ci, "__version__", "unknown")
