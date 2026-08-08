#!/usr/bin/env python3
"""AST-014 evidence: drive the material source IR over every indexed RVMAT.

    python scripts/verify_rvmat_ir.py

Hand-written fixtures only contain what their author thought to put in them. This
runs the real IR, through the shipped PoseidonFormats DLL, over every RVMAT in the
owner-provided corpora and compares its shape against an independent text scan.

That comparison is not academic: it is how `TexGen<N>` was found being parsed as a
stage. Both carry a trailing index, the fixture had no TexGen block, and the bug
would have surfaced much later as a shader schema looking for a generator that had
silently become a textureless stage.

Reported per shader family rather than as one pass rate, because a compatibility
claim has to name its denominator.
"""
from __future__ import annotations

import collections
import ctypes
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DLL = ROOT / "build/win-x64-clang-rwdi/engine/PoseidonFormats/PoseidonFormats.dll"

STAGE_CLASS = re.compile(r"class\s+(Stage)(\d+)\s*\{", re.IGNORECASE)
TEXGEN_CLASS = re.compile(r"class\s+(TexGen)(\d+)\s*\{", re.IGNORECASE)
SHADER = re.compile(r'PixelShaderID\s*=\s*"([^"]*)"', re.IGNORECASE)


def load_dll() -> ctypes.CDLL:
    if not DLL.is_file():
        sys.exit(f"missing {DLL}; build the PoseidonFormatsDLL target first")
    lib = ctypes.CDLL(str(DLL))
    lib.pf_init.restype = ctypes.c_int
    lib.pf_material_load.restype = ctypes.c_void_p
    lib.pf_material_load.argtypes = [ctypes.c_char_p]
    lib.pf_material_free.argtypes = [ctypes.c_void_p]
    lib.pf_material_pixel_shader.restype = ctypes.c_char_p
    lib.pf_material_pixel_shader.argtypes = [ctypes.c_void_p]
    for name in ("pf_material_stage_count", "pf_material_texgen_count", "pf_material_other_class_count"):
        getattr(lib, name).restype = ctypes.c_int
        getattr(lib, name).argtypes = [ctypes.c_void_p]
    for name in ("pf_material_stage_index", "pf_material_stage_is_procedural"):
        getattr(lib, name).restype = ctypes.c_int
        getattr(lib, name).argtypes = [ctypes.c_void_p, ctypes.c_int]
    for name in ("pf_material_stage_texture", "pf_material_stage_uvsource"):
        getattr(lib, name).restype = ctypes.c_char_p
        getattr(lib, name).argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.pf_init()
    return lib


def corpus_roots() -> list[pathlib.Path]:
    import json

    config = ROOT / ".reference-corpora.json"
    if not config.is_file():
        sys.exit("no .reference-corpora.json; see the .example beside it")
    spec = json.loads(config.read_text(encoding="utf-8"))
    return [pathlib.Path(os.path.expandvars(r["path"])) for r in spec["roots"].values()]


def main() -> int:
    lib = load_dll()
    files: list[pathlib.Path] = []
    for root in corpus_roots():
        if root.is_dir():
            files.extend(p for p in root.rglob("*") if p.suffix.lower() == ".rvmat")
    if not files:
        sys.exit("no RVMAT files found in the configured corpora")

    total = refused = 0
    stage_mismatch = texgen_mismatch = 0
    shaders: collections.Counter[str] = collections.Counter()
    uv_sources: collections.Counter[str] = collections.Counter()
    procedural = file_textures = 0
    examples: list[str] = []

    for path in files:
        raw = path.read_bytes()
        if raw[:4] == b"\0raP":
            continue  # binarized; none exist in the current corpora
        text = raw.decode("latin1")
        expected_stages = len(STAGE_CLASS.findall(text))
        expected_texgens = len(TEXGEN_CLASS.findall(text))

        handle = lib.pf_material_load(str(path).encode("utf-8"))
        if not handle:
            refused += 1
            continue
        try:
            total += 1
            shaders[lib.pf_material_pixel_shader(handle).decode(errors="replace") or "<none>"] += 1

            stages = lib.pf_material_stage_count(handle)
            texgens = lib.pf_material_texgen_count(handle)
            if stages != expected_stages:
                stage_mismatch += 1
                if len(examples) < 5:
                    examples.append(f"stages {stages} != {expected_stages}  {path.name}")
            if texgens != expected_texgens:
                texgen_mismatch += 1
                if len(examples) < 5:
                    examples.append(f"texgens {texgens} != {expected_texgens}  {path.name}")

            for i in range(stages):
                if lib.pf_material_stage_is_procedural(handle, i):
                    procedural += 1
                elif lib.pf_material_stage_texture(handle, i):
                    file_textures += 1
                source = lib.pf_material_stage_uvsource(handle, i).decode(errors="replace")
                if source:
                    uv_sources[source] += 1
        finally:
            lib.pf_material_free(handle)

    print(f"RVMAT parsed through the IR : {total}")
    print(f"  refused                   : {refused}")
    print(f"  stage-count mismatches    : {stage_mismatch}")
    print(f"  texgen-count mismatches   : {texgen_mismatch}")
    for line in examples:
        print(f"      {line}")
    print(f"\ntexture references: {file_textures} file, {procedural} procedural")
    print("uvSource values:", dict(uv_sources.most_common(8)))
    print("\nshader families (denominator for any MAT-030 claim):")
    for name, count in shaders.most_common(12):
        print(f"  {name:44} {count}")
    return 1 if (stage_mismatch or texgen_mismatch) else 0


if __name__ == "__main__":
    raise SystemExit(main())
