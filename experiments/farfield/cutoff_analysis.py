#!/usr/bin/env python3
"""Dial in the far tier's start rule against a real world, offline.

The far tier draws a cheap proxy for a placement the real renderer is not drawing.
Getting its start rule right is not a matter of taste, it is a matter of two failure
modes that a screenshot shows only as "looks a bit off":

  OVERLAP -- a proxy is drawn where the real object is ALSO drawn.  A crude white box
             competing with the real model.
  GAP     -- neither is drawn.  The object is simply missing, and this is where the
             reported pop-in lives: it appears the moment one of the two starts.

The engine culls real objects by a SIZE-dependent test, not by distance
(gfx3d/cull.wgsl):

    drop when  diameter^2 < (pixel_limit * lod_scale)^2 * dist^2 * lod_inv_width^2

so a 30 m building survives to the fog edge while a 1 m rock vanishes at a few hundred
metres -- and `lod_inv_width` is driven by a framerate feedback loop, so that boundary
MOVES.  Any single cutoff distance is therefore wrong in both directions at once, and
this script measures by how much.

It needs no engine build and no running game: it reads the placement dump and the model
bounds straight out of the shipped P3Ds.

    python cutoff_analysis.py --placements everon.bin --models "<game>/reforger" \\
                              [--width 1920] [--fov 55] [--fog 880]
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
from collections import defaultdict

# ---------------------------------------------------------------- MLOD bounds

MLOD_HEADER = 12  # "MLOD", major u8, minor u8, pad u16, lodCount u32
LOD_HEADER = 28  # sig, major, minor, nPos, nNorm, nFace, flags
VERTEX_STRIDE = 16  # Vector3 + int32 flags


def mlod_bounds(path: str):
    """(radius, minY, maxY, horizontal_extent) from the FIRST LOD's vertex block.

    Only the first LOD is read, and only its positions -- no faces, normals, UVs or
    TAGGs. That is the whole point: a world has hundreds of distinct models against
    millions of placements, so this must stay a per-model cost.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(MLOD_HEADER)
            if len(head) < MLOD_HEADER or head[:4] != b"MLOD":
                return None
            lod_head = f.read(LOD_HEADER)
            if len(lod_head) < LOD_HEADER:
                return None
            sig = lod_head[:4]
            if sig not in (b"P3DM", b"SP3X"):
                return None
            n_pos = struct.unpack_from("<I", lod_head, 12)[0]
            if n_pos == 0 or n_pos > 4_000_000:
                return None
            raw = f.read(n_pos * VERTEX_STRIDE)
            if len(raw) < n_pos * VERTEX_STRIDE:
                return None
    except OSError:
        return None

    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    r2 = 0.0
    for i in range(n_pos):
        x, y, z = struct.unpack_from("<fff", raw, i * VERTEX_STRIDE)
        if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
            continue
        for k, v in enumerate((x, y, z)):
            if v < lo[k]:
                lo[k] = v
            if v > hi[k]:
                hi[k] = v
        r2 = max(r2, x * x + y * y + z * z)
    if lo[0] is math.inf:
        return None
    return (math.sqrt(r2), lo[1], hi[1], max(hi[0] - lo[0], hi[2] - lo[2]))


# ---------------------------------------------------------------- placements


def load_placements(path: str):
    with open(path, "rb") as f:
        blob = f.read()
    n = struct.unpack_from("<I", blob, 0)[0]
    out = []
    for i in range(n):
        o = 4 + i * 13
        x, y, z = struct.unpack_from("<fff", blob, o)
        out.append((x, y, z, blob[o + 12]))
    return out


# ---------------------------------------------------------------- the rules


