#!/usr/bin/env python3
"""Offline local scene-preview upgrade. Preserves user configs/presets; no process launch."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

REPO = Path(__file__).resolve().parents[1]
MANIFEST_SHA = "aa21713718843e531da396e2bfc80772c9cb3c369d6c30b836be6b0ae812d503"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


CORE = module("reshade_core", REPO / "scripts/Prepare-ReShade.py")
DEPTH = module("reshade_depth", REPO / "scripts/Prepare-ReShadeDepth.py")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inside(root, relative):
    result = root / relative
    if not result.resolve().is_relative_to(root.resolve()):
        raise RuntimeError(f"Path leaves intended runtime: {relative}")
    return result


def cached_core(root):
    expected = {"ReShade64.dll": CORE.DLL_SHA, "ReShade64.json": MANIFEST_SHA,
                "Shaders/OpenPoseidonLook.fx": sha((REPO / "tools/reshade-bridge/OpenPoseidonLook.fx").read_bytes())}
    for name, digest in expected.items():
        if sha(inside(root, name).read_bytes()) != digest:
            raise RuntimeError(f"Preserving different cached core/look file: {name}")
    provenance = json.loads(inside(root, "PROVENANCE.json").read_text(encoding="utf-8-sig"))
    if provenance.get("api_source_commit") != CORE.COMMIT or provenance.get("version") != CORE.VERSION:
        raise RuntimeError("Existing cached API provenance does not identify the supported pin")
    for name in ("ReShade.ini", "OpenPoseidon.ini", "ReShade-LICENSE.md", "api/reshade.hpp"):
        if not inside(root, name).is_file():
            raise RuntimeError(f"Existing prepared cache required: {name}; no downloads performed")


def prepare(runtime, stage, local_sources):
    runtime, stage = runtime.resolve(), stage.resolve()
    if stage.exists() or stage.is_relative_to(runtime) or runtime.is_relative_to(stage):
        raise RuntimeError("Choose a new staging directory outside the existing runtime")
    cached_core(runtime)
    # Preflight every pinned source before creating any stage. No acquire/network fallback.
    for repository, _, source, _, expected in DEPTH.FILES:
        data = (local_sources / repository / source).read_bytes()
        if len(data) > 512 * 1024 or sha(data) != expected:
            raise RuntimeError(f"Missing/different offline pinned source: {repository}/{source}")
    stage.mkdir(parents=True)
    for name in ("ReShade64.dll", "ReShade64.json", "ReShade-LICENSE.md", "PROVENANCE.json",
                 "ReShade.ini", "OpenPoseidon.ini", "Shaders/OpenPoseidonLook.fx"):
        destination = inside(stage, name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(inside(runtime, name), destination)
    shutil.copytree(inside(runtime, "api"), stage / "api")
    DEPTH.prepare(stage, local_sources=local_sources)
    return stage


def payload(stage):
    policy = (REPO / "engine/Poseidon/Dev/Debug/ReShadePreviewPolicy.hpp").read_text()
    assets = dict(re.findall(r'\{"([^"\n]+)", "([a-f0-9]{64})"\}', policy))
    if len(assets) != 9:
        raise RuntimeError("Preview policy changed; review the upgrade asset catalogue")
    assets["Presets/OpenPoseidon-Depth-Off.ini"] = sha(
        b"Techniques=\nTechniqueSorting=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx,DH_UBER_RT@OpenPoseidonDH.fx\n")
    result = {}
    for name, expected in assets.items():
        data = inside(stage, name).read_bytes()
        if sha(data) != expected:
            raise RuntimeError(f"Staged fixed asset digest mismatch: {name}")
        result[name] = data
    receipt = inside(stage, "DEPTH-PROVENANCE.json").read_bytes()
    provenance = json.loads(receipt)
    adapter = provenance.get("scene_adapter") or {}
    if (provenance.get("dh_license") != "GPL-2.0 (upstream LICENSE)" or
            provenance.get("probe_license") != "GPL-3.0-or-later" or
            adapter.get("helper_license") != "MIT" or
            adapter.get("generated_sha256") != assets["Shaders/OpenPoseidonDH.fx"]):
        raise RuntimeError("Staged licence/derivative receipt missing or mismatched")
    result["DEPTH-PROVENANCE.json"] = receipt
    addon = inside(stage, "OpenPoseidonReShade.addon64").read_bytes()
    # The PowerShell wrapper builds this from the current repository, not an external download.
    if len(addon) < 256 or addon[:2] != b"MZ":
        raise RuntimeError("Build the current Windows x64 add-on in the stage first")
    offset = int.from_bytes(addon[60:64], "little")
    if addon[offset:offset+6] != b"PE\0\0\x64\x86":
        raise RuntimeError("Staged add-on is not a Windows x64 PE image")
    result["OpenPoseidonReShade.addon64"] = addon
    return result


def plan(runtime, stage):
    runtime, stage = runtime.resolve(), stage.resolve()
    if stage.is_relative_to(runtime) or runtime.is_relative_to(stage):
        raise RuntimeError("Stage and runtime must be separate")
    cached_core(runtime)
    if inside(runtime, "Shaders/dh_uber_rt.fx").exists():
        raise RuntimeError("Preserving original DH in active scan; use a separate runtime instead")
    files = payload(stage)
    actions = []
    for name, data in files.items():
        destination = inside(runtime, name)
        existing = destination.read_bytes() if destination.exists() else None
        if existing is not None and existing != data and name not in (
                "OpenPoseidonReShade.addon64", "DEPTH-PROVENANCE.json"):
            raise RuntimeError(f"Preserving changed shader/preset/licence: {name}; use an isolated runtime")
        actions.append({"path": name, "sha256": sha(data),
                        "action": "unchanged" if existing == data else "replace-backed-up" if existing is not None else "add"})
    preserved = {name: sha(inside(runtime, name).read_bytes()) for name in ("ReShade.ini", "OpenPoseidon.ini")}
    return {"runtime": str(runtime), "stage": str(stage), "files": actions, "preservedConfigs": preserved,
            "network": "none: cached pinned sources and existing official SDK/core only",
            "selection": "No preset, graphics preference or startup mode changed; GI requires explicit developer selection"}, files


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".upgrade-" + uuid.uuid4().hex)
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def require_game_closed():
    if os.name == "nt":
        for executable in ("OpenPoseidon.exe", "ColdWarAssault.exe"):
            result = subprocess.run(["tasklist.exe", "/FI", "IMAGENAME eq " + executable, "/FO", "CSV", "/NH"],
                                    check=True, capture_output=True, text=True)
            if executable.lower() in result.stdout.lower():
                raise RuntimeError("Close the game before applying the local cache upgrade")


def install(runtime, stage, apply=False):
    if apply:
        require_game_closed()
    report, files = plan(runtime, stage)  # Full conflict preflight before any destination write.
    if not apply:
        return report
    runtime = runtime.resolve()
    changed = [row["path"] for row in report["files"] if row["action"] != "unchanged"]
    if not changed:
        return dict(report, applied=True, backup=None)
    backup = inside(runtime, "Backups/scene-preview/" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
    backup.mkdir(parents=True)
    originals = {}
    for name in changed + list(report["preservedConfigs"]):
        source = inside(runtime, name)
        data = source.read_bytes() if source.exists() else None
        if name in changed:
            originals[name] = data
        if data is not None:
            target = inside(backup, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    (backup / "UPGRADE-PLAN.json").write_text(json.dumps(report, indent=2) + "\n")
    applied = []
    try:
        for name in changed:
            atomic_write(inside(runtime, name), files[name])
            applied.append(name)
        for name, expected in report["preservedConfigs"].items():
            if sha(inside(runtime, name).read_bytes()) != expected:
                raise RuntimeError(f"Existing config changed during upgrade: {name}")
    except Exception:
        for name in reversed(applied):
            destination = inside(runtime, name)
            if originals[name] is None:
                destination.unlink(missing_ok=True)
            else:
                atomic_write(destination, originals[name])
        raise
    return dict(report, applied=True, backup=str(backup))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    preparation = commands.add_parser("prepare")
    preparation.add_argument("runtime", type=Path)
    preparation.add_argument("stage", type=Path)
    preparation.add_argument("--local-sources", type=Path, required=True)
    installation = commands.add_parser("install")
    installation.add_argument("runtime", type=Path)
    installation.add_argument("stage", type=Path)
    installation.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    if args.command == "prepare":
        print(prepare(args.runtime, args.stage, args.local_sources))
    else:
        print(json.dumps(install(args.runtime, args.stage, args.apply), indent=2))
