"""Tidewater's humpback whale as an OFP animal (owner, 2026-10-19: "port them over as well").

Build-time only. Reads the runtime export export_tidewater_whale.py wrote from Tidewater's procedural humpback
(dgreenheck/tidewater, MIT, (c) DRG Software Solutions LLC; side profile traced from a NOAA Fisheries
illustration, public domain) and writes, into --out:

* humpback.p3d   MLOD: Tidewater's three levels of detail (the finest with its relief map baked in), the
                 flippers as named selections, geometry and fire geometry (convex slices along the body, 30 t),
                 memory points;
* whale_skin.png Tidewater's baked skin (albedo);
* config.cpp     CfgVehicles OP_TidewaterHumpback (simulation "whale", Whale.cpp) with Tidewater's spine rig
                 (class TidewaterWhale): 40 frames from the snout to past the flukes, each frame's rest height,
                 the flipper roots and the blowhole. Whale.cpp poses every vertex on the CPU from the frames
                 its port of WhaleBrain computes, exactly as Tidewater's vertex shader does.

Frames: as the boat (generate_tidewater_boat.py): Tidewater's +Z forward, +Y up, +X the whale's left; the file
stores (x, y, -z), which the engine's MLOD loader turns into (-x, y, z). The memory point "tw_origin" is
Tidewater's rest-frame origin.
"""

import argparse
import gzip
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.spatial import ConvexHull

from generate_tidewater_boat import Lod, mirror, VISUAL_ORDER, RES_GEOMETRY, RES_MEMORY, RES_FIRE, fmt, solve_masses

PREFIX = "op_tidewater_wildlife"
MODEL = "humpback.p3d"
SKIN = "whale_skin"
FLESH = "whale_flesh"
CHUNKS = 3  # whale_chunk1..3.p3d: what an explosion tears off (Whale.cpp)
MASS = 30000.0  # kg, an adult humpback
# visual LOD resolutions (Tidewater switches at 48 m and 170 m)
RESOLUTIONS = (1.0, 4.0, 14.0)
# geometry slices along the body (Tidewater z, snout +5.8 to the fluke notch -8.7)
SLICES = (6.0, 3.6, 1.2, -1.2, -3.6, -5.6, -7.2, -8.0, -9.5)


def load(path):
    raw = gzip.decompress(Path(path).read_bytes())
    assert raw[:4] == b"OPWH", "not a humpback runtime file"
    version, K, root, z_head, dz, notch_z, length = struct.unpack_from("<IIIffff", raw, 4)
    o = 4 + struct.calcsize("<IIIffff")
    blow = struct.unpack_from("<3f", raw, o); o += 12
    pec_l = struct.unpack_from("<3f", raw, o); o += 12
    pec_r = struct.unpack_from("<3f", raw, o); o += 12
    rest_y = struct.unpack_from(f"<{K}f", raw, o); o += 4 * K
    (n_levels,) = struct.unpack_from("<I", raw, o); o += 4
    counts = [struct.unpack_from("<II", raw, o + 8 * i) for i in range(n_levels)]
    o += 8 * n_levels
    levels = []
    for nv, ni in counts:
        v = np.frombuffer(raw, np.float32, nv * 12, o).reshape(-1, 12).astype(np.float64); o += nv * 48
        ix = np.frombuffer(raw, np.uint32, ni, o).reshape(-1, 3).astype(np.int64); o += ni * 4
        levels.append((v, ix))
    rig = dict(K=K, root=root, zHead=z_head, dz=dz, notchZ=notch_z, length=length, blowhole=blow, pecL=pec_l,
               pecR=pec_r, restY=rest_y)
    return rig, levels


