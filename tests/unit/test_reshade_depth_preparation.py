"""Offline integrity/configuration checks; these do not compile or render FX."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("depth", Path(__file__).parents[2] / "scripts/Prepare-ReShadeDepth.py")
DEPTH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPTH)


class Preparation(unittest.TestCase):
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
