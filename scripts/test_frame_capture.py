import unittest
import runpy
from pathlib import Path
from frame_capture import parse_windows


def fixture():
    return [line for phase in ("static", "move") for line in (
        f'FFCAM phase={phase} t=1',
        'triPerfCapture begin n=3 dropped=0',
        'triPerfCapture offset=0 ms=1.25,280.5',
        'triPerfCapture offset=2 ms=30',
        'triPerfCapture end')]


class CaptureTests(unittest.TestCase):
    def test_report_keeps_millisecond_units(self):
        report = runpy.run_path(str(Path(__file__).with_name("farfield-frametimes.py")))
        row = report["capture_stats"]([1.25, 280.5, 30])
        self.assertEqual(row["median_ms"], 30)
        self.assertEqual(row["worst_ms"], 280.5)
        self.assertEqual(row["frames_over_100ms"], 1)
        self.assertEqual(row["frames_over_250ms"], 1)

    def test_complete(self):
        self.assertEqual(parse_windows(fixture()), {"S": [1.25, 280.5, 30], "M": [1.25, 280.5, 30]})

    def test_legacy_is_not_a_capture(self):
        self.assertEqual(parse_windows(['triPerfSeries n=3 ms=1,2,3']), {})

    def test_armed_but_missing_or_failed(self):
        for line in ('triPerfCapture armed limit=16384', 'triPerfCapture failed allocation'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_windows([line])

    def test_reject_corruption(self):
        for before, after in (("dropped=0", "dropped=1"), ("offset=2", "offset=1"),
                              ("n=3", "n=4"), ("280.5", "nan"), ("280.5", "-1"),
                              ("n=3", "n=16385")):
            with self.subTest(after=after), self.assertRaises(ValueError):
                parse_windows([line.replace(before, after) for line in fixture()])

    def test_reject_truncated_or_missing_phase(self):
        for cut in (1, 2, 3, 5):
            with self.subTest(cut=cut), self.assertRaises(ValueError):
                parse_windows(fixture()[:-cut])


if __name__ == "__main__":
    unittest.main()
