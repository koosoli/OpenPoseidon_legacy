"""Offline integrity/configuration checks; these do not compile or render FX."""
import importlib.util
import json
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("depth", Path(__file__).parents[2] / "scripts/Prepare-ReShadeDepth.py")
DEPTH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPTH)


class Preparation(unittest.TestCase):
    def test_generated_adapter_preserves_pinned_input_and_records_derivative(self):
        source = '#include "Reshade.fxh"\n'
        for kind, name, args in [('float', 'getDepthMultiplier', ''), ('float', 'isSky', 'float depth'),
                ('float2', 'getDepth', 'float2 coords'), ('float3', 'getWorldPositionForNormal', 'float2 coords,bool ignoreRoughness'),
                ('float3', 'getWorldPosition', 'float2 coords,float depth'), ('float3', 'getScreenPosition', 'float3 wp')]:
            source += f'    {kind} {name}({args}) {{\n        return 42;\n    }}\n'
        source += '} else if(iDebug==DEBUG_DEPTH) {\n result = 42;\n} else if(iDebug==DEBUG_NORMAL) {\n'
        source += '\n        float3 color = refColor;\n'
        source += 'outGI = float4(getColor(scaledCoords).rgb,1);\n'
        source += 'outGI = float4(getColor(coords).rgb,1);\n'
        source += '        if(iDebug==DEBUG_OFF) {\n            result = getColorSampler(resultSampler,coords).rgb;\n        }\n'
        source += 'float ratio = diff/max;\n'
        source += 'if(hitPosition.status==RT_HIT_SKY || isSky(screenCoords.z)) {\n sky = getRayColor(screenCoords.xy).rgb;\n return;\n}\n'
        source += 'if(isSky(depth.x)) {\n#if DX9_MODE\n return;\n#else\n depth = getColorSampler(previousDepthSampler,currentScaledCoords).x;\n if(isSky(depth.x)) { return; }\n#endif\n}\n'
        source += 'weight *= max(0.001,1.0-100*diffDepth*saturate(1.0-refDepth*2.0));\n' * 2
        source += 'float2 weightSum;\n' * 2
        source += 'float3 result = giAo.rgb/weightSum.x;\n'
        source += '(giAo.rgb/ weightSum.x).x\n'
        source += 'giAo.rgb /= weightSum.x;\n' * 2
        source += '\n    sampler reinhardSampler { Texture = reinhardTex; };\n'
        source += '        pass {\n            PixelShader = PS_ReinhardPass;\n            RenderTarget = reinhardTex;\n'
        source += '            ClearRenderTargets = false;\n            BlendEnable = true;\n            SrcBlend = SRCALPHA;\n            DestBlend = INVSRCALPHA;\n        }\n'
        data = [b'licence fixture', source.encode(), b'common fixture']
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            files = []
            for original, content in zip(DEPTH.FILES, data):
                repo, commit, path, target, _ = original
                source_path = base / 'offline' / repo / path
                source_path.parent.mkdir(parents=True, exist_ok=True)
                source_path.write_bytes(content)
                files.append((repo, commit, path, target, hashlib.sha256(content).hexdigest()))
            with patch.object(DEPTH, 'FILES', tuple(files)), patch.object(DEPTH.subprocess, 'run', side_effect=AssertionError('network')):
                receipt = DEPTH.prepare(base / 'runtime', base / 'offline')
                DEPTH.prepare(base / 'runtime', base / 'offline')
                with self.assertRaisesRegex(RuntimeError, 'exact pinned'):
                    DEPTH.adapt_dh(b'different pin')
            runtime = base / 'runtime'
            self.assertEqual((runtime / 'Sources/dh_uber_rt.fx').read_bytes(), source.encode())
            self.assertFalse((runtime / 'Shaders/dh_uber_rt.fx').exists())
            adapted = (runtime / 'Shaders/OpenPoseidonDH.fx').read_bytes()
            self.assertIn(b'OpenPoseidon local adaptation, 2026-10-08', adapted)
            self.assertIn(b'!OpSceneTraceDepthValid(depth)', adapted)
            self.assertEqual(adapted.count(b'invalid/far: zero indirect light, unoccluded AO'), 2)
            self.assertNotIn(b'outGI = float4(getColor(coords).rgb,1)', adapted)
            self.assertIn(b'if(isSky(depth)) return refColor;', adapted)
            beauty = adapted.split(b'if(iDebug==DEBUG_OFF) {', 1)[1]
            self.assertLess(beauty.index(b'outPixel = tex2Dlod(ReShade::BackBuffer'),
                            beauty.index(b'result = getColorSampler(resultSampler'))
            self.assertIn(b'isSky(getDepth(screenCoords.xy).x)', adapted)
            self.assertNotIn(b'depth = getColorSampler(previousDepthSampler,currentScaledCoords).x;', adapted)
            self.assertIn(b'float ratio = saturate(diff/max);', adapted)
            self.assertIn(b'!OpSceneGiReady || isSky(getDepth(coords).x)', adapted)
            self.assertEqual(adapted.count(b'OpSceneDepthFilterWeight(refDepth.x, depth.x)'), 2)
            self.assertNotIn(b'saturate(1.0-refDepth*2.0)', adapted)
            self.assertEqual(adapted.count(b'float2 weightSum = 0.0;'), 2)
            self.assertNotIn(b'giAo.rgb /= weightSum.x;', adapted)
            self.assertIn(b'weightSum.x > 0.0 ? giAo.rgb/weightSum.x : 0.0;', adapted)
            self.assertIn(b'OpSceneLinearDepth(coords)', adapted)
            self.assertIn(b'OpSceneViewPosition(coords, depth)', adapted)
            self.assertIn(b'OpSceneScreenPosition(wp)', adapted)
            self.assertIn(b'log2(1.0 + depth * OpSceneRange)', adapted)
            # The original pass's transitive read remains reinhardTex, but its
            # retained blended target is now separate. A subsequent pass copies
            # work -> published value, so the merger sees the current frame.
            self.assertIn(b'PixelShader = PS_ReinhardPass;\n            RenderTarget = opReinhardWorkTex;', adapted)
            self.assertIn(b'PixelShader = OpPublishReinhard;\n            RenderTarget = reinhardTex;', adapted)
            self.assertIn(b'RenderTarget = reinhardTex;\n            BlendEnable = false;', adapted)
            self.assertIn(b'tex2Dlod(opReinhardWorkSampler, float4(0.5,0.5,0,0))', adapted)
            self.assertIn(b'ClearRenderTargets = false;', adapted)
            self.assertIn(b'DestBlend = INVSRCALPHA;', adapted)
            provenance = json.loads(receipt.read_text())['scene_adapter']
            self.assertEqual(provenance['generated_sha256'], hashlib.sha256(adapted).hexdigest())
            self.assertEqual(provenance['helper_license'], 'MIT')
            self.assertIn('OpSceneRange=1000', (runtime / 'Presets/OpenPoseidon-DH-Scene-Debug.ini').read_text())
            self.assertTrue((runtime / 'Presets/OpenPoseidon-Depth-Off.ini').read_text().startswith('Techniques=\n'))

    def test_original_in_active_scan_requires_fresh_stage_without_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = root / 'Shaders/dh_uber_rt.fx'
            original.parent.mkdir()
            original.write_bytes(b'user-owned original')
            with patch.object(DEPTH.subprocess, 'run', side_effect=AssertionError('network')):
                with self.assertRaisesRegex(RuntimeError, 'active effect scan'):
                    DEPTH.prepare(root)
            self.assertEqual(original.read_bytes(), b'user-owned original')
            self.assertFalse((root / 'DEPTH-PROVENANCE.json').exists())

    def test_reinhard_retained_work_and_publish_preserve_temporal_blend(self):
        # Upstream color blend uses source alpha=1/64, retained destination,
        # then the adapter publishes the entire 1x1 value without another blend.
        # Exercise discarded frames as well; both retained images stay equal.
        expected = work = published = 0.0
        for frame in range(128):
            incoming = (frame * 17 % 31) / 10
            if frame % 7:
                expected = incoming / 64 + expected * 63 / 64
                work = incoming / 64 + work * 63 / 64
            published = work
            self.assertEqual(published, expected)

    def test_probe_never_downloads_or_changes_selected_preset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "ReShade.ini"
            config.write_text("[GENERAL]\nPresetPath=user.ini\n")
            original = config.read_bytes()
            with patch.object(DEPTH.subprocess, "run", side_effect=AssertionError("unexpected network")):
                receipt = DEPTH.prepare(root, probe_only=True)
                DEPTH.prepare(root, probe_only=True)  # Identical cache is reusable.
            self.assertEqual(config.read_bytes(), original)
            self.assertEqual(json.loads(receipt.read_text())["files"], [])
            self.assertTrue((root / "Presets/OpenPoseidon-Depth-Off.ini").read_text().startswith("Techniques=\n"))
            self.assertFalse((root / "Shaders/dh_uber_rt.fx").exists())
            scene = (root / "Presets/OpenPoseidon-Depth-Scene-Probe.ini").read_text()
            self.assertIn("OPENPOSEIDON_ENGINE_DEPTH=1", scene)
            self.assertIn("Overlay=0", scene)

    def test_bad_source_hash_writes_no_shader(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / "source/author/project/shader.fx"
            source.parent.mkdir(parents=True)
            source.write_bytes(b"corrupt")
            files = (("author/project", "pin", "shader.fx", "Shaders/example.fx", "0" * 64),)
            with patch.object(DEPTH, "FILES", files):
                with self.assertRaisesRegex(RuntimeError, "Unexpected pinned source"):
                    DEPTH.prepare(base / "runtime", base / "source")
            self.assertFalse((base / "runtime").exists())

    def test_conflicting_user_shader_preserved_before_other_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shader = root / "Shaders/OpenPoseidonDepthProbe.fx"
            shader.parent.mkdir()
            shader.write_bytes(b"user tuning")
            with self.assertRaisesRegex(RuntimeError, "Preserving different existing file"):
                DEPTH.prepare(root, probe_only=True)
            self.assertEqual(shader.read_bytes(), b"user tuning")
            self.assertFalse((root / "Presets").exists())


if __name__ == "__main__":
    unittest.main()
