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
                  res=0.5),
    "krovi2": dict(visual=((-2.95, -1.13, -1.68), (2.95, 1.06, 1.68)), geometry=((-2.26, -1.12, -0.38), (2.38, -0.1, 0.85)),
                   res=0.5),
    "krovi3": dict(visual=((-3.01, -1.13, -1.63), (3.03, 1.16, 1.63)), geometry=((-2.75, -1.12, -0.33), (2.33, -0.1, 0.9)),
                   res=0.5),
    "krovi4": dict(visual=((-2.66, -1.09, -1.56), (2.66, 1.13, 1.56)), geometry=((-2.24, -1.1, -0.63), (2.32, 0.1, 0.58)),
                   res=0.0),
    "krovi_bigest": dict(visual=((-2.95, -1.47, -2.09), (2.97, 1.42, 2.09)), geometry=((-2.89, -1.47, -1.3), (2.84, 0.74, 1.32)),
                         res=0.1),
    "krovi_long": dict(visual=((-4.08, -1.36, -1.18), (4.08, 1.51, 1.1)), geometry=((-3.75, -1.28, -0.73), (3.76, 0.18, 0.7)),
                       res=0.3),
}
# LOD resolutions after LOD 0, and the share of sprigs (and their scale) each keeps
LODS = [(None, 1.0, 1.0), (2.0, 0.33, 1.5), (5.0, 0.11, 2.3)]
RES_GEOMETRY = 1e13
RES_SHADOW = 6e15
# Sprig cards (owner, 2026-10-13: "the hedges must be volumetric not just 3-4 textures"): each hedge
# is a few hundred leafy sprigs (ez-tree's oak and ash leaf sheets, MIT) rooted inside the body and
# pointing out of it, in two populations -- a shell and a darker interior that closes the gaps.
SPRIG_LEN = (0.45, 0.75)  # m, the sheet's full height
LAYERS = 2.6  # mean sprig cover over the skin (sheet fill ~35 %)
SHEET_FILL = 0.35
INTERIOR_SHARE = 0.3
LEAF_TEXTURE = "hedge_leaves.paa"  # 2 x 2 atlas: oak | ash, lit row over shaded row
# ez-tree's sheets are bright spring leaves (mean sRGB ~(92, 124, 52)); next to OFP's grass they read
# lime. Graded toward a summer hedge: a little desaturated, darker. The shaded row is the same
# leaves inside the hedge and near the ground, where little sky reaches (OFP models carry no AO).
LEAF_SATURATION = 0.8
LEAF_GAIN = 0.52
SHADED_GAIN = 0.36
# Owner: "it needs to have the same color as the grass". Measured on a capture (Everon 7050/6441,
# 2026-10-13): the grass renders at R/G 0.91, B/G 0.71, the graded leaves at 0.79 / 0.52, and the
# lighting keeps those ratios -- so the leaves take the grass's ratios per channel.
LEAF_TINT = (0.914 / 0.785, 1.0, 0.711 / 0.524)


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


def skin_point(b, x, th, depth):
    """A point of the body at fraction `depth` of the way from its axis to its skin."""
    end = (1 - abs(x) ** 6) ** (1 / 6)
    cz, cy = math.cos(th), math.sin(th)
    sz = np.sign(cz) * abs(cz) ** 0.8 * b["c"] * end * depth
    sy = abs(cy) ** 0.8 * b["h"] * end * depth
    return np.array([b["cx"] + x * b["a"] * depth ** 0.3, b["y0"] + sy, sz])


def sprigs(spec, seed):
    rng = np.random.default_rng(seed)
    b = body(spec)
    skin = 2 * b["a"] * math.pi * (b["h"] + b["c"]) / 2
    mean_len = sum(SPRIG_LEN) / 2
    n = int(LAYERS * skin / (mean_len * mean_len * SHEET_FILL))
    out = []
    for i in range(n):
        interior = rng.uniform() < INTERIOR_SHARE
        depth = rng.uniform(0.35, 0.75) if interior else 0.78 + 0.22 * math.sqrt(rng.uniform())
        p = skin_point(b, rng.uniform(-1, 1), rng.uniform(0, math.pi), depth)
        out.append(dict(p=p, depth=depth, length=rng.uniform(*SPRIG_LEN), roll=rng.uniform(0, math.pi),
                        bend=rng.normal(0, 0.45, 3), leaf=int(rng.integers(2)), flip=bool(rng.integers(2)),
                        keep=rng.uniform()))
    return b, out


