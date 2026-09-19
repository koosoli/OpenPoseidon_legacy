"""Pair fixed-FFT-time harness frames; the clip is visual evidence, not FPS data."""
import argparse
import bisect
import json
from pathlib import Path

import cv2
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--live", action="store_true", help="Pair live elapsed-time sequences; not phase-locked")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for view in (("live",) if args.live else ("shore", "elevated")):
        sequences = [json.loads((p / f"{view}-samples.json").read_text(encoding="utf-8-sig"))
                     for p in (args.before, args.after)]
        if args.live:
            # Screenshot readbacks can run slower than the requested cadence.
            # Resample recorded wall time so playback does not speed the ocean up.
            end = min(sequence[-1]["elapsed"] for sequence in sequences)
            times = np.arange(0.0, end, 0.125)
            resampled = []
            for sequence in sequences:
                stamps = [sample["elapsed"] for sample in sequence]
                resampled.append([sequence[min(bisect.bisect_left(stamps, float(t)), len(sequence)-1)] for t in times])
            sequences = resampled
        if len(sequences[0]) != len(sequences[1]):
            raise ValueError("Mismatched capture lengths")
        path = args.output / f"ocean-{view}-comparison.mp4"
        writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"mp4v"), 8, (1280, 396))
        if not writer.isOpened():
            raise RuntimeError("Video encoder unavailable")
        try:
            for left, right in zip(*sequences):
                if not args.live and abs(left["time"] - right["time"]) > 1e-5:
                    raise ValueError("Mismatched FFT times")
                frames = []
                for base, sample in ((args.before, left), (args.after, right)):
                    image = cv2.imread(str(base / sample["file"]))
                    if image is None:
                        raise ValueError(f"Missing {sample['file']}")
                    frames.append(cv2.resize(image, (640, 360), interpolation=cv2.INTER_AREA))
                frame = np.zeros((396, 1280, 3), dtype=np.uint8)
                frame[:360] = np.hstack(frames)
                label = (f"Surf disabled          Live elapsed={left['elapsed']:.2f}s (phase not locked)          Surf pilot"
                         if args.live else f"Before             FFT t={left['time']:.3f}s / 8 samples per second             Scale correction")
                cv2.putText(frame, label, (12, 381), cv2.FONT_HERSHEY_SIMPLEX, .47, (255, 255, 255), 1, cv2.LINE_AA)
                writer.write(frame)
        finally:
            writer.release()
        check = cv2.VideoCapture(str(path))
        count = int(check.get(cv2.CAP_PROP_FRAME_COUNT))
        check.release()
        if count != len(sequences[0]):
            raise RuntimeError("Encoded frame count differs")
        print(path)


if __name__ == "__main__":
    main()
