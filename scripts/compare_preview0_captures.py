#!/usr/bin/env python3
"""Create a reproducible, deliberately simple PNG comparison bundle."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
from typing import Any

from PIL import Image, ImageChops, ImageEnhance


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_grid(text: str) -> tuple[int, int]:
    rows, _, cols = text.lower().partition("x")
    try:
        parsed = (int(rows), int(cols))
    except ValueError as error:
        raise SystemExit(f"--regions must look like ROWSxCOLS, got {text!r}") from error
    if parsed[0] < 1 or parsed[1] < 1:
        raise SystemExit("--regions rows and columns must both be >= 1")
    return parsed


def region_report(
    rgb_deltas: list[float], width: int, height: int, rows: int, cols: int, threshold: int
) -> dict[str, Any]:
    """Per-tile changed-pixel ratios, plus the worst tile.

    The whole-image scalars have no useful scale on their own: a 200x200 object
    that vanished entirely scores changed_pixel_ratio 0.027, while an *expected*
    GL33-vs-WGPU backend difference on the same scene scores 0.55. A reader with
    only the global numbers would dismiss the real regression and investigate the
    non-bug. Where the difference sits is the part that discriminates, and it has
    to be in the report rather than only in the mask PNG, or the report cannot be
    acted on without a human looking at an image.
    """
    tiles: list[dict[str, Any]] = []
    for row in range(rows):
        y0, y1 = row * height // rows, (row + 1) * height // rows
        for col in range(cols):
            x0, x1 = col * width // cols, (col + 1) * width // cols
            count = 0
            total = 0.0
            for y in range(y0, y1):
                base = y * width
                for delta in rgb_deltas[base + x0 : base + x1]:
                    total += delta
                    if delta > threshold:
                        count += 1
            pixels = (y1 - y0) * (x1 - x0)
            tiles.append(
                {
                    "row": row,
                    "col": col,
                    "box": [x0, y0, x1, y1],
                    "changed_pixel_ratio": count / pixels if pixels else 0.0,
                    "mean_absolute_rgb_delta": total / pixels if pixels else 0.0,
                }
            )
    peak = max(tiles, key=lambda tile: tile["changed_pixel_ratio"]) if tiles else None
    return {"rows": rows, "cols": cols, "tiles": tiles, "peak": peak}


def load_acceptance_profile(path: pathlib.Path) -> dict[str, Any]:
    try:
        profile = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read acceptance profile {path}: {error}") from error
    if not isinstance(profile, dict) or profile.get("schema_version") != 1:
        raise SystemExit("acceptance profile must be a JSON object with schema_version 1")
    policy = profile.get("policy")
    if policy not in {"review-required", "thresholds", "correctness-review"}:
        raise SystemExit("acceptance profile policy must be review-required, thresholds, or correctness-review")
    if policy == "thresholds":
        thresholds = profile.get("thresholds")
        if not isinstance(thresholds, dict):
            raise SystemExit("thresholds policy requires a thresholds object")
        for key in ("max_changed_pixel_ratio", "max_mean_absolute_rgb_delta"):
            value = thresholds.get(key)
            if not isinstance(value, (int, float)) or value < 0:
                raise SystemExit(f"thresholds.{key} must be a non-negative number")
    return profile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True)
    parser.add_argument("--mask", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--threshold", type=int, default=12,
                        help="per-channel difference that counts as changed (0..255)")
    parser.add_argument("--regions", default="4x4",
                        help="ROWSxCOLS grid of per-tile ratios in the report; 1x1 to disable")
    parser.add_argument("--acceptance-profile", type=pathlib.Path,
                        help="optional JSON policy; supports thresholds, pending review, or correctness review")
    parser.add_argument("--require-accepted", action="store_true",
                        help="return non-zero unless the supplied acceptance profile accepts this comparison")
    args = parser.parse_args()
    if not 0 <= args.threshold <= 255:
        raise SystemExit("threshold must be in 0..255")
    grid_rows, grid_cols = parse_grid(args.regions)
    with Image.open(args.reference) as reference_image, Image.open(args.candidate) as candidate_image:
        reference = reference_image.convert("RGBA")
        candidate = candidate_image.convert("RGBA")
    if reference.size != candidate.size:
        raise SystemExit(f"capture dimensions differ: {reference.size} vs {candidate.size}")

    difference = ImageChops.difference(reference, candidate)
    channels = list(difference.getdata())
    rgb_deltas = [sum(pixel[:3]) / 3.0 for pixel in channels]
    changed = sum(delta > args.threshold for delta in rgb_deltas)
    # Amplify the RGB difference only; alpha is opaque in normal game captures.
    mask = ImageEnhance.Brightness(difference.convert("RGB")).enhance(4.0)
    args.mask.parent.mkdir(parents=True, exist_ok=True)
    mask.save(args.mask)
    changed_pixel_ratio = changed / (reference.width * reference.height)
    mean_absolute_rgb_delta = sum(rgb_deltas) / len(rgb_deltas)
    result: dict[str, Any] = {
        "schema_version": 1,
        "reference": {"path": str(args.reference), "sha256": sha256(args.reference)},
        "candidate": {"path": str(args.candidate), "sha256": sha256(args.candidate)},
        "dimensions": {"width": reference.width, "height": reference.height},
        "threshold": args.threshold,
        "changed_pixels": changed,
        "changed_pixel_ratio": changed_pixel_ratio,
        "mean_absolute_rgb_delta": mean_absolute_rgb_delta,
        "max_absolute_rgb_delta": max(rgb_deltas),
        "regions": region_report(
            rgb_deltas, reference.width, reference.height, grid_rows, grid_cols, args.threshold
        ),
        "mask": {"path": str(args.mask), "sha256": sha256(args.mask)},
    }
    accepted = None
    if args.acceptance_profile:
        profile = load_acceptance_profile(args.acceptance_profile)
        policy = profile["policy"]
        if policy == "review-required":
            acceptance = {
                "profile": str(args.acceptance_profile),
                "policy": policy,
                "verdict": "REVIEW_REQUIRED",
                "reason": profile.get("reason", "owner visual review is required"),
            }
            accepted = False
        elif policy == "correctness-review":
            classification = profile.get("classification", {})
            review = profile.get("review")
            allowed_non_blocking = (
                classification.get("non_blocking", [])
                if isinstance(classification, dict)
                else []
            )
            review_matches_capture = (
                isinstance(review, dict)
                and review.get("reference_sha256") == result["reference"]["sha256"]
                and review.get("candidate_sha256") == result["candidate"]["sha256"]
                and isinstance(review.get("reviewer"), str)
                and bool(review["reviewer"].strip())
                and isinstance(review.get("approved_at"), str)
                and bool(review["approved_at"].strip())
            )
            review_classification = review.get("classification") if isinstance(review, dict) else None
            approved_non_blocking = (
                isinstance(review_classification, str)
                and review_classification in allowed_non_blocking
            )
            accepted = review_matches_capture and approved_non_blocking
            acceptance = {
                "profile": str(args.acceptance_profile),
                "policy": policy,
                "verdict": "ACCEPTED_EXPECTED_DIFFERENCE" if accepted else "REVIEW_REQUIRED",
                "reason": profile.get("reason", "comparison requires an explicit correctness review"),
                "classification": classification,
                "review": review if isinstance(review, dict) else None,
                "review_matches_capture": review_matches_capture,
                "review_classification_allowed": approved_non_blocking,
            }
        else:
            thresholds = profile["thresholds"]
            changed_ok = changed_pixel_ratio <= thresholds["max_changed_pixel_ratio"]
            mean_ok = mean_absolute_rgb_delta <= thresholds["max_mean_absolute_rgb_delta"]
            accepted = changed_ok and mean_ok
            acceptance = {
                "profile": str(args.acceptance_profile),
                "policy": policy,
                "verdict": "ACCEPTED" if accepted else "REJECTED",
                "thresholds": thresholds,
                "checks": {
                    "changed_pixel_ratio": changed_ok,
                    "mean_absolute_rgb_delta": mean_ok,
                },
            }
        result["acceptance"] = acceptance
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(args.out)
    if args.require_accepted and accepted is not True:
        raise SystemExit("comparison was not accepted by its profile")


if __name__ == "__main__":
    main()
