"""Denser hedges for OFP's krovi* models (owner, 2026-10-13: "do better hedges ... not cartoony ... work
well in different lighting ... sensitive to wind direction"). Build-time only; writes MLOD P3D models
and the redirect manifest the engine reads (ModelCache ModelRedirect, op_ground_materials\\models.json).

The stock hedges are 12-38 faces: three to six large crossed photo cards. Here each is a volume of
small crossed "clump" cards built from the same photographed bush images (referenced by their stock
paths, not copied), spread over a rounded hedge body (a dome in cross-section, rounded ends) and
packed toward its skin, which is what reads as foliage instead of a billboard:

* normals follow the BODY, not the card: each vertex takes the hedge body's outward normal at its
  position, with an upward bias -- a card lit by its own flat normal goes black or white as the sun
  moves (the cartoon look); lit by the volume's normal it shades as one rounded mass;
* the deeper a clump sits, the more its normal turns down and inward, so the interior reads darker
  under any light (OFP models carry no vertex colour or AO channel);
* cards lean out from the body and are tilted at random, so no two rows line up.

Wind: the hedges stay `map=bush` (the geometry LOD), so the renderer's bush sway -- which follows the
wind's direction -- moves them as it moved the old ones. Collision: our own convex box per hedge in
the measured extent of the stock geometry LOD (no original geometry is copied); `dammage=tent` keeps
their crushable behaviour. Extents: measured from the stock models (PoseidonFormats, 2026-10-13).
"""

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path

import numpy as np

# stock extents (centred model space, as the ODOL stores them): visual LOD 0 and the geometry LOD,
# LOD 0's resolution, and the stock images each hedge is built from (w/h aspect)
HEDGES = {
    "krovi": dict(visual=((-2.69, -0.9, -1.52), (2.65, 0.9, 1.52)), geometry=((-2.13, -0.9, -0.63), (2.13, 0.3, 0.6)),
                  res=0.5, textures=[("data\\maliny_opak.pac", 1.0), ("data\\krovi1.pac", 2.0), ("data\\kerik_bobul_02.pac", 1.0)]),
    "krovi2": dict(visual=((-2.95, -1.13, -1.68), (2.95, 1.06, 1.68)), geometry=((-2.26, -1.12, -0.38), (2.38, -0.1, 0.85)),
                   res=0.5, textures=[("data\\krovi6.pac", 1.0), ("data\\kerik_bobul_02.pac", 1.0), ("data\\krovi7.pac", 1.0)]),
    "krovi3": dict(visual=((-3.01, -1.13, -1.63), (3.03, 1.16, 1.63)), geometry=((-2.75, -1.12, -0.33), (2.33, -0.1, 0.9)),
                   res=0.5, textures=[("data\\krovi6.pac", 1.0), ("data\\krovi7.pac", 1.0), ("data\\kerik_bobul_02.pac", 1.0)]),
    "krovi4": dict(visual=((-2.66, -1.09, -1.56), (2.66, 1.13, 1.56)), geometry=((-2.24, -1.1, -0.63), (2.32, 0.1, 0.58)),
                   res=0.0, textures=[("data\\kerik_bobul_02.pac", 1.0), ("data\\krovi6.pac", 1.0)]),
    "krovi_bigest": dict(visual=((-2.95, -1.47, -2.09), (2.97, 1.42, 2.09)), geometry=((-2.89, -1.47, -1.3), (2.84, 0.74, 1.32)),
                         res=0.1, textures=[("data\\krovi6.pac", 1.0), ("data\\krovi7.pac", 1.0)]),
    "krovi_long": dict(visual=((-4.08, -1.36, -1.18), (4.08, 1.51, 1.1)), geometry=((-3.75, -1.28, -0.73), (3.76, 0.18, 0.7)),
                       res=0.3, textures=[("data\\krovi4_start.pac", 1.0), ("data\\kerik_bobul_02.pac", 1.0), ("data\\krovi6.pac", 1.0)]),
}
# LOD resolutions after LOD 0, and the share of clumps (and their scale) each keeps
LODS = [(None, 1.0, 1.0), (2.0, 0.4, 1.35), (5.0, 0.15, 1.8)]
RES_GEOMETRY = 1e13
RES_SHADOW = 6e15
DENSITY = 0.32  # clump cards per (card size)^2 of hedge skin


