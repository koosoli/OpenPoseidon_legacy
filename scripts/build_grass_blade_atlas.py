#!/usr/bin/env python3
"""Build the near-LOD grass blade layers from a registered source sheet.

GRS-GATE-1 wants a *deterministic processing recipe*: the current blade images
were made by hand, so nothing can rebuild them and nothing can say which source
they came from. This is the replacement. Same input, same output, every time --
no randomness, no interactive step, fixed thresholds and a fixed ordering.

    python scripts/build_grass_blade_atlas.py
    python scripts/build_grass_blade_atlas.py --check   # verify without writing

What it does, and why each step is what it is:

The source is ~20 isolated leaf blades on a transparent sheet. The renderer wants
8 layers of 64x256, OPAQUE -- the blade silhouette is real geometry, so these
supply surface detail only and must not be alpha cutouts (see grass-plan.md).

So each blade is UNWRAPPED rather than cropped. A crop of a curved, tilted blade
carries background down its sides, and the background is dilated photo, so it
would appear as coloured fringing along a blade that should be a clean gradient.
Instead, for each row of the blade the mask's span is found and the albedo across
exactly that span is resampled to the layer width. A curved blade straightens,
which is correct: the geometry provides the curve at runtime.

Row 0 of the output is the blade TIP, because `blade_uv.y` is 0 at the tip and 1
at the root. Getting that backwards puts the dry tip colour at the ground.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_ID = "pbrpx-grass-species-4k"
# The eight ABI layers deliberately draw from several photographed PBRPX species
# instead of repeatedly unwrapping one leaf sheet. These local inputs are covered
# by source-registry entries and remain outside the tracked source tree.
SOURCES = (
    "PX_Echinochloa_crus_galli_Leaf_01",
    "PX_Acorus_calamus_Atlas_02",
    "PX_Nassella_tenuissima_Atlas_13",
    "PX_Pennisetum_alopecuroides_Leaf_06",
    "PX_Zoysia_japonica_Atlas_04",
)
SOURCE_ROOT = ROOT / "assets" / "grass" / "more_grass"
OUT_DIR = ROOT / "assets" / "grass"

# Bumping this invalidates every derived hash on purpose: it is the "processing
# version" ASSET-010 asks for, so a recipe change is visible as a provenance
# change rather than as an unexplained image diff.
RECIPE_VERSION = 5

LAYER_W, LAYER_H, LAYERS = 64, 256, 8

# A blade must be tall enough to be worth a 256-row layer, and solid enough not to
# be a fragment. Both are in source pixels at 1024x1024.
# Sources are reduced 4x before component analysis, so these retain the original
# 4K thresholds in source-space terms (380/4 pixels tall, 3000/16 pixels area).
MIN_HEIGHT = 95
MIN_AREA = 188
# Rows where the mask has more than one run mean two blades crossed and merged
# into one component; unwrapping that would blend two blades together.
MAX_MULTI_RUN_FRACTION = 0.08
ALPHA_THRESHOLD = 128


def label_components(mask: np.ndarray) -> tuple[np.ndarray, int]:
    """Two-pass connected-component labelling, 4-connected, no scipy dependency."""
    height, width = mask.shape
    labels = np.zeros((height, width), dtype=np.int32)
    parent: list[int] = [0]

    def find(x: int) -> int:
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a: int, b: int) -> None:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    for y in range(height):
        row, above = mask[y], mask[y - 1] if y else None
        for x in range(width):
            if not row[x]:
                continue
            left = labels[y, x - 1] if x and row[x - 1] else 0
            up = labels[y - 1, x] if y and above[x] else 0
            if left and up:
                labels[y, x] = min(left, up)
                union(left, up)
            elif left or up:
                labels[y, x] = left or up
            else:
                parent.append(len(parent))
                labels[y, x] = len(parent) - 1

    remap = {0: 0}
    for old in range(1, len(parent)):
        root = find(old)
        remap.setdefault(root, len(remap))
    flat = labels.ravel()
    out = np.array([remap.get(find(v), 0) if v else 0 for v in range(len(parent))], dtype=np.int32)
    labels = out[flat].reshape(labels.shape)
    return labels, int(labels.max())


def row_span(row_mask: np.ndarray) -> tuple[int, int, int] | None:
    """Leftmost and rightmost set texel of a row, plus how many separate runs."""
    xs = np.flatnonzero(row_mask)
    if xs.size == 0:
        return None
    runs = 1 + int(np.count_nonzero(np.diff(xs) > 1))
    return int(xs[0]), int(xs[-1]), runs


def unwrap(albedo: np.ndarray, mask: np.ndarray, bbox: tuple[int, int, int, int]) -> np.ndarray:
    """Resample one blade into LAYER_W x LAYER_H x RGBA, tip-first.

    The colour is trimmed to the mask, but the ALPHA is not: it records how much
    of the layer's width the blade actually occupied at that height, centred. The
    solid-geometry path ignores alpha entirely, while the alpha-card path uses it
    as the silhouette -- so one set of images serves both, and the card path gets a
    real tapering blade outline instead of a rectangle.
    """
    y0, y1, _, _ = bbox
    spans = [row_span(mask[y]) for y in range(y0, y1 + 1)]
    widths = np.array([0.0 if s is None else float(s[1] - s[0] + 1) for s in spans])
    # SMOOTH the width profile before using it as a silhouette. Raw per-row width
    # follows the photograph's own noise -- a blade that is foreshortened, curled
    # or partly occluded fluctuates several texels row to row -- and cutting the
    # card to that produced lumpy leaf shapes rather than blades. A wide moving
    # average keeps the real taper and discards the wobble.
    window = max(9, (y1 - y0 + 1) // 12)
    if window % 2 == 0:
        window += 1
    kernel = np.ones(window) / window
    padded = np.pad(widths, window // 2, mode="edge")
    smooth = np.convolve(padded, kernel, mode="valid")[: widths.size]
    # Force the profile to widen monotonically from tip to root. Measured width
    # alone gives a LEAF -- widest in the middle, tapering at both ends -- because
    # that is what a photographed leaf lying at an angle measures as. A grass
    # blade is widest where it leaves the ground and narrows all the way to the
    # tip, so a running maximum from row 0 (the tip) downward turns the measured
    # shape into a believable blade while keeping each source blade's own rate of
    # taper. Without this the alpha-card path draws leaves standing on end.
    smooth = np.maximum.accumulate(smooth)
    widest = max(1.0, float(smooth.max()))
    rows: list[np.ndarray] = []
    for offset, y in enumerate(range(y0, y1 + 1)):
        span = spans[offset]
        if span is None:
            continue
        left, right, _ = span
        if right - left < 1:
            continue
        # Sample strictly inside the span: the outermost texel of a photographed
        # blade is a partially covered edge pixel that has already been blended
        # with the dilated background.
        centres = np.linspace(left + 0.5, right - 0.5, LAYER_W)
        idx = np.clip(np.rint(centres).astype(np.int32), 0, albedo.shape[1] - 1)
        colour = albedo[y, idx]
        # Alpha: this row's share of the widest row, centred in the layer. A
        # narrow tip therefore occupies a narrow band of the card, which is what
        # gives the cut-out its taper.
        share = float(smooth[offset]) / widest
        half = 0.5 * share * LAYER_W
        centre = 0.5 * LAYER_W
        columns = np.arange(LAYER_W) + 0.5
        # Soften by one texel so the cutoff has a gradient to bite into instead
        # of a hard step that would alias along the whole edge.
        alpha = np.clip((half - np.abs(columns - centre)) + 0.5, 0.0, 1.0) * 255.0
        rows.append(np.dstack([colour[None], alpha[None, :, None]])[0])
    if not rows:
        raise ValueError("blade had no usable rows")
    stacked = np.stack(rows)  # (n, LAYER_W, 4), row 0 is the topmost = the tip
    src_y = np.linspace(0, stacked.shape[0] - 1, LAYER_H)
    lower = np.floor(src_y).astype(np.int32)
    upper = np.clip(lower + 1, 0, stacked.shape[0] - 1)
    frac = (src_y - lower)[:, None, None]
    return (stacked[lower] * (1.0 - frac) + stacked[upper] * frac).astype(np.uint8)


def source_candidates(product: str) -> list[tuple[int, int, int, int, int, np.ndarray, np.ndarray]]:
    source_dir = SOURCE_ROOT / f"{product}_4K"
    albedo_path = source_dir / f"{product}_albedo.jpg"
    opacity_path = source_dir / f"{product}_opacity.jpg"
    for path in (albedo_path, opacity_path):
        if not path.is_file():
            raise SystemExit(f"missing registered source: {path.relative_to(ROOT)}")

    # Final layers are 64x256. Reducing a 4K source to 1024 before the Python
    # connected-component pass preserves the blade topology at that target while
    # avoiding an impractical 16-million-pixel scalar walk per source.
    albedo_image = Image.open(albedo_path).convert("RGB")
    opacity_image = Image.open(opacity_path).convert("L")
    albedo_image.thumbnail((1024, 1024), Image.Resampling.LANCZOS)
    opacity_image.thumbnail((1024, 1024), Image.Resampling.LANCZOS)
    albedo = np.asarray(albedo_image)
    opacity = np.asarray(opacity_image)
    labels, count = label_components(opacity >= ALPHA_THRESHOLD)
    candidates: list[tuple[int, int, int, int, int, np.ndarray, np.ndarray]] = []
    for label in range(1, count + 1):
        ys, xs = np.nonzero(labels == label)
        if ys.size < MIN_AREA:
            continue
        y0, y1 = int(ys.min()), int(ys.max())
        if y1 - y0 < MIN_HEIGHT:
            continue
        blade_mask = labels == label
        multi = sum(
            1
            for y in range(y0, y1 + 1)
            if (span := row_span(blade_mask[y])) is not None and span[2] > 1
        )
        if multi / max(1, y1 - y0 + 1) > MAX_MULTI_RUN_FRACTION:
            continue  # two blades merged into one component
        candidates.append((int(ys.size), y0, y1, int(xs.min()), int(xs.max()), blade_mask, albedo))
    candidates.sort(key=lambda c: (-c[0], c[3]))
    return candidates


def build() -> list[tuple[str, bytes]]:
    # Take one distinctive, largest valid blade from each species first. The
    # remaining two layers come from the largest second candidates, preserving a
    # mixed field even if a source sheet contains many near-identical blades.
    per_source = [source_candidates(product) for product in SOURCES]
    per_source = [candidates for candidates in per_source if candidates]
    selected = [candidates[0] for candidates in per_source]
    for candidates in per_source:
        if len(selected) >= LAYERS:
            break
        if len(candidates) > 1:
            selected.append(candidates[1])

    if len(selected) < LAYERS:
        raise SystemExit(
            f"only {len(selected)} usable blades found, need {LAYERS}."
        )

    outputs: list[tuple[str, bytes]] = []
    for layer, (_, y0, y1, x0, x1, blade_mask, albedo) in enumerate(selected[:LAYERS]):
        rgba = unwrap(albedo, blade_mask, (y0, y1, x0, x1))
        image = Image.fromarray(rgba)
        from io import BytesIO

        buffer = BytesIO()
        # Deterministic bytes: no timestamp chunk, fixed compression.
        image.save(buffer, format="PNG", optimize=False, compress_level=6)
        outputs.append((f"meadow-grass-blade-{layer}.png", buffer.getvalue()))
    return outputs


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args(argv)

    outputs = build()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"recipe v{RECIPE_VERSION}, source {SOURCE_ID}")
    drift = False
    for name, data in outputs:
        path = OUT_DIR / name
        digest = hashlib.sha256(data).hexdigest()
        if args.check:
            current = path.read_bytes() if path.is_file() else b""
            state = "ok" if current == data else "DRIFT"
            drift = drift or state == "DRIFT"
        else:
            path.write_bytes(data)
            state = "written"
        print(f"  {name:<28} {len(data):7} bytes  {digest[:16]}  {state}")
    if args.check and drift:
        print("derived blades differ from the recipe output", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
