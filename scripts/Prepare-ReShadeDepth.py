#!/usr/bin/env python3
"""Explicit optional depth probe + pinned GPL DH_UBER_RT preparation; never selects a preset."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

DH_COMMIT = "0783b8fde77df48412d79070be7de4bc3743b898"
COMMON_COMMIT = "fd0022170615ce0d8162d219bff07232fa6dd84f"
FILES = (
    ("AlucardDH/dh-reshade-shaders", DH_COMMIT, "LICENSE", "Licenses/DH-UBER-RT-GPL-2.0.txt",
     "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643"),
    ("AlucardDH/dh-reshade-shaders", DH_COMMIT, "Shaders/dh_uber_rt.fx", "Shaders/dh_uber_rt.fx",
     "c14f19eb7b0359cb4042749e52c28c6a18c2df89b9fd40f5ab9bd209e36e5320"),
    ("crosire/reshade-shaders", COMMON_COMMIT, "Shaders/ReShade.fxh", "Shaders/ReShade.fxh",
     "6dabfbbaf968c3871905d2ea17f96572ff7b1cec01310b5d0e5252b66b30174f"),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_preserved(path, data):
    if path.exists() and path.read_bytes() != data:
        raise RuntimeError(f"Preserving different existing file: {path}; choose an isolated runtime")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def prepare(root, local_sources=None, probe_only=False):
    receipt = []
    staged = []
    for repo, commit, source, target, expected in (() if probe_only else FILES):
        url = f"https://raw.githubusercontent.com/{repo}/{commit}/{source}"
        if local_sources:
            data = (local_sources / repo / source).read_bytes()
        elif (root / target).is_file() and sha((root / target).read_bytes()) == expected:
            data = (root / target).read_bytes()
        else:
            # Match the existing Windows preparer's working IPv4 curl route.
            with tempfile.TemporaryDirectory(prefix="op-reshade-depth-") as temporary:
                download = Path(temporary) / "source"
                subprocess.run(["curl.exe", "-4", "--fail", "--location", "--connect-timeout", "10",
                    "--max-time", "30", "--max-filesize", "524288", "--user-agent",
                    "OpenPoseidon-OptionalDepth-Manual/1.0", "--output", str(download), url], check=True)
                data = download.read_bytes()
        if len(data) > 512 * 1024 or sha(data) != expected:
            raise RuntimeError(f"Unexpected pinned source for {repo}/{source}; no retry")
        staged.append((root / target, data))
        receipt.append({"url": url, "sha256": expected, "bytes": len(data), "target": target})
    own = Path(__file__).resolve().parents[1] / "tools" / "reshade-depth"
    staged.append((root / "Shaders/OpenPoseidonDepthProbe.fx", (own / "OpenPoseidonDepthProbe.fx").read_bytes()))
    presets = {
        "OpenPoseidon-Depth-Off.ini": "Techniques=\nTechniqueSorting=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx,DH_UBER_RT@dh_uber_rt.fx\n",
        "OpenPoseidon-Depth-Probe.ini": "Techniques=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx\n"
            "TechniqueSorting=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx\n"
            "\n[OpenPoseidonDepthProbe.fx]\nReversedZ=1\nFlipY=0\nDepthPower=0.125\nOverlay=0.25\n",
        "OpenPoseidon-Depth-Scene-Probe.ini": "PreprocessorDefinitions=OPENPOSEIDON_ENGINE_DEPTH=1\n"
            "Techniques=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx\n"
            "TechniqueSorting=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx\n"
            "\n[OpenPoseidonDepthProbe.fx]\nReversedZ=1\nFlipY=0\nDepthPower=0.125\nOverlay=0\n",
        "OpenPoseidon-DH-RTGI-Preview.ini": "PreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=1,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0\n"
            "Techniques=DH_UBER_RT@dh_uber_rt.fx\nTechniqueSorting=DH_UBER_RT@dh_uber_rt.fx\n"
            "\n[dh_uber_rt.fx]\niDebug=0\nbSSR=0\nbRemoveAmbient=0\nfGIRenderScale=0.333\niRTMaxRays=2\n"
            "iRTPrecision=1\niGIFrameAccu=1\niAOFrameAccu=1\niSSRFrameAccu=1\nfAOMultiplier=0.4\nfGIFinalMerging=0.35\n",
        "OpenPoseidon-DH-Depth-Debug.ini": "PreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=1,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0\n"
            "Techniques=DH_UBER_RT@dh_uber_rt.fx\nTechniqueSorting=DH_UBER_RT@dh_uber_rt.fx\n"
            "\n[dh_uber_rt.fx]\niDebug=5\nbSSR=0\nbRemoveAmbient=0\niGIFrameAccu=1\niAOFrameAccu=1\n",
    }
    if probe_only:
        presets = {name: text for name, text in presets.items() if name.startswith("OpenPoseidon-Depth-")}
    staged.extend((root / "Presets" / name, value.encode()) for name, value in presets.items())
    # Check every destination before writing any; keep tuning/configs untouched.
    for path, data in staged:
        if path.exists() and path.read_bytes() != data:
            raise RuntimeError(f"Preserving different existing file: {path}; choose an isolated runtime")
    for path, data in staged:
        write_preserved(path, data)
    receipt_path = root / "DEPTH-PROVENANCE.json"
    receipt_path.write_text(json.dumps({"probe_only": probe_only, "files": receipt, "dh_license": "GPL-2.0 (upstream LICENSE)",
        "common_license": "CC0-1.0 (SPDX in header)", "probe_license": "GPL-3.0-or-later",
        "configuration": "No active preset/config/preference modified; depth availability not established",
        "motion": "No external motion provider; accumulation=1 stationary validation preset only"}, indent=2) + "\n")
    return receipt_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path, help="Prepared opt-in runtime or isolated staging directory")
    parser.add_argument("--local-sources", type=Path, help="Offline sources arranged as owner/repository/path")
    parser.add_argument("--probe-only", action="store_true", help="Only stage original diagnostic/off presets; zero downloads")
    args = parser.parse_args()
    print(prepare(args.runtime.resolve(), args.local_sources, args.probe_only))