def visual_lod(v, ix):
    lod = Lod()
    pos, nrm, uv, part = v[:, 0:3], v[:, 3:6], v[:, 6:8], np.rint(v[:, 9]).astype(int)
    lod.points.extend(mirror(pos))
    # MLOD normals point into the surface (see the boat's add_mesh)
    lod.normals.extend(-mirror(nrm))
    tex = f"{PREFIX}\\{SKIN}.paa"
    for a, b, c in ix:
        order = VISUAL_ORDER(a, b, c)
        lod.faces.append((list(order), list(order), [(uv[i][0], uv[i][1]) for i in order], tex))
    # the flippers turn about their roots (Whale.cpp reads the selections; Tidewater's rig.y)
    for name, p in (("tw_flipper_l", 1), ("tw_flipper_r", 2)):
        pts = np.nonzero(part == p)[0]
        sel = set(pts.tolist())
        faces = [i for i, t in enumerate(ix) if t[0] in sel]
        lod.select(name, pts.tolist(), faces)
    return lod


def hull_component(lod, pts, name):
    """One convex component (faces written inward, the engine's geometry convention)."""
    pts = np.asarray(pts, dtype=np.float64)
    hull = ConvexHull(pts)
    used = np.unique(hull.simplices)
    remap = {int(u): k for k, u in enumerate(used)}
    base = lod.add_points(list(pts[used]))
    first = len(lod.faces)
    centre = pts[used].mean(axis=0)
    for s in hull.simplices:
        a, b, c = (pts[i] for i in s)
        outward = np.dot(np.cross(b - a, c - a), a - centre) > 0
        t = [remap[int(i)] + base for i in s]
        if outward:
            t = [t[0], t[2], t[1]]
        lod.faces.append((t, [0, 0, 0], [(0, 0)] * 3, ""))
    lod.select(name, range(base, base + len(used)), range(first, len(lod.faces)))


def collision_lod(v):
    """Convex slices of the coarsest level along the body, and each flipper: Tidewater's rest pose."""
    pos, part = v[:, 0:3], np.rint(v[:, 9]).astype(int)
    lod = Lod()
    body = pos[part == 0]
    n = 0
    for z1, z0 in zip(SLICES[:-1], SLICES[1:]):
        # each slice overlaps its neighbours a little so the body has no seams
        s = body[(body[:, 2] <= z1 + 0.15) & (body[:, 2] >= z0 - 0.15)]
        if len(s) >= 8:
            n += 1
            hull_component(lod, mirror(s), f"Component{n:02d}")
    for p in (1, 2):
        n += 1
        hull_component(lod, mirror(pos[part == p]), f"Component{n:02d}")
    return lod


def build(rig, levels):
    lods = []
    for (v, ix), res in zip(levels, RESOLUTIONS):
        lods.append((visual_lod(v, ix), res, {}))
    coarse = levels[-1][0]
    geo = collision_lod(coarse)
    # the centre of mass: the body's centroid (it floats level in Tidewater's rest pose)
    com = mirror(coarse[np.rint(coarse[:, 9]) == 0, 0:3].mean(axis=0))
    mass = solve_masses(geo.points, MASS, com)
    lods.append((geo, RES_GEOMETRY, dict(mass=list(mass), properties=[("class", "vehicle"), ("map", "")])))
    lods.append((collision_lod(coarse), RES_FIRE, {}))
    mem = Lod()
    # (blowhole and flipper_l also tell Whale.cpp which way the loader turned the model)
    for name, p in (("tw_origin", (0.0, 0.0, 0.0)), ("blowhole", rig["blowhole"]), ("flipper_l", rig["pecL"])):
        base = mem.add_points([mirror(p)])
        mem.select(name, [base])
    lods.append((mem, RES_MEMORY, {}))
    # pre-centred on the box of every LOD's points (an MLOD written off-centre draws shifted, the boat measured)
    allp = np.array([p for lod, _, _ in lods for p in lod.points], dtype=np.float64)
    centre = (allp.min(axis=0) + allp.max(axis=0)) * 0.5
    for lod, _, _ in lods:
        lod.points = [np.asarray(p, dtype=np.float64) - centre for p in lod.points]
    blobs = [lod.bytes(res, **kw) for lod, res, kw in lods]
    return b"MLOD" + bytes([1, 1, 0, 0]) + struct.pack("<I", len(blobs)) + b"".join(blobs)


