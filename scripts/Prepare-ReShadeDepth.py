#!/usr/bin/env python3
"""Explicit optional depth probe + pinned GPL DH_UBER_RT preparation; never selects a preset."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tempfile

DH_COMMIT = "0783b8fde77df48412d79070be7de4bc3743b898"
COMMON_COMMIT = "fd0022170615ce0d8162d219bff07232fa6dd84f"
FILES = (
    ("AlucardDH/dh-reshade-shaders", DH_COMMIT, "LICENSE", "Licenses/DH-UBER-RT-GPL-2.0.txt",
     "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643"),
    ("AlucardDH/dh-reshade-shaders", DH_COMMIT, "Shaders/dh_uber_rt.fx", "Sources/dh_uber_rt.fx",
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


def adapt_dh(data):
    """Generate a marked local derivative only from the verified upstream pin.

    Keep the upstream file and licence intact; never guess patches on a new pin.
    Depth remains normalized by an explicit metric working range for DH's
    filters, while both view reconstruction and inverse projection use metres.
    """
    if sha(data) != FILES[1][4]:
        raise RuntimeError("DH adapter requires the exact pinned upstream source")
    source = data.decode().replace("\r\n", "\n")
    source = '// OpenPoseidon local adaptation, 2026-10-08: checked scene depth and metric projection.\n' + source
    source = source.replace('#include "Reshade.fxh"', '#include "Reshade.fxh"\n#include "OpenPoseidonSceneDepth.fxh"', 1)
    # Top-level function bodies have four-space indentation in this exact pin.
    def body(name, replacement):
        nonlocal source
        pattern = rf'(^    (?:float[234]?|bool) {name}\([^\n]*\) \{{\n).*?\n    \}}'
        source, count = re.subn(pattern, lambda m: m[1] + replacement + '\n    }', source, count=0, flags=re.S | re.M)
        if count != 1:
            raise RuntimeError(f"DH adapter function mismatch: {name}, {count}")
    body('getDepthMultiplier', '        return 1.0; // engine snapshot: no game-specific depth multiplier')
    body('isSky', '        return !OpSceneTraceDepthValid(depth); // invalid receiver/donor or bounded range exhausted')
    body('getDepth', '        return float2(OpSceneLinearDepth(coords), 0.0); // no weapon heuristic')
    body('getWorldPositionForNormal', '        return OpSceneViewPosition(coords, getDepth(coords).x); // metric normals, no roughness displacement')
    body('getWorldPosition', '        return OpSceneViewPosition(coords, depth);')
    body('getScreenPosition', '        return OpSceneScreenPosition(wp);')
    debug_pattern = r'(\} else if\(iDebug==DEBUG_DEPTH\) \{).*?(\} else if\(iDebug==DEBUG_NORMAL\) \{)'
    source, count = re.subn(debug_pattern, lambda m: m[1] + '\n            float depth = getDepth(coords).x;\n'
        '            result = isSky(depth) ? float3(0.2,0.4,1.0) :\n'
        '                (log2(1.0 + depth * OpSceneRange) / log2(1.0 + OpSceneRange)).xxx;\n'
        '        ' + m[2], source, flags=re.S)
    if count != 1:
        raise RuntimeError('DH depth debug patch mismatch')
    # The pinned smoother writes native scene color into GI for sky/far pixels.
    # That is not neutral lighting: it gets added again during composition.
    def replace_once(old, new, label):
        nonlocal source
        if source.count(old) != 1:
            raise RuntimeError(f'DH scene-preservation patch mismatch: {label}')
        source = source.replace(old, new, 1)
    for coordinate in ('scaledCoords', 'coords'):
        replace_once(f'outGI = float4(getColor({coordinate}).rgb,1);',
                     'outGI = float4(0,0,0,1); // invalid/far: zero indirect light, unoccluded AO',
                     'neutral smoother ' + coordinate)
    replace_once('\n        float3 color = refColor;',
                 '\n        if(isSky(depth)) return refColor; // preserve native fog/sky, including Reinhard input\n'
                 '        float3 color = refColor;', 'composition receiver')
    replace_once('        if(iDebug==DEBUG_OFF) {',
                 '        if(iDebug==DEBUG_OFF) {\n'
                 '            if(!OpSceneGiReady || isSky(getDepth(coords).x)) {\n'
                 '                outPixel = tex2Dlod(ReShade::BackBuffer,float4(coords,0,0));\n'
                 '                return; // exact native beauty passthrough before AO/SSR/levels/fading\n'
                 '            }', 'beauty receiver')
    replace_once('float ratio = diff/max;', 'float ratio = saturate(diff/max);', 'bounded existing range fade')
    donor = r'if\(hitPosition.status==RT_HIT_SKY \|\| isSky\(screenCoords.z\)\) \{.*?return;\s*\}'
    source, count = re.subn(donor,
        'if(hitPosition.status==RT_HIT_SKY || isSky(screenCoords.z) || isSky(getDepth(screenCoords.xy).x)) {\n'
        '            return; // no visible in-range surface: no extra sky/fog screen-space radiance\n'
        '        }', source, flags=re.S)
    if count != 1:
        raise RuntimeError('DH valid donor patch mismatch')
    history = r'if\(isSky\(depth.x\)\) \{\s*#if DX9_MODE\s*return;\s*#else.*?#endif\s*\}'
    source, count = re.subn(history,
        'if(isSky(depth.x)) {\n            return; // current invalid/far donor cannot be rescued by stale depth\n        }',
        source, flags=re.S)
    if count != 1:
        raise RuntimeError('DH current-depth donor patch mismatch')
    # Both spatial and history filters used a factor that disables depth
    # rejection in the far half of the normalized range. Keep the pin's near
    # 10 m bandwidth in metric units throughout the scene, without a leak floor.
    old_weight = 'max(0.001,1.0-100*diffDepth*saturate(1.0-refDepth*2.0))'
    if source.count(old_weight) != 2:
        raise RuntimeError('DH metric bilateral filter patch mismatch')
    source = source.replace(old_weight, 'OpSceneDepthFilterWeight(refDepth.x, depth.x)')
    # Defined accumulators and neutral zero-coverage results are necessary when
    # every candidate is rejected; don't normalize undefined/empty weights.
    if source.count('float2 weightSum;') != 2:
        raise RuntimeError('DH smoother accumulator patch mismatch')
    source = source.replace('float2 weightSum;', 'float2 weightSum = 0.0;')
    replace_once('float3 result = giAo.rgb/weightSum.x;',
        'float3 result = weightSum.x > 0.0 ? giAo.rgb/weightSum.x : 0.0;', 'empty spatial weights')
    replace_once('(giAo.rgb/ weightSum.x).x',
        '(giAo.rgb/ max(weightSum.x, 0.000001)).x', 'empty history comparison')
    if source.count('giAo.rgb /= weightSum.x;') != 2:
        raise RuntimeError('DH empty normalization patch mismatch')
    source = source.replace('giAo.rgb /= weightSum.x;',
        'giAo.rgb = weightSum.x > 0.0 ? giAo.rgb/weightSum.x : 0.0;')
    # ReShade 6.8 tracks transitive sampler use, including compureResult's
    # reinhardFirstPass=false branch when PS_ReinhardPass passes true. Never
    # sample and attach that same image. Preserve the original 1/64 blend in a
    # retained work target, then publish its complete 1x1 value in a new pass.
    declaration = '    sampler reinhardSampler { Texture = reinhardTex; };'
    if source.count(declaration) != 1:
        raise RuntimeError('DH Reinhard declaration patch mismatch')
    source = source.replace(declaration, declaration + '\n'
        '    texture opReinhardWorkTex { Width = 1; Height = 1; Format = RGBA16F; };\n'
        '    sampler opReinhardWorkSampler { Texture = opReinhardWorkTex; };\n'
        '    void OpPublishReinhard(float4 p : SV_Position, float2 uv : TexCoord, out float4 value : SV_Target0) {\n'
        '        value = tex2Dlod(opReinhardWorkSampler, float4(0.5,0.5,0,0));\n    }', 1)
    pattern = r'(PixelShader = PS_ReinhardPass;\s*RenderTarget = )reinhardTex;(.*?\n        \})'
    source, count = re.subn(pattern, lambda m: m[1] + 'opReinhardWorkTex;' + m[2] + '\n'
        '        pass {\n            VertexShader = PostProcessVS;\n'
        '            PixelShader = OpPublishReinhard;\n            RenderTarget = reinhardTex;\n'
        '            BlendEnable = false;\n        }', source, flags=re.S)
    if count != 1:
        raise RuntimeError('DH Reinhard pass patch mismatch')
    return source.encode()


def prepare(root, local_sources=None, probe_only=False):
    if not probe_only and (root / 'Shaders/dh_uber_rt.fx').exists():
        raise RuntimeError('Original DH source remains in active effect scan; choose a fresh isolated runtime')
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
    adapter = None
    if not probe_only:
        dh = next(data for path, data in staged if path.name == 'dh_uber_rt.fx')
        adapter = adapt_dh(dh)
        staged.append((root / 'Shaders/OpenPoseidonDH.fx', adapter))
        staged.append((root / 'Shaders/OpenPoseidonSceneDepth.fxh', (own / 'OpenPoseidonSceneDepth.fxh').read_bytes()))
    presets = {
        "OpenPoseidon-Depth-Off.ini": "Techniques=\nTechniqueSorting=OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx,DH_UBER_RT@OpenPoseidonDH.fx\n",
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
        "OpenPoseidon-DH-Scene-Debug.ini": "Techniques=DH_UBER_RT@OpenPoseidonDH.fx\nTechniqueSorting=DH_UBER_RT@OpenPoseidonDH.fx\n"
            "\n[OpenPoseidonDH.fx]\niDebug=5\nbSSR=0\nbRemoveAmbient=0\niGIFrameAccu=1\niAOFrameAccu=1\niSSRFrameAccu=1\n"
            "fGIRenderScale=0.333\niRTMaxRays=2\niRTPrecision=1\nOpSceneRange=1000\nfWeaponDepth=0\nfNormalRoughness=0\n",
        "OpenPoseidon-DH-Scene-Preview.ini": "Techniques=DH_UBER_RT@OpenPoseidonDH.fx\nTechniqueSorting=DH_UBER_RT@OpenPoseidonDH.fx\n"
            "\n[OpenPoseidonDH.fx]\niDebug=0\nbSSR=0\nbRemoveAmbient=0\niGIFrameAccu=1\niAOFrameAccu=1\niSSRFrameAccu=1\n"
            "fGIRenderScale=0.333\niRTMaxRays=2\niRTPrecision=1\nOpSceneRange=1000\nfWeaponDepth=0\nfNormalRoughness=0\n"
            "fAOMultiplier=0.4\nfGIFinalMerging=0.35\n",
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
        "scene_adapter": None if adapter is None else {"source_sha256": FILES[1][4], "generated_sha256": sha(adapter),
            "helper_sha256": sha((own / 'OpenPoseidonSceneDepth.fxh').read_bytes()), "helper_license": "MIT",
            "projection": "metric z=near/raw; exact p00/p55/offset inverse, screen-down Y; 1000m bounded DH range; sky=2",
            "reinhard": "retained 1x1 work target with upstream blend, then separate publish pass; no same-image feedback",
            "validity": "native beauty passthrough unless producer proves native surface fog disabled; independent depth debug; open metric working range; neutral invalid GI/AO; no invalid/stale-depth donors; clamped existing fade",
            "filter": "symmetric 10m depth bandwidth, inherited from pinned near filter at1000m; zero weight for invalid/separated layers; defined empty accumulators; not a fog correction"},
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
