"""Command line entry point.

    python -m oyi_codegen generate <include-relative header> --include-root DIR --out-dir DIR
                                   [--depfile FILE] [--libclang PATH] [-- <clang args>]
    python -m oyi_codegen validate <header path> [--libclang PATH] [-- <clang args>]

Diagnostics are printed as `file:line:col: error: [OYI-Cnnn] message` so build systems and IDEs link them.
Exit code 0 on success, 1 on validation errors, 2 on usage errors.
"""

from __future__ import annotations

import argparse
import os
import sys

from oyi_codegen import analyze, emit, frontend
from oyi_codegen.model import Diagnostic


def _split_clang_args(argv: list[str]) -> tuple[list[str], list[str]]:
    if "--" in argv:
        i = argv.index("--")
        return argv[:i], argv[i + 1 :]
    return argv, []


def _run(path: str, header_name: str, libclang: str | None, clang_args: list[str]):
    frontend.load_libclang(libclang)
    tu, diagnostics = frontend.parse(path, clang_args)
    if tu is None or diagnostics:
        # clang errors (OYI-P001) leave recovered, half-valid types in the AST; running the rules on
        # it would only add misleading follow-on diagnostics, so stop at the parse errors.
        return None, diagnostics
    schema = analyze.analyze(tu, path, header_name, diagnostics)
    return schema, diagnostics


def _report(diagnostics: list[Diagnostic]) -> int:
    for d in sorted(diagnostics, key=lambda x: (x.file, x.line, x.column, x.code)):
        print(d.format(), file=sys.stderr)
    return 1 if diagnostics else 0


def cmd_generate(args: argparse.Namespace, clang_args: list[str]) -> int:
    header = args.header.replace("\\", "/")
    path = os.path.join(args.include_root, header)
    schema, diagnostics = _run(path, header, args.libclang, clang_args)
    if diagnostics or schema is None:
        return _report(diagnostics)

    stem, _ = os.path.splitext(header)
    gen_h = os.path.join(args.out_dir, stem + ".gen.h")
    gen_json = os.path.join(args.out_dir, stem + ".schema.json")
    os.makedirs(os.path.dirname(gen_h), exist_ok=True)
    with open(gen_h, "w", encoding="ascii", newline="\n") as fh:
        fh.write(emit.render_header(schema))
    with open(gen_json, "w", encoding="ascii", newline="\n") as fh:
        fh.write(emit.render_json(schema))
    if args.depfile:
        os.makedirs(os.path.dirname(os.path.abspath(args.depfile)), exist_ok=True)
        with open(args.depfile, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(emit.render_depfile([gen_h], schema.dependencies))
    return 0


def cmd_validate(args: argparse.Namespace, clang_args: list[str]) -> int:
    header_name = os.path.basename(args.header)
    schema, diagnostics = _run(args.header, header_name, args.libclang, clang_args)
    code = _report(diagnostics)
    if code == 0 and schema is not None and args.json:
        sys.stdout.write(emit.render_json(schema))
    return code


def main(argv: list[str]) -> int:
    own, clang_args = _split_clang_args(argv)
    ap = argparse.ArgumentParser(
        prog="oyi_codegen", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="command", required=True)

    gen = sub.add_parser("generate", help="emit <name>.gen.h and <name>.schema.json for one header")
    gen.add_argument("header", help="header path relative to --include-root, e.g. oyi_transform/Components.h")
    gen.add_argument("--include-root", required=True)
    gen.add_argument("--out-dir", required=True)
    gen.add_argument("--depfile")
    gen.add_argument("--libclang", help="path to libclang; defaults to OYI_LIBCLANG")
    gen.set_defaults(func=cmd_generate)

    val = sub.add_parser("validate", help="check one header against the component rules")
    val.add_argument("header", help="path to the header")
    val.add_argument("--json", action="store_true", help="print the schema JSON on success")
    val.add_argument("--libclang")
    val.set_defaults(func=cmd_validate)

    try:
        args = ap.parse_args(own)
    except SystemExit as exc:
        return 2 if exc.code else 0
    return args.func(args, clang_args)
