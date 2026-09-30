"""Diagnostic scheduled-image deltas, including wind motion and capture timing.

No threshold in this script establishes adjacent-rendered-frame flicker or
visual acceptance. Requires NumPy and Pillow; captures retain their provenance.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--captures", type=Path, default=Path(__file__).resolve().parent.parent / "build/grass-stability")
    args = parser.parse_args()
    regions = {"OwnerEveron": (400, 250, 900, 600), "Aerial": (160, 120, 1120, 640), "Close": (160, 360, 1120, 640)}
    rows, excluded = [], []
    for folder in sorted(args.captures.iterdir()):
        if not folder.is_dir():
            continue
        try:
            if not json.loads((folder / "result.json").read_text(encoding="utf-8-sig")).get("passed"):
                raise ValueError("Capture did not pass")
            provenance = json.loads((folder / "provenance.json").read_text(encoding="utf-8-sig"))
            metrics = json.loads((folder / "metrics.json").read_text(encoding="utf-8-sig"))
            grass = metrics["grass"]
            if grass["near_instances"] + grass["mid_instances"] <= 0:
                raise ValueError("No near/mid grass")
            roi = regions[provenance["view"]]
            x0, y0, x1, y1 = roi
            images = []
            for path in sorted(folder.glob("frame-*.png")):
                with Image.open(path) as im:
                    if im.size != (1280, 720):
                        raise ValueError("Fixed ROI requires the 1280x720 capture profile")
                    images.append(np.asarray(im.convert("RGB"), dtype=np.int16)[y0:y1, x0:x1])
            if len(images) != 6:
                raise ValueError("Expected six scheduled captures")
            crops = np.stack(images)
            delta = np.max(np.abs(np.diff(crops, axis=0)), axis=-1)
            rows.append(dict(run=folder.name, roi=roi, filter=provenance["filter"],
                installed=provenance["installed"], build=metrics["build"], grass=grass,
                mean_luma=float(np.sum(crops * [0.2126, 0.7152, 0.0722], axis=-1).mean()),
                mean_max_channel_delta=float(delta.mean()),
                above24_percent=float(np.mean(delta > 24) * 100),
                above48_percent=float(np.mean(delta > 48) * 100)))
        except (OSError, KeyError, ValueError) as error:
            excluded.append(dict(run=folder.name, reason=str(error)))
    output = dict(scope="Five scheduled-image deltas; includes coherent motion and capture timing. Not adjacent-rendered-frame flicker proof.", rows=rows, excluded=excluded)
    target = args.captures / "scores.json"
    target.write_text(json.dumps(output, indent=2), encoding="utf-8")
    for row in rows:
        print(f"{row['run']}: luma {row['mean_luma']:.3f}, delta {row['mean_max_channel_delta']:.3f}, >48 {row['above48_percent']:.4f}%")
    print(f"{len(rows)} included; {len(excluded)} excluded. {target}")


if __name__ == "__main__":
    main()
