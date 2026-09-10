import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "farfield_frametimes", Path(__file__).with_name("farfield-frametimes.py"))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class FrameTraceTests(unittest.TestCase):
    def test_gap_average_is_not_a_lower_bound_on_p99(self):
        exact = [0.001] * 99 + [1.0]
        averaged = [sum(exact) / len(exact)] * len(exact)
        original = bench.stats(exact)
        estimate = bench.stats(averaged)
        self.assertAlmostEqual(original['mean_ms'], estimate['mean_ms'])
        self.assertGreater(estimate['p99_ms'], original['p99_ms'])
        self.assertLess(estimate['worst_ms'], original['worst_ms'])

    def test_duplicate_ticks_do_not_create_frames_or_lose_wall_time(self):
        run = bench.Run()
        run.samples = [("M", 0.0, 0.0, 10), ("M", 0.01, 0.01, 10),
                       ("M", 0.02, 0.02, 11), ("M", 0.03, 0.03, 11),
                       ("M", 0.04, 0.04, 12)]
        bench.build_intervals(run, 0, True)
        values = bench.expand(run.intervals["M"])
        self.assertEqual(len(values), 2)
        self.assertAlmostEqual(sum(values), 0.04)
        self.assertAlmostEqual(bench.stats(values)["fps_throughput"], 50)
        self.assertEqual(run.coverage["M"], (2, 2))

    def test_skipped_frames_preserve_weight_and_time(self):
        run = bench.Run()
        run.samples = [("M", 0.0, 0.0, 10), ("M", 0.06, 0.06, 13)]
        bench.build_intervals(run, 0, True)
        self.assertEqual(bench.expand(run.intervals["M"]), [0.02] * 3)
        self.assertEqual(run.coverage["M"], (0, 1))

    def test_counter_reset_is_not_a_frame(self):
        run = bench.Run()
        run.samples = [("M", 0.0, 0.0, 20), ("M", 1.0, 1.0, 0),
                       ("M", 1.02, 1.02, 1)]
        bench.build_intervals(run, 0, True)
        values = bench.expand(run.intervals["M"])
        self.assertEqual(len(values), 1)
        self.assertAlmostEqual(values[0], 0.02)

    def test_both_residency_formats_keep_field_indices(self):
        for extra in ("", "desired=143 cells lead=100 m "):
            line = ("Modern object residency: centre=(80, -316) window=128 cells "
                    + extra + "requested=19996 resident=19000 created=10 released=5 refused=0")
            match = bench.RESIDENCY.search(line)
            self.assertIsNotNone(match)
            self.assertEqual(match.groups()[:7],
                             ("80", "-316", "128", "19996", "19000", "10", "5"))


if __name__ == "__main__":
    unittest.main()