def visual_mesh(spec, b, sp, share, scale, texture):
    """One quad a sprig, both faces (each its own points), rooted at its point and pointing out of the
    body; normals from the body, turned down and in with depth."""
    pts, nrm, faces = [], [], []
    (bx0, by0, bz0), (bx1, by1, bz1) = b["box"]
    for k in sp:
        if k["keep"] >= share:
            continue
        length = k["length"] * scale
        root = k["p"]
        out = outward(b, root)
        # out of the body, scattered, a little upward (sprigs grow toward the light)
        axis = out + k["bend"] + np.array([0.0, 0.25, 0.0])
        axis /= np.linalg.norm(axis)
        side = np.cross(axis, [0.0, 1.0, 0.0])
        if np.linalg.norm(side) < 1e-3:
            side = np.array([1.0, 0.0, 0.0])
        side /= np.linalg.norm(side)
        other = np.cross(axis, side)
        across = side * math.cos(k["roll"]) + other * math.sin(k["roll"])
        # the sheet's stem is at its bottom centre: the root sits a little above the card's bottom edge
        base = root - axis * length * 0.08
        hw = length * 0.5
        corners = [base - across * hw, base + across * hw, base + across * hw + axis * length, base - across * hw + axis * length]
        corners = [np.clip(c, (bx0 - 0.2, by0 - 0.05, bz0 - 0.2), (bx1 + 0.2, by1 + 0.2, bz1 + 0.2)) for c in corners]
        u0, u1 = (0.5, 0.0) if k["flip"] else (0.0, 0.5)
        u0, u1 = u0 + 0.5 * k["leaf"], u1 + 0.5 * k["leaf"]
        # the shaded row (the atlas's lower half) inside the body and along its foot
        low = (root[1] - b["y0"]) / b["h"]
        shaded = k["depth"] < 0.78 or low < 0.18 * k["keep"] + 0.08
        v0 = 0.0 if shaded else 0.5
        uv = [(u0, v0), (u1, v0), (u1, v0 + 0.5), (u0, v0 + 0.5)]  # v up the sheet (the writer flips it)
        n = outward(b, np.array(corners))
        # the interior reads darker: deeper sprigs turn their normals down and in
        dark = min(max((0.95 - k["depth"]) / 0.6, 0.0), 1.0)
        n = n * (1 - 0.6 * dark) + np.array([0.0, -0.5, 0.0]) * dark
        n /= np.linalg.norm(n, axis=1, keepdims=True)
        g = np.cross(corners[1] - corners[0], corners[3] - corners[0])
        front = g @ n.mean(0) > 0
        for face_side in (0, 1):
            first = len(pts)
            pts.extend(corners)
            nrm.extend(n.tolist())
            tri = [(0, 1, 2), (0, 2, 3)]
            if front == (face_side == 1):
                tri = [(t[0], t[2], t[1]) for t in tri]
            for t in tri:
                faces.append(([first + i for i in t], [uv[i] for i in t], texture))
    return pts, nrm, faces


def leaf_atlas(ez_tree_leaves, out_png):
    """oak | ash from ez-tree's leaf sheets (MIT), each 512 px, colour dilated into the clear texels so
    the mips do not pull the leaf edges toward the sheet's background. Alpha keeps a narrow soft rim
    (DXT5): a 1-bit edge drew every leaf's contour hard (owner: "cartoony because the leaf contours are
    drawn"), and the rim is what MSAA alpha-to-coverage softens. It stays narrow (a few texels) so the
    texture classifies as an anti-aliased cutout (TextureWgpu.cpp), not a blend."""
    from PIL import Image, ImageFilter

    atlas = Image.new("RGBA", (1024, 1024))
    for i, name in enumerate(("oak", "ash")):
        im = Image.open(Path(ez_tree_leaves) / f"{name}.png").convert("RGBA").resize((512, 512), Image.LANCZOS)
        a = np.asarray(im).astype(np.float32)
        alpha = a[..., 3] >= 128
        t = np.clip((a[..., 3] / 255.0 - 0.3) / 0.4, 0.0, 1.0)
        soft = t * t * (3 - 2 * t)
        rgb = a[..., :3] * alpha[..., None]
        w = alpha.astype(np.float32)
        # push-pull dilation: blur colour and coverage together, divide, fill only the clear texels
        for radius in (2, 6, 16, 48):
            cb = np.asarray(Image.fromarray(rgb.astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))).astype(np.float32)
            wb = np.asarray(Image.fromarray((w * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))).astype(np.float32) / 255
            fill = (w < 0.5) & (wb > 0.01)
            rgb[fill] = (cb[fill] / np.maximum(wb[fill, None], 1e-3)).clip(0, 255)
            w = np.maximum(w, (wb > 0.01).astype(np.float32))
        lum = rgb @ np.array([0.2126, 0.7152, 0.0722])
        rgb = (lum[..., None] + (rgb - lum[..., None]) * LEAF_SATURATION) * LEAF_GAIN * np.array(LEAF_TINT)
        for row, gain in ((0, 1.0), (1, SHADED_GAIN)):
            tile = np.dstack([(rgb * gain).clip(0, 255), soft * 255.0]).astype(np.uint8)
            atlas.paste(Image.fromarray(tile), (512 * i, 512 * row))
    atlas.save(out_png)


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


def build(name, spec, texture):
    seed = int(hashlib.sha256(name.encode()).hexdigest()[:8], 16)
    b, sp = sprigs(spec, seed)
    lods, triangles = [], []
    for i, (res, share, scale) in enumerate(LODS):
        pts, nrm, faces = visual_mesh(spec, b, sp, share, scale, texture)
        triangles.append(len(faces))
        # Only LOD 0 is `lodnoshadow`: the cascades then cast from LOD 1 (a third of the sprigs,
        # alpha-tested), so a hedge shades itself and the ground. With the flag on every visual LOD
        # (the first version) the shadow pass found no caster and every leaf was lit alike.
        props = [("lodnoshadow", "1")] if i == 0 else []
        lods.append(lod_bytes(pts, nrm, faces, spec["res"] if res is None else res, props))
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
    parser.add_argument("--leaves", type=Path, required=True, help="ez-tree's textures/leaves (oak.png, ash.png)")
    parser.add_argument("--atlas", type=Path, required=True, help="where to write the leaf atlas PNG")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    leaf_atlas(args.leaves, args.atlas)
    manifest = {"api": 1, "models": []}
    for name, spec in HEDGES.items():
        data, triangles = build(name, spec, f"{args.prefix}\\{LEAF_TEXTURE}")
        file = f"hedge_{name}.p3d"
        (args.out / file).write_bytes(data)
        manifest["models"].append({"source": f"data3d\\{name}.p3d", "model": f"{args.prefix}\\{file}"})
        print(f"{name}: LOD triangles {triangles}")
    (args.out / "models.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