def icosphere(subdiv=1):
    t = (1 + 5 ** 0.5) / 2
    v = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t), (t, 0, -1),
         (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    v = [np.array(p, dtype=np.float64) / np.linalg.norm(p) for p in v]
    f = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
         (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7),
         (9, 8, 1)]
    for _ in range(subdiv):
        cache, nf = {}, []

        def mid(a, b):
            k = (min(a, b), max(a, b))
            if k not in cache:
                m = v[a] + v[b]
                v.append(m / np.linalg.norm(m))
                cache[k] = len(v) - 1
            return cache[k]

        for a, b, c in f:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        f = nf
    return np.array(v), f


def flesh_image(size=128):
    """Torn whale meat and blubber: dark red muscle, a band of pale blubber, wet highlights."""
    rng = np.random.default_rng(7)
    n = rng.random((size // 8, size // 8))
    n = np.asarray(Image.fromarray((n * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC), dtype=np.float64) / 255
    fine = rng.random((size, size))
    meat = np.stack([0.42 + 0.25 * n, 0.04 + 0.05 * n, 0.05 + 0.04 * n], -1) * (0.75 + 0.25 * fine[..., None])
    y = np.linspace(0, 1, size)[:, None, None]
    blubber = np.clip((y - 0.72) * 6, 0, 1)
    img = meat * (1 - blubber) + np.array([0.86, 0.78, 0.7]) * blubber
    return Image.fromarray(np.clip(img * 255, 0, 255).astype(np.uint8))


def chunk_model(seed, back_uv):
    """A torn lump of whale (0.4-0.8 m): skin on one side, red meat on the rest; a convex box of geometry."""
    rng = np.random.default_rng(seed)
    v, f = icosphere(1)
    r = 0.25 + 0.15 * seed
    scale = np.array([1.0 + 0.4 * rng.random(), 0.55 + 0.3 * rng.random(), 0.8 + 0.5 * rng.random()]) * r
    pts = v * scale * (0.8 + 0.4 * rng.random((len(v), 1)))
    lod = Lod()
    lod.points.extend(pts)
    nrm = pts / np.linalg.norm(pts, axis=1, keepdims=True)
    lod.normals.extend(-nrm)  # MLOD normals point into the surface
    for a, b, c in f:
        centre = (pts[a] + pts[b] + pts[c]) / 3
        skin = centre[1] > 0.25 * scale[1]
        tex = f"{PREFIX}\{SKIN if skin else FLESH}.paa"
        uvs = [(back_uv[0] + 0.01 * pts[i][0], back_uv[1] + 0.01 * pts[i][2]) if skin else
               (0.5 + 0.5 * pts[i][0] / scale[0], 0.5 - 0.5 * pts[i][1] / scale[1]) for i in (a, b, c)]
        lod.faces.append(([a, b, c], [a, b, c], uvs, tex))
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    from generate_tidewater_boat import box
    bv, bf = box(lo * 0.85, hi * 0.85)
    geo = Lod()
    geo.points.extend(bv)
    for t in bf:
        geo.faces.append(([t[0], t[2], t[1]], [0, 0, 0], [(0, 0)] * 3, ""))
    geo.select("Component01", range(8), range(len(geo.faces)))
    mass = [float(900 * np.prod(hi - lo) * 0.5 / 8)] * 8
    blobs = [lod.bytes(1.0), geo.bytes(RES_GEOMETRY, mass=mass, properties=[("class", "vehicle")]), Lod().bytes(RES_MEMORY)]
    return b"MLOD" + bytes([1, 1, 0, 0]) + struct.pack("<I", len(blobs)) + b"".join(blobs)


def config(rig):
    chunk_classes = "".join(f"""	class OP_TidewaterWhaleChunk{i + 1}: FxExploArmor1
	{{
		scope = 1;
		model = "\{PREFIX}\whale_chunk{i + 1}";
		timeToLive = 40;
		submergeSpeed = 0.05;
	}};
""" for i in range(CHUNKS))
    return f"""// Generated by scripts/default-content/generate_tidewater_whale.py -- do not edit.
// Tidewater's humpback whale (dgreenheck/tidewater, MIT). Whale.cpp swims it (a port of Tidewater's WhaleBrain:
// a loop through deep water, surfacing sequences of 3-6 blows, breaches, flipper slaps, the fluke-up dive) and
// poses it from the spine rig below, in Tidewater's whale frame (+Z forward, +Y up, +X the whale's left), placed
// in the model at the memory point "tw_origin".
class CfgPatches
{{
	class {PREFIX}
	{{
		units[] = {{"OP_TidewaterHumpback"}};
		weapons[] = {{}};
		requiredVersion = 0;
		requiredAddons[] = {{}};
	}};
}};
class CfgVehicles
{{
	class Thing;
	class OP_TidewaterHumpback: Thing
	{{
		scope = 2;
		simulation = "whale";
		vehicleClass = "Objects";
		displayName = "Humpback whale (Tidewater)";
		model = "\\{PREFIX}\\{Path(MODEL).stem}";
		accuracy = 0.2;
		armor = 350;
		destrType = "DestructMan"; // hit like a man: damage without the wreck's crumple (DestructNo takes no damage at all)
		mapSize = 15;
		// (OP) what an explosion tears off it (Whale.cpp)
		chunks[] = {{{", ".join(f'"OP_TidewaterWhaleChunk{i + 1}"' for i in range(CHUNKS))}}};
		class TidewaterWhale
		{{
			origin = "tw_origin";
			zHead = {rig['zHead']:.5f}; // the first spine frame (the snout), frames dz apart toward the tail
			dz = {rig['dz']:.5f};
			rootIndex = {rig['root']}; // the frame nearest z = 0 (the chain is integrated outward from it)
			restY[] = {{{fmt(rig['restY'])}}}; // each frame's rest height on the centre line
			notchZ = {rig['notchZ']:.4f};
			blowhole[] = {{{fmt(rig['blowhole'])}}};
			flipperLeft[] = {{{fmt(rig['pecL'])}}}; // the flippers' roots (selections tw_flipper_l / _r)
			flipperRight[] = {{{fmt(rig['pecR'])}}};
			flipperLength = 4.5;
		}};
	}};
	// pieces of a whale blown apart: the stock explosion debris' simulation (thrown, they splash and sink)
	class FxExploArmor1;
{chunk_classes}}};
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="content/default-packs/tidewater-wildlife/source")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rig, levels = load(args.source / "humpback.bin.gz")
    (args.out / MODEL).write_bytes(build(rig, levels))
    Image.open(args.source / "humpback_albedo.png").convert("RGB").save(args.out / f"{SKIN}.png")
    flesh_image().save(args.out / f"{FLESH}.png")
    # the chunks' skin: a patch of the back (the coarsest level's highest body vertices)
    v = levels[-1][0]
    back = v[(np.rint(v[:, 9]) == 0) & (v[:, 1] > np.percentile(v[:, 1], 90)), 6:8].mean(axis=0)
    for i in range(CHUNKS):
        (args.out / f"whale_chunk{i + 1}.p3d").write_bytes(chunk_model(i + 1, back))
    (args.out / "textures.json").write_text(json.dumps({"DXT1": [SKIN, FLESH], "DXT5": []}, indent=1))
    (args.out / "config.cpp").write_text(config(rig), newline="\n")
    print(f"{MODEL}: visual levels {[len(ix) for _, ix in levels]} triangles, K {rig['K']}, root {rig['root']}")


if __name__ == "__main__":
    main()