def real_drawn(dist: float, diameter: float, k: float, draw_distance: float) -> bool:
    """Would the engine draw the REAL object at this distance?

    Mirrors cull.wgsl: a radial distance cut, then the sub-pixel test. `k` folds
    pixel_limit * lod_scale * lod_inv_width into one coefficient, so sweeping k is
    sweeping the LOD governor across its range.
    """
    if dist > draw_distance:
        return False
    return diameter >= k * dist


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--placements", required=True, help="binary from mkplacements.py")
    ap.add_argument("--models", required=True, help="root directory of shipped .p3d files")
    ap.add_argument("--fog", type=float, default=880.0, help="object draw distance / fog edge (m)")
    ap.add_argument("--sample", type=int, default=120000, help="placements to sample (0 = all)")
    args = ap.parse_args()

    placements = load_placements(args.placements)
    print(f"placements: {len(placements):,}")

    # Bounds per model. The dump carries no model name, so approximate with a size
    # distribution measured from the real P3Ds and assigned by class -- stated plainly
    # because it is the one approximation in this script.
    by_class = defaultdict(list)
    scanned = 0
    for root, _dirs, files in os.walk(args.models):
        for name in files:
            if not name.lower().endswith(".p3d"):
                continue
            b = mlod_bounds(os.path.join(root, name))
            if not b:
                continue
            scanned += 1
            low = os.path.join(root, name).lower().replace("\\", "/")
            cls = 0 if ("/vegetation/" in low or "/tree" in low or "/bush" in low) else (
                1 if ("/structures/" in low or "/buildings/" in low) else 2)
            by_class[cls].append(b)
    print(f"models read: {scanned}")
    if scanned == 0:
        print("no models found -- check --models", file=sys.stderr)
        return 2

    for cls, label in ((0, "vegetation"), (1, "structures"), (2, "other")):
        vals = sorted(b[3] for b in by_class.get(cls, []))
        if vals:
            med = vals[len(vals) // 2]
            print(f"  {label:<11} n={len(vals):<5} horizontal extent  "
                  f"min {vals[0]:6.1f}  median {med:6.1f}  max {vals[-1]:7.1f} m")

    # Sample placements around the map centre, using a median size per class.
    size = {c: (sorted(b[3] for b in v)[len(v) // 2] if v else 4.0) for c, v in by_class.items()}
    xs = [p[0] for p in placements]
    zs = [p[2] for p in placements]
    cam = ((min(xs) + max(xs)) * 0.5, (min(zs) + max(zs)) * 0.5)

    sample = placements if args.sample <= 0 else placements[:: max(1, len(placements) // args.sample)]
    print(f"\nsampled {len(sample):,} placements around map centre, fog edge {args.fog:.0f} m")

    # k folds pixel_limit * lod_scale * lod_inv_width. The governor's fine and coarse
    # rails differ by the governor range, which is why both are swept.
    print("\nOVERLAP = proxy drawn where the real object is too (crude box over real model)")
    print("GAP     = neither drawn (the object is missing -- this is where pop-in lives)\n")
    print(f"{'governor':<10} {'rule':<22} {'overlap':>9} {'gap':>9} {'proxy-only':>11}")

    for rail, k in (("fine", 0.125 * 1.13 * 0.0104), ("coarse", 0.125 * 1.13 * 0.0104 * 4)):
        rules = [
            ("fixed 150 m", ("fixed", 150.0)),
            ("fixed 396 m (0.45x)", ("fixed", 396.0)),
            ("fixed 880 m (1.0x)", ("fixed", args.fog)),
            ("complement of cull", ("complement", 0.0)),
        ]
        for label, (kind, param) in rules:
            overlap = gap = proxy_only = 0
            for (x, _y, z, cls) in sample:
                d = math.hypot(x - cam[0], z - cam[1])
                if d > args.fog:
                    continue  # beyond fog: nothing to argue about
                diameter = size.get(cls, 4.0)
                real = real_drawn(d, diameter, k, args.fog)
                far = d >= param if kind == "fixed" else not real
                if real and far:
                    overlap += 1
                elif not real and not far:
                    gap += 1
                elif far:
                    proxy_only += 1
            total = max(1, len([1 for (x, _y, z, _c) in sample
                                if math.hypot(x - cam[0], z - cam[1]) <= args.fog]))
            print(f"{rail:<10} {label:<22} {overlap * 100.0 / total:>8.1f}% "
                  f"{gap * 100.0 / total:>8.1f}% {proxy_only * 100.0 / total:>10.1f}%")
    print("\nA rule with 0% overlap and 0% gap is the complement of the real cull by")
    print("construction. Any fixed distance trades one against the other, and the trade")
    print("moves whenever the framerate governor moves.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