def body(spec):
    (vx0, vy0, vz0), (vx1, vy1, vz1) = spec["visual"]
    (gx0, _, gz0), (gx1, _, gz1) = spec["geometry"]
    a = (vx1 - vx0) / 2 * 0.94
    cx = (vx0 + vx1) / 2
    c = min(max(abs(gz0), abs(gz1)) * 1.35, (vz1 - vz0) / 2 * 0.9)
    return dict(cx=cx, a=a, c=c, y0=vy0, h=(vy1 - vy0) * 0.92, box=spec["visual"])


def outward(b, p):
    """The hedge body's outward normal at p (a dome in cross-section, rounded ends), biased up."""
    x = (p[..., 0] - b["cx"]) / b["a"]
    y = np.maximum(p[..., 1] - b["y0"], 0) / b["h"]
    z = p[..., 2] / b["c"]
    g = np.stack([6 * np.sign(x) * np.abs(x) ** 5 / b["a"], 2.5 * y ** 1.5 / b["h"], 2.5 * np.sign(z) * np.abs(z) ** 1.5 / b["c"]], -1)
    g = g / np.maximum(np.linalg.norm(g, axis=-1, keepdims=True), 1e-6)
    g[..., 1] += 0.35
    return g / np.linalg.norm(g, axis=-1, keepdims=True)


def clumps(spec, seed):
    rng = np.random.default_rng(seed)
    b = body(spec)
    size = min(b["h"], 2 * b["c"]) * 0.5
    skin = 2 * b["a"] * math.pi * (b["h"] + b["c"]) / 2
    n = int(skin / (size * size) / DENSITY)
    out = []
    for _ in range(n):
        x = rng.uniform(-1, 1)
        th = rng.uniform(0, math.pi)
        end = (1 - abs(x) ** 6) ** (1 / 6)
        depth = 0.62 + 0.38 * math.sqrt(rng.uniform())  # toward the skin
        cz, cy = math.cos(th), math.sin(th)
        sz = np.sign(cz) * abs(cz) ** 0.8 * b["c"] * end * depth
        sy = abs(cy) ** 0.8 * b["h"] * end * depth
        p = np.array([b["cx"] + x * b["a"] * depth ** 0.3, b["y0"] + sy, sz])
        out.append(dict(p=p, depth=depth, size=size * rng.uniform(0.75, 1.25), yaw=rng.uniform(0, math.pi),
                        tilt=rng.uniform(-0.25, 0.25), tex=int(rng.integers(len(spec["textures"]))), keep=rng.uniform()))
    return b, out


def visual_mesh(spec, b, cl, share, scale):
    """Two crossed quads a clump, both faces (each its own points), normals from the body."""
    pts, nrm, faces = [], [], []
    (bx0, by0, bz0), (bx1, by1, bz1) = b["box"]
    for k in cl:
        if k["keep"] >= share:
            continue
        tex, aspect = spec["textures"][k["tex"]]
        s = k["size"] * scale
        hw, hh = s * 0.5 * min(aspect, 1.6) * 0.95, s * 0.45
        centre = k["p"].copy()
        centre[1] = max(centre[1], by0 + hh * 0.9)  # stands on the ground, not in it
        out = outward(b, centre)
        for q in range(2):
            yaw = k["yaw"] + q * math.pi / 2
            right = np.array([math.cos(yaw), 0.0, math.sin(yaw)])
            # lean the card out from the body and tilt it a little
            up = np.array([0.0, 1.0, 0.0]) + out * 0.35 + np.cross(right, [0, 1, 0]) * k["tilt"]
            up /= np.linalg.norm(up)
            corners = [centre - right * hw - up * hh, centre + right * hw - up * hh,
                       centre + right * hw + up * hh, centre - right * hw + up * hh]
            corners = [np.clip(c, (bx0, by0 - 0.05, bz0), (bx1, by1, bz1)) for c in corners]
            uv = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]  # v up the card (the writer flips it)
            n = outward(b, np.array(corners))
            # the interior reads darker: deeper clumps turn their normals down and in
            dark = (1 - k["depth"]) / 0.38
            n = n * (1 - 0.55 * dark) + np.array([0.0, -0.45, 0.0]) * dark
            n /= np.linalg.norm(n, axis=1, keepdims=True)
            g = np.cross(corners[1] - corners[0], corners[3] - corners[0])
            front = g @ n.mean(0) > 0
            for side in (0, 1):
                base = len(pts)
                pts.extend(corners)
                nrm.extend(n.tolist())
                tri = [(0, 1, 2), (0, 2, 3)]
                if front == (side == 1):
                    tri = [(t[0], t[2], t[1]) for t in tri]
                for t in tri:
                    faces.append(([base + i for i in t], [uv[i] for i in t], tex))
    return pts, nrm, faces


