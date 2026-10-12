#!/usr/bin/env python3
"""Exercises the `oyi` command-line tool (ADR-0016): exit codes, JSON output, error lines.

    run_cli.py --oyi <exe> --cases <codegen golden cases dir> --work <dir>
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys


def run(exe: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([exe, *args], capture_output=True, text=True, check=False)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--oyi", required=True)
    ap.add_argument("--cases", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args(argv)
    os.makedirs(args.work, exist_ok=True)
    failures: list[str] = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        if condition:
            print(f"ok   {name}")
        else:
            print(f"FAIL {name} {detail}")
            failures.append(name)

    # --version: text and JSON with a non-zero schema hash
    result = run(args.oyi, "--version")
    expect("version text", result.returncode == 0 and "schema 0x" in result.stdout, result.stdout)
    result = run(args.oyi, "--version", "--json")
    expect("version json exit", result.returncode == 0, result.stderr)
    try:
        doc = json.loads(result.stdout)
        fields_ok = set(doc) == {"engine", "protocol", "schemaHash"} and doc["schemaHash"] != "0"
        expect("version json fields", fields_ok, result.stdout)
    except json.JSONDecodeError:
        expect("version json parse", False, result.stdout)

    # usage errors
    expect("no args is usage", run(args.oyi).returncode == 2)
    expect("unknown command is usage", run(args.oyi, "frobnicate").returncode == 2)
    expect("world without file is usage", run(args.oyi, "world", "inspect").returncode == 2)
    expect("unknown switch is usage", run(args.oyi, "--version", "--verbose").returncode == 2)

    # inspect on missing and damaged files: exit 1 with a JSON error line
    missing = os.path.join(args.work, "missing.oyiw")
    result = run(args.oyi, "world", "inspect", missing, "--json")
    expect(
        "world missing file",
        result.returncode == 1 and json.loads(result.stdout)["error"]["code"] == "IoFailure",
        result.stdout,
    )
    damaged = os.path.join(args.work, "damaged.oyib")
    with open(damaged, "wb") as fh:
        fh.write(b"not a blob at all, just some bytes to read")
    result = run(args.oyi, "asset", "inspect", damaged, "--json")
    expect(
        "asset damaged file",
        result.returncode == 1 and json.loads(result.stdout)["error"]["code"] == "CorruptData",
        result.stdout,
    )
    result = run(args.oyi, "world", "inspect", damaged)
    expect("world damaged text", result.returncode == 1 and "CorruptData" in result.stderr, result.stderr)
    result = run(args.oyi, "world", "inspect", damaged, "--quiet")
    expect("quiet says nothing", result.returncode == 1 and result.stdout == "" and result.stderr == "")

    # the files the engine tests wrote, when they ran before us
    world_file = os.path.join(args.work, "..", "oyi_cli_world.oyiw")
    if os.path.exists(world_file):
        result = run(args.oyi, "world", "inspect", world_file, "--json")
        try:
            doc = json.loads(result.stdout)
            world_ok = (
                result.returncode == 0
                and doc["entityCount"] > 0
                and len(doc["archetypes"]) >= 1
                and doc["components"][0]["name"].startswith("oyi::")
            )
            expect("world inspect json", world_ok, result.stdout)
        except (json.JSONDecodeError, KeyError):
            expect("world inspect parse", False, result.stdout)
        result = run(args.oyi, "world", "inspect", world_file)
        expect("world inspect text", result.returncode == 0 and "archetype" in result.stdout, result.stdout)
    blob_file = os.path.join(args.work, "..", "oyi_cli_asset.oyib")
    if os.path.exists(blob_file):
        result = run(args.oyi, "asset", "inspect", blob_file, "--json")
        try:
            doc = json.loads(result.stdout)
            expect("asset inspect json", result.returncode == 0 and doc["payloadSize"] % 64 == 0, result.stdout)
        except (json.JSONDecodeError, KeyError):
            expect("asset inspect parse", False, result.stdout)

    # schema validate forwards to the generator
    result = run(args.oyi, "schema", "validate", os.path.join(args.cases, "Basic.h"))
    expect("schema validate ok", result.returncode == 0, result.stdout + result.stderr)
    result = run(args.oyi, "schema", "validate", os.path.join(args.cases, "Invalid.h"))
    expect("schema validate fails", result.returncode == 1 and "error:" in result.stderr, result.stderr[-400:])
    result = run(args.oyi, "schema", "validate", os.path.join(args.cases, "Basic.h"), "--json")
    try:
        doc = json.loads(result.stdout)
        schema_ok = result.returncode == 0 and doc["records"][0]["name"].startswith("oyi::golden::")
        expect("schema validate json", schema_ok, result.stdout[:200])
    except (json.JSONDecodeError, KeyError):
        expect("schema validate json parse", False, result.stdout[:400] + result.stderr[-400:])

    print(f"run_cli: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
