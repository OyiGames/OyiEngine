#!/usr/bin/env python3
"""Golden tests for tools/codegen.

    run_golden.py --codegen-dir DIR --libclang PATH --cases DIR --work DIR [--update] -- <clang args>

Every cases/<Name>.h is run through the generator. If cases/expected/<Name>.errors.txt exists the
header is expected to fail validation with exactly those diagnostics (paths reduced to basenames);
otherwise cases/expected/<Name>.json must equal the emitted schema JSON. --update re-records.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys


def run_codegen(
    codegen_dir: str, libclang: str | None, args: list[str], clang_args: list[str]
) -> subprocess.CompletedProcess:
    cmd = [sys.executable, "-m", "oyi_codegen", *args]
    if libclang:
        cmd += ["--libclang", libclang]
    cmd += ["--", *clang_args]
    return subprocess.run(cmd, cwd=codegen_dir, capture_output=True, text=True, check=False)


def normalize_errors(stderr: str) -> str:
    lines = []
    for line in stderr.splitlines():
        if ": error: " not in line:
            continue
        location, _, rest = line.partition(": error: ")
        location = re.sub(r"^.*[\\/]", "", location)
        lines.append(f"{location}: error: {rest}")
    return "\n".join(sorted(lines)) + "\n"


def read(path: str) -> str | None:
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def main(argv: list[str]) -> int:
    clang_args: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, clang_args = argv[:i], argv[i + 1 :]
    ap = argparse.ArgumentParser()
    ap.add_argument("--codegen-dir", required=True)
    ap.add_argument("--libclang", default=None, help="defaults to the OYI_LIBCLANG environment variable")
    ap.add_argument("--cases", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args(argv)

    expected_dir = os.path.join(args.cases, "expected")
    failures = 0
    cases = sorted(f for f in os.listdir(args.cases) if f.endswith(".h"))
    for case in cases:
        name = os.path.splitext(case)[0]
        expected_errors = os.path.join(expected_dir, f"{name}.errors.txt")
        expected_json = os.path.join(expected_dir, f"{name}.json")

        if os.path.exists(expected_errors) or (args.update and not os.path.exists(expected_json)):
            result = run_codegen(
                args.codegen_dir, args.libclang, ["validate", os.path.join(args.cases, case)], clang_args
            )
            actual = normalize_errors(result.stderr)
            if result.returncode == 0 and not args.update:
                print(f"FAIL {case}: expected validation errors, got none")
                failures += 1
                continue
            if args.update:
                if result.returncode == 0:
                    print(f"note {case}: validates cleanly; recording its schema instead")
                else:
                    write(expected_errors, actual)
                    print(f"updated {expected_errors}")
                    continue
            else:
                if actual != read(expected_errors):
                    print(f"FAIL {case}: diagnostics differ\n--- expected\n{read(expected_errors)}--- actual\n{actual}")
                    failures += 1
                else:
                    print(f"ok   {case}")
                continue

        result = run_codegen(
            args.codegen_dir,
            args.libclang,
            ["generate", case, "--include-root", args.cases, "--out-dir", args.work],
            clang_args,
        )
        if result.returncode != 0:
            print(f"FAIL {case}: generator failed\n{result.stderr}")
            failures += 1
            continue
        actual = read(os.path.join(args.work, f"{name}.schema.json")) or ""
        generated_header = read(os.path.join(args.work, f"{name}.gen.h")) or ""
        if re.search(r"[A-Za-z]:[\\/]", generated_header):
            print(f"FAIL {case}: generated header contains an absolute path")
            failures += 1
        if args.update:
            write(expected_json, actual)
            print(f"updated {expected_json}")
        elif actual != read(expected_json):
            print(f"FAIL {case}: schema differs from {expected_json}\n--- actual\n{actual}")
            failures += 1
        else:
            print(f"ok   {case}")

    print(f"run_golden: {len(cases)} case(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
