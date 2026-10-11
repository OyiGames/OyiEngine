#!/usr/bin/env python3
"""Repository convention checks (ADR-0003, ADR-0021). Runs on every PR; locally:

    python scripts/check_sources.py [--vcpkg-include <dir>]

Checks:
1. .h/.cpp/.inl files under engine/, tools/, tests/ are named in PascalCase.
2. No `#include "..."`; angle brackets only.
3. Every top-level directory under a module's include root starts with `oyi_`.
4. Source files are ASCII (comments and identifiers are English; Unicode belongs in data, not code).
5. No machine-local information in tracked text files (absolute drive-letter paths).
6. Repository documents are bilingual: every `<name>.md` has a `<name>.zh-CN.md` sibling and vice versa.
7. Collision check: first-level names under our include roots must not exist in the vcpkg include root
   (when `--vcpkg-include` is given or a vcpkg_installed tree can be found).

Non-zero exit on any problem; one problem per line so both humans and agents can read it.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIRS = ["engine", "tools", "tests"]
PASCAL = re.compile(r"^[A-Z][A-Za-z0-9]*$")
QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s+"')
LOCAL_PATH = re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/]")
TEXT_SUFFIXES = (".h", ".cpp", ".inl", ".cmake", ".txt", ".json", ".yml", ".yaml", ".md", ".py", ".toml")
DOC_LANG_SUFFIX = ".zh-CN.md"


def rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace("\\", "/")


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True, capture_output=True).stdout
    return [os.path.join(ROOT, p.decode("utf-8")) for p in out.split(b"\0") if p]


def iter_sources():
    for d in SOURCE_DIRS:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if f.endswith((".h", ".cpp", ".inl")):
                    yield os.path.join(dirpath, f)


def check_filenames(problems: list[str]) -> None:
    for path in iter_sources():
        stem = os.path.splitext(os.path.basename(path))[0]
        if not PASCAL.match(stem):
            problems.append(f"{rel(path)}: file name must be PascalCase")


def check_quoted_includes(problems: list[str]) -> None:
    for path in iter_sources():
        with open(path, encoding="utf-8") as fh:
            for n, line in enumerate(fh, 1):
                if QUOTED_INCLUDE.match(line):
                    problems.append(f'{rel(path)}:{n}: use #include <...>, not "..."')


def check_ascii_sources(problems: list[str]) -> None:
    for path in iter_sources():
        with open(path, "rb") as fh:
            for n, line in enumerate(fh, 1):
                if any(b > 0x7F for b in line):
                    problems.append(f"{rel(path)}:{n}: non-ASCII character; code and comments must be English")


def include_roots() -> list[str]:
    return [p for p in glob.glob(os.path.join(ROOT, "engine", "*", "include")) if os.path.isdir(p)]


def check_prefix(problems: list[str]) -> None:
    for root in include_roots():
        for name in os.listdir(root):
            if not name.startswith("oyi_"):
                problems.append(f"{rel(os.path.join(root, name))}: top-level include dir must start with oyi_")


def check_local_info(problems: list[str], files: list[str]) -> None:
    for path in files:
        if not path.endswith(TEXT_SUFFIXES):
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                if LOCAL_PATH.search(line):
                    problems.append(f"{rel(path)}:{n}: absolute local path (machine-local information)")


def check_bilingual_docs(problems: list[str], files: list[str]) -> None:
    docs = [p for p in files if p.endswith(".md") and not rel(p).startswith(".github/")]
    names = {rel(p) for p in docs}
    for doc in sorted(names):
        if doc.endswith(DOC_LANG_SUFFIX):
            english = doc[: -len(DOC_LANG_SUFFIX)] + ".md"
            if english not in names:
                problems.append(f"{doc}: missing English counterpart {english}")
        else:
            chinese = doc[: -len(".md")] + DOC_LANG_SUFFIX
            if chinese not in names:
                problems.append(f"{doc}: missing Chinese counterpart {chinese}")


def check_collisions(problems: list[str], vcpkg_include: str | None) -> None:
    if vcpkg_include is None:
        candidates = glob.glob(os.path.join(ROOT, "out", "build", "*", "vcpkg_installed", "*", "include")) + glob.glob(
            os.path.join(ROOT, "vcpkg_installed", "*", "include")
        )
        vcpkg_include = candidates[0] if candidates else None
    if vcpkg_include is None or not os.path.isdir(vcpkg_include):
        print("note: vcpkg include dir not found, collision check skipped")
        return
    third_party = {os.path.splitext(n)[0] for n in os.listdir(vcpkg_include)}
    for root in include_roots():
        for name in os.listdir(root):
            if name in third_party:
                problems.append(f"{rel(os.path.join(root, name))}: collides with {vcpkg_include}/{name}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vcpkg-include", default=None)
    args = ap.parse_args(argv)
    files = tracked_files()
    problems: list[str] = []
    check_filenames(problems)
    check_quoted_includes(problems)
    check_ascii_sources(problems)
    check_prefix(problems)
    check_local_info(problems, files)
    check_bilingual_docs(problems, files)
    check_collisions(problems, args.vcpkg_include)
    for p in problems:
        print(p)
    print(f"check_sources: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
