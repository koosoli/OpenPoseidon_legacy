import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "farfield_compare", Path(__file__).with_name("farfield-compare.py"))
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class MotionCaptureTests(unittest.TestCase):
    def load_fixture(self, directory, metadata):
        path = Path(directory) / "run-01.json"
        path.write_text(json.dumps({
            "gpu_timestamps_available": True,
            "gpu_timings_ms": [{"name": "GPU frame total", "milliseconds": 20}],
        }), encoding="utf-8")
        path.with_suffix(".meta.json").write_text(
            json.dumps(metadata), encoding="utf-8")
        return compare.load_arm("fixture", directory)

    def test_motion_readbacks_cannot_be_reported_as_performance(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(compare.BadCapture, "visual motion sequence"):
                self.load_fixture(directory, {"performance_comparable": False})

    def test_single_capture_remains_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            arm = self.load_fixture(directory, {"performance_comparable": True})
            self.assertEqual(len(arm.captures), 1)

    def test_legacy_metadata_remains_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            arm = self.load_fixture(directory, {})
            self.assertEqual(len(arm.captures), 1)


if __name__ == "__main__":
    unittest.main()
