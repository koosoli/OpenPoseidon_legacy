"""CPU filesystem transaction tests; no real runtime cache, network or process use."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("upgrade", Path(__file__).parents[2] / "scripts/Upgrade-ReShadeSceneCache.py")
UPGRADE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPGRADE)


class Upgrade(unittest.TestCase):
    def setUp(self):
        guard = patch.object(UPGRADE, "require_game_closed")
        self.guard = guard.start()
        self.addCleanup(guard.stop)

    def fixture(self, directory):
        root = Path(directory) / "runtime"
        stage = Path(directory) / "stage"
        root.mkdir()
        stage.mkdir()
        (root / "ReShade.ini").write_bytes(b"user paths and overlay tuning")
        (root / "OpenPoseidon.ini").write_bytes(b"Strength=0.723")
        (root / "OpenPoseidon-UI-Session-A.ini").write_bytes(b"custom session tuning")
        (root / "OpenPoseidonReShade.addon64").write_bytes(b"old owned addon")
        (root / "Presets").mkdir()
        (root / "Presets/User.ini").write_bytes(b"user custom preset")
        files = {"Shaders/OpenPoseidonDH.fx": b"verified fixed shader",
                 "Presets/OpenPoseidon-DH-Scene-Preview.ini": b"verified fixed preset",
                 "OpenPoseidonReShade.addon64": b"new compiled addon",
                 "DEPTH-PROVENANCE.json": b"verified receipt"}
        return root, stage, files

    def test_plan_is_read_only_apply_preserves_tuning_and_backs_up_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root, stage, files = self.fixture(directory)
            before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            with patch.object(UPGRADE, "cached_core"), patch.object(UPGRADE, "payload", return_value=files):
                plan = UPGRADE.install(root, stage)
                self.assertNotIn("applied", plan)
                self.assertEqual(before, {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()})
                result = UPGRADE.install(root, stage, True)
                backup = Path(result["backup"])
                self.assertEqual((backup / "OpenPoseidonReShade.addon64").read_bytes(), b"old owned addon")
                for name, data in before.items():
                    if str(name) != "OpenPoseidonReShade.addon64":
                        self.assertEqual((root / name).read_bytes(), data)
                self.assertEqual((root / "OpenPoseidonReShade.addon64").read_bytes(), b"new compiled addon")
                again = UPGRADE.install(root, stage, True)
                self.assertIsNone(again["backup"])  # Exact payload is idempotent.

    def test_conflict_refuses_before_any_runtime_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root, stage, files = self.fixture(directory)
            path = root / "Presets/OpenPoseidon-DH-Scene-Preview.ini"
            path.write_bytes(b"user adjusted GI tuning")
            before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            with patch.object(UPGRADE, "cached_core"), patch.object(UPGRADE, "payload", return_value=files):
                with self.assertRaisesRegex(RuntimeError, "Preserving changed"):
                    UPGRADE.install(root, stage, True)
            self.assertEqual(before, {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()})
            self.assertFalse((root / "Backups").exists())

    def test_failed_write_restores_completed_payload_and_keeps_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root, stage, files = self.fixture(directory)
            # Replace the old binary first, then simulate a subsequent disk error.
            files = {"OpenPoseidonReShade.addon64": files["OpenPoseidonReShade.addon64"],
                     "Shaders/OpenPoseidonDH.fx": files["Shaders/OpenPoseidonDH.fx"]}
            original_write = UPGRADE.atomic_write
            def fail_shader(path, data):
                if path.name.endswith(".fx"):
                    raise OSError("simulated disk failure")
                return original_write(path, data)
            with patch.object(UPGRADE, "cached_core"), patch.object(UPGRADE, "payload", return_value=files), patch.object(UPGRADE, "atomic_write", side_effect=fail_shader):
                with self.assertRaisesRegex(OSError, "simulated"):
                    UPGRADE.install(root, stage, True)
            self.assertEqual((root / "OpenPoseidonReShade.addon64").read_bytes(), b"old owned addon")
            self.assertFalse((root / "Shaders/OpenPoseidonDH.fx").exists())
            self.assertEqual(len(list((root / "Backups/scene-preview").iterdir())), 1)

    def test_path_cannot_escape_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "leaves intended"):
                UPGRADE.inside(Path(directory), "../outside")


if __name__ == "__main__":
    unittest.main()
