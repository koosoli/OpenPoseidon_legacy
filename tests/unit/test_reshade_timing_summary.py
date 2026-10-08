"""Synthetic CPU receipt validation; these fixtures are not GPU measurements."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("summary", Path(__file__).parents[2] / "scripts/Summarize-ReShadeTiming.py")
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def receipt():
    windows = []
    for index, enabled in enumerate((False, True, True, False)):
        samples = [{"gpuMs": 10 + frame / 1000 if enabled else 0,
                    "presentMs": 20 if enabled else 10, "flags": 7 if enabled else 1}
                   for frame in range(120)]
        windows.append({"enabled": enabled, "timing": {"status": 5, "frames": 120,
            "recorded": 120, "generation": index + 1, "frequency": 1000000,
            "preview": 4, "completedGpu": 120 if enabled else 0, "samples": samples}})
    return {"captureSucceeded": True, "cleanTeardown": True, "measureGpu": True,
            "timingWindows": windows, "mode": "SceneDH", "msaaSamples": 4}


class TimingSummary(unittest.TestCase):
    def test_true_brackets_and_distinct_wall_scope(self):
        report = SUMMARY.summarize(receipt())
        self.assertIsNone(report["windows"][0]["gpu"])  # Absent OFF callbacks are not zero GPU cost.
        self.assertEqual(report["onGpu"]["count"], 240)
        self.assertAlmostEqual(report["onGpu"]["medianMs"], 10.0595)
        self.assertAlmostEqual(report["onGpu"]["p95Ms"], 10.113)
        self.assertEqual(report["onMinusOffMedianPresentMs"], 10)
        self.assertIn("not GPU busy", report["scope"])

    def test_incomplete_or_false_evidence_rejected(self):
        mutations = [
            lambda r: r.update(captureSucceeded=False),
            lambda r: r["timingWindows"][1]["timing"].update(completedGpu=119),
            lambda r: r["timingWindows"][1]["timing"].update(status=7),
            lambda r: r["timingWindows"][1]["timing"].update(preview=3),
            lambda r: r["timingWindows"][1]["timing"].update(generation=1),
            lambda r: r["timingWindows"][1]["timing"]["samples"][0].update(gpuMs=float("nan")),
            lambda r: r["timingWindows"][1]["timing"]["samples"][0].update(flags=1),
        ]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                data = receipt()
                mutate(data)
                with self.assertRaises(ValueError):
                    SUMMARY.summarize(data)


if __name__ == "__main__":
    unittest.main()
