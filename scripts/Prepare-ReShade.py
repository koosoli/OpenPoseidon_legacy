#!/usr/bin/env python3
"""Prepare an opt-in, local official ReShade runtime without running its installer."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

VERSION = "6.8.0"
COMMIT = "18deaa52de0c425a78b329e9cb3c497281cd00ec"
INSTALLER_SHA = "afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445"
DLL_SHA = "0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7"
INSTALLER_URL = f"https://reshade.me/downloads/ReShade_Setup_{VERSION}_Addon.exe"
SOURCE_URL = f"https://api.github.com/repos/crosire/reshade/zipball/{COMMIT}"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def acquire(path, url, expected=None):
    if not path.is_file():
        subprocess.run(["curl.exe", "-4", "--fail", "--location", "--retry", "2",
                        "--connect-timeout", "15", "--max-time", "120", url,
                        "--output", str(path)], check=True)
    if expected and digest(path) != expected:
        raise RuntimeError(f"Unexpected SHA256 for {path}; refusing runtime preparation")
    return path


def embedded_archive(data):
    # The official setup scans for its appended ZIP; its executable is never run.
    start = 0
    while (offset := data.find(b"PK\x03\x04", start)) >= 0:
        try:
            archive = zipfile.ZipFile(io.BytesIO(data[offset:]))
            if {"ReShade64.dll", "ReShade64.json"}.issubset(archive.namelist()):
                return archive
        except zipfile.BadZipFile:
            pass
        start = offset + 4
    raise RuntimeError("Official installer embedded archive not found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("--installer", type=Path)
    parser.add_argument("--source-archive", type=Path)
    args = parser.parse_args()
    root = args.runtime.resolve()
    root.mkdir(parents=True, exist_ok=True)
    downloads = root / "downloads"
    downloads.mkdir(exist_ok=True)
    installer = acquire(args.installer or downloads / "setup.exe", INSTALLER_URL, INSTALLER_SHA)
    source = acquire(args.source_archive or downloads / "source.zip", SOURCE_URL)
    with embedded_archive(installer.read_bytes()) as archive:
        for name in ("ReShade64.dll", "ReShade64.json"):
            (root / name).write_bytes(archive.read(name))
    if digest(root / "ReShade64.dll") != DLL_SHA:
        raise RuntimeError("Extracted official x64 runtime digest does not match")
    includes = root / "api"
    includes.mkdir(exist_ok=True)
    with zipfile.ZipFile(source) as archive:
        prefixes = {entry.split("/", 1)[0] for entry in archive.namelist()}
        if prefixes != {"crosire-reshade-" + COMMIT[:7]}:
            raise RuntimeError("Source archive does not identify the pinned official commit")
        for entry in archive.namelist():
            relative = entry.split("/", 1)[-1]
            if relative.startswith("include/reshade") and relative.endswith(".hpp"):
                (includes / Path(relative).name).write_bytes(archive.read(entry))
            elif relative == "LICENSE.md":
                (root / "ReShade-LICENSE.md").write_bytes(archive.read(entry))
    shaders = root / "Shaders"
    shaders.mkdir(exist_ok=True)
    own = Path(__file__).resolve().parents[1] / "tools" / "reshade-bridge"
    shutil.copyfile(own / "OpenPoseidonLook.fx", shaders / "OpenPoseidonLook.fx")
    # Preserve user tuning once created. All paths resolve under the opt-in cache.
    config = root / "ReShade.ini"
    if not config.exists():
        config.write_text("[GENERAL]\nEffectSearchPaths=.\\Shaders\n"
                          "PresetPath=.\\OpenPoseidon.ini\nPerformanceMode=0\n"
                          "[ADDON]\nAddonPath=.\n[INPUT]\nKeyOverlay=36,0,0,0\n"
                          "[SCREENSHOT]\nSavePath=.\\Screenshots\n", encoding="utf-8")
    preset = root / "OpenPoseidon.ini"
    if not preset.exists():
        preset.write_text("Techniques=OpenPoseidonLook@OpenPoseidonLook.fx\n"
                          "TechniqueSorting=OpenPoseidonLook@OpenPoseidonLook.fx\n"
                          "\n[OpenPoseidonLook.fx]\nStrength=0.45\n", encoding="utf-8")
    (root / "PROVENANCE.json").write_text(json.dumps({
        "version": VERSION, "api_source_commit": COMMIT,
        "installer_url": INSTALLER_URL, "installer_sha256": digest(installer),
        "runtime_sha256": DLL_SHA, "source_url": SOURCE_URL,
        "source_archive_sha256": digest(source),
        "installation": "Process-local Vulkan explicit layer; no registry/global install",
    }, indent=2) + "\n", encoding="utf-8")
    print(root)


if __name__ == "__main__":
    main()
