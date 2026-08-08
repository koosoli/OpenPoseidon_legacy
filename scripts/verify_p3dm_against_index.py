#!/usr/bin/env python3
"""AST-011B evidence: check the C++ P3DM reader against the AST-007 index.

    python scripts/verify_p3dm_against_index.py [--limit N]

A synthetic fixture proves the reader parses what this repository wrote, which is
close to circular -- the same understanding of the format produced both sides. The
useful question is whether it agrees with thousands of real Bohemia models, and
whether it agrees with an implementation that shares no code with it.

So this drives the shipped PoseidonFormats DLL over the corpora AST-007 indexed
and compares, per model, the LOD count and every LOD resolution against what the
independent Python walker recorded. The two were written from the format
description separately; a shared misreading would have to be identical in both to
go unnoticed here, and a desynchronised walk cannot produce matching resolutions
by chance because they are exact sentinels like 1.0e13.

Needs the corpora, so it is a local evidence tool rather than a CI test -- CI has
no licensed Bohemia data. The synthetic fixtures in tests/fixtures/mlod cover the
same paths for CI.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
INDEX = ROOT / "docs/roadmap/evidence/compat-c1-fixture-index.json"
DLL = ROOT / "build/win-x64-clang-rwdi/engine/PoseidonFormats/PoseidonFormats.dll"


def load_dll() -> ctypes.CDLL:
    if not DLL.is_file():
        sys.exit(f"missing {DLL}; build the PoseidonFormatsDLL target first")
    lib = ctypes.CDLL(str(DLL))
    lib.pf_init.restype = ctypes.c_int
    lib.pf_model_load.restype = ctypes.c_void_p
    lib.pf_model_load.argtypes = [ctypes.c_char_p]
    lib.pf_model_free.argtypes = [ctypes.c_void_p]
    lib.pf_last_error.restype = ctypes.c_char_p
    lib.pf_model_lod_count.restype = ctypes.c_int
    lib.pf_model_lod_count.argtypes = [ctypes.c_void_p]
    lib.pf_model_lod_resolution.restype = ctypes.c_float
    lib.pf_model_lod_resolution.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.pf_lod_material_count.restype = ctypes.c_int
    lib.pf_lod_material_count.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.pf_init()
    return lib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    if not INDEX.is_file():
        sys.exit("no fixture index; run scripts/build_reference_fixture_index.py first")
    index = json.loads(INDEX.read_text(encoding="utf-8"))
    config = json.loads((ROOT / ".reference-corpora.json").read_text(encoding="utf-8"))
    roots = {alias: pathlib.Path(spec["path"]) for alias, spec in config["roots"].items()}

    lib = load_dll()
    checked = refused = mismatched = 0
    failures: list[str] = []

    for root in index["roots"]:
        base = roots.get(root["alias"])
        if base is None or not base.is_dir():
            continue
        for fixture in root["fixtures"]:
            if fixture["kind"] != "model" or fixture["headline"].get("container") != "MLOD":
                continue
            if args.limit and checked >= args.limit:
                break
            path = base / fixture["path"]
            handle = lib.pf_model_load(str(path).encode("utf-8"))
            if not handle:
                refused += 1
                failures.append(f"REFUSED {fixture['path']}: {lib.pf_last_error().decode(errors='replace')}")
                continue
            try:
                checked += 1
                expected_lods = fixture["headline"]["lods"]
                actual_lods = lib.pf_model_lod_count(handle)
                if actual_lods != expected_lods:
                    mismatched += 1
                    failures.append(f"LODCOUNT {fixture['path']}: cpp={actual_lods} index={expected_lods}")
                    continue
                # Resolutions are exact sentinels, so an offset error anywhere in
                # the walk shows up here as a number that is not merely close.
                resolutions = fixture["headline"].get("lod_resolutions") or []
                for lod in range(min(actual_lods, len(resolutions))):
                    expected = resolutions[lod]
                    if expected is None:
                        continue
                    actual = lib.pf_model_lod_resolution(handle, lod)
                    if not math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-9):
                        mismatched += 1
                        failures.append(f"RESOLUTION {fixture['path']} lod{lod}: cpp={actual} index={expected}")
                        break
            finally:
                lib.pf_model_free(handle)

    print(f"checked   {checked}")
    print(f"refused   {refused}")
    print(f"mismatched {mismatched}")
    for line in failures[:25]:
        print("  " + line)
    if len(failures) > 25:
        print(f"  ... and {len(failures) - 25} more")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