def box(lo, hi):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    # outward-facing triangles
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2), (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5)]
    return v, f


def lod_bytes(points, normals, faces, resolution, properties=(), selections=None, mass=None):
    """One P3DM 28/256 LOD: faces [(point indices (normal index = point index), uvs, texture)]."""
    np_, nf = len(points), len(faces)
    out = [b"P3DM", struct.pack("<iiiiii", 28, 256, np_, len(normals), nf, 0)]
    out.append(b"".join(struct.pack("<fffi", *map(float, p), 0) for p in points))
    out.append(b"".join(struct.pack("<fff", *map(float, n)) for n in normals))
    for idx, uvs, tex in faces:
        rec = [struct.pack("<i", 3)]
        for i in range(4):
            if i < 3:
                rec.append(struct.pack("<iiff", idx[i], idx[i], float(uvs[i][0]), 1.0 - float(uvs[i][1])))
            else:
                rec.append(struct.pack("<iiff", 0, 0, 0.0, 0.0))
        rec.append(struct.pack("<I", 0))
        rec.append(tex.encode("ascii") + b"\0" + b"\0")
        out.append(b"".join(rec))
    out.append(b"TAGG")
    for name, (sp, sf) in (selections or {}).items():
        pay = bytearray(np_ + nf)
        for p in sp:
            pay[p] = 1
        for f in sf:
            pay[np_ + f] = 1
        out.append(b"\x01" + name.encode() + b"\0" + struct.pack("<i", len(pay)) + bytes(pay))
    if mass is not None:
        pay = struct.pack(f"<{np_}f", *mass)
        out.append(b"\x01#Mass#\0" + struct.pack("<i", len(pay)) + pay)
    for k, v in properties:
        pay = k.encode().ljust(64, b"\0") + v.encode().ljust(64, b"\0")
        out.append(b"\x01#Property#\0" + struct.pack("<i", len(pay)) + pay)
    out.append(b"\x01#EndOfFile#\0" + struct.pack("<i", 0))
    out.append(struct.pack("<f", resolution))
    return b"".join(out)


def build(name, spec):
    seed = int(hashlib.sha256(name.encode()).hexdigest()[:8], 16)
    b, cl = clumps(spec, seed)
    lods, triangles = [], []
    for res, share, scale in LODS:
        pts, nrm, faces = visual_mesh(spec, b, cl, share, scale)
        triangles.append(len(faces))
        lods.append(lod_bytes(pts, nrm, faces, spec["res"] if res is None else res, [("lodnoshadow", "1")]))
    # collision: one convex box in the stock geometry LOD's extent, written inward (the engine's convention)
    gv, gf = box(*spec["geometry"])
    gn = [(np.array(p) - np.mean(gv, 0)) / max(np.linalg.norm(np.array(p) - np.mean(gv, 0)), 1e-6) for p in gv]
    inward = [((a, c, b_), [(0, 0)] * 3, "") for a, b_, c in gf]
    lods.append(lod_bytes(gv, gn, inward, RES_GEOMETRY, [("dammage", "tent"), ("map", "bush"), ("canocclude", "0")],
                          {"Component01": (list(range(8)), list(range(12)))}, [100.0 / 8] * 8))
    # shadow volume: a closed hull a little inside the body
    (x0, y0, z0), (x1, y1, z1) = spec["visual"]
    sv, sf = box((x0 * 0.85, y0, -b["c"] * 0.9), (x1 * 0.85, y0 + b["h"] * 0.85, b["c"] * 0.9))
    sn = [(0.0, 1.0, 0.0)] * 8
    lods.append(lod_bytes(sv, sn, [(list(f), [(0, 0)] * 3, "") for f in sf], RES_SHADOW))
    data = b"MLOD" + bytes([1, 1, 0, 0]) + struct.pack("<I", len(lods)) + b"".join(lods)
    return data, triangles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--prefix", default="op_ground_materials")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {"api": 1, "models": []}
    for name, spec in HEDGES.items():
        data, triangles = build(name, spec)
        file = f"hedge_{name}.p3d"
        (args.out / file).write_bytes(data)
        manifest["models"].append({"source": f"data3d\\{name}.p3d", "model": f"{args.prefix}\\{file}"})
        print(f"{name}: LOD triangles {triangles}")
    (args.out / "models.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
