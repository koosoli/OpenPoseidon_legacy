"""Sinkhole W5 step 2: The Forest's surface nature (trees, bushes, saplings, plants, rocks, boulders, cliffs) ->
MLOD P3D models + world placements for theforest.wrp. Called by forest_terrain.py.

Input (extracted from the owner's own copy of The Forest by M:\\ForestClaude\\tools, never committed):
  scenes/level2.json   every placement of the main island scene (Unity world matrix, model key, scene path)
  models.json, models/<id>.npz   each model's High/Mid/Low LOD meshes in prefab-root space (export_models.py)
  prefab_roots.json    each LOD prefab root's own scale (export_prefab_roots.py)
  colliders.json       the prefabs' colliders, with the mesh colliders' meshes (export_colliders.py)
  materials.json, textures/*.png  Unity materials and their textures
Output under <out>: o/<id>.p3d (one MLOD per model), t/<texture>.png (the textures they use).

Facts this relies on (checked 2026-10-04):
  * Unity and Poseidon are both left-handed, y up: model vertices and placement rotations copy straight across;
    a placement's world position moves by the terrain corner (-1750, -1742.63) and the sea level.
  * A prefab placement (a LOD_* spawner) spawns its High/Mid/Low prefabs at the spawner's position and rotation
    with the PREFAB ROOT's own scale; the spawner's scale is not applied (PineTree root 11.0 / spawner 11.57,
    TopHeavy 1.0 / 10.68: only root sizing buries every pine type's trunk 2-3 m like the rest). The root scales
    come from prefab_roots.json (export_prefab_roots.py) and are baked into each LOD.
  * The engine keeps a placement's uniform scale only: the 11 direct mesh placements with a non-uniform one (cliff
    sections) get a model variant with that scale baked in.
  * Many pines are pivoted ~4 m under the terrain, so a placement's depth below the terrain does not mean it is in
    a cave: caves are left out by their scene folder instead (step 3 brings them).
  * Unity UV v runs bottom-up; Poseidon's top-down.
  * Every model carries autocenter=0: the engine otherwise moves a model's origin to the middle of its bounds,
    and the authored-elevation placement this world uses never adds it back -- every object sat sunk by about
    half its height (70 m pines showed their upper half; rocks were half-buried domes) until this was set.
"""
import hashlib
import json
import os
import re
import shutil
import struct

import numpy as np
from PIL import Image

import forest_collision as col

# The island's surface nature, by top-level scene folder. Caves, cave planks and the cave props wait for step 3,
# waterfalls for step 4 (water); villages, the yacht and the plane wreck are man-made set dressing for later.
SURFACE_FOLDERS = ("Nature_Spawned", "Nature_Placed", "NaturePlaced_Cliff_Boulders", "NaturePlaced_CliffCaps",
                   "NaturePlaced_Cliffs", "NaturePlaced_rockK", "SnowCliffs_left", "SnowCliffs_Right",
                   "BackgroundMountains_LODGroup")
# MLOD resolutions for the High/Mid/Low LODs. Scene::LevelFromDistance2 asks for about
# distance * 2 * lodCoef / screen width * tan(fov/2), ~ distance / 850 at 1280 px: Mid from ~50 m, Low from ~170 m.
LOD_RESOLUTIONS = (0.01, 0.06, 0.2)
REVERSE_WINDING = False  # MLOD lists a face in the same order as Unity (checked: reversed, rocks drew inside out)


def _safe(key):
    return key.replace(".assets", "").replace(":", "_").replace(".", "_")


def model_id(p):
    if p["kind"] == "prefab":
        return "p_" + _safe(p["lods"][0])
    return "m_" + _safe(p["mesh"]) + "_" + hashlib.md5(json.dumps(p["mats"]).encode()).hexdigest()[:6]


def _lod_bytes(v, n, uv, tris, textures, resolution, properties=(), two_sided=None):
    """One P3DM 28/256 LOD (MLODStructures.hpp): points, per-vertex normals, triangles with one texture per submesh,
    and named properties.

    A face's winding decides which side the engine culls; its vertex normals decide the light. Where the two
    disagree (7-13 % of some trees' bark, as authored) the face is culled from the side it is lit for and the
    trunk shows holes: such faces are turned round to agree with their normals. Foliage (`two_sided`) gets every
    face a second time, reversed, on its own copy of the points with the normals negated (added after the
    originals), as Unity's leaf shaders draw both sides of a card. (Sharing the points made the loader average
    each vertex's front and back normals to nothing: the shells drew blown-out white.)"""
    count = len(v)
    two_sided = two_sided or [False] * len(tris)
    fixed, sides = [], []
    for t, both in zip(tris, two_sided):
        g = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
        against = (g * n[t].sum(axis=1)).sum(axis=1) < 0
        t = np.where(against[:, None], t[:, ::-1], t)
        fixed.append(t)
        sides.append(both)
    back = any(sides)
    normals = np.vstack([n, -n]) if back else n
    pts = np.zeros(len(normals), dtype=[("p", "<f4", 3), ("flags", "<i4")])
    pts["p"] = np.vstack([v, v]) if back else v
    nfaces = sum(len(t) * (2 if both else 1) for t, both in zip(fixed, sides))
    out = [b"P3DM", struct.pack("<iiiiii", 28, 256, len(pts), len(normals), nfaces, 0),
           pts.tobytes(), np.ascontiguousarray(normals, "<f4").tobytes()]
    corner = np.dtype([("p", "<i4"), ("n", "<i4"), ("u", "<f4"), ("v", "<f4")])
    head = np.dtype([("count", "<i4"), ("c", corner, 4), ("flags", "<u4")])
    passes = [(t, tex, 0) for t, tex in zip(fixed, textures)] + \
             [(t[:, ::-1], tex, count) for t, tex, both in zip(fixed, textures, sides) if both]
    for t, texture, normal_base in passes:
        if REVERSE_WINDING:
            t = t[:, ::-1]
        rec = np.zeros(len(t), head)
        rec["count"] = 3
        for k in range(3):
            rec["c"]["p"][:, k] = t[:, k] + normal_base
            rec["c"]["n"][:, k] = t[:, k] + normal_base
            rec["c"]["u"][:, k] = uv[t[:, k], 0]
            rec["c"]["v"][:, k] = 1.0 - uv[t[:, k], 1]
        tail = np.frombuffer(texture.encode("ascii") + b"\0\0", np.uint8)
        rows = np.concatenate([rec.view(np.uint8).reshape(len(t), head.itemsize),
                               np.broadcast_to(tail, (len(t), len(tail)))], axis=1)
        out.append(rows.tobytes())
    out.append(b"TAGG")
    for k, val in properties:
        pay = k.encode().ljust(64, b"\0") + val.encode().ljust(64, b"\0")
        out.append(b"\x01#Property#\0" + struct.pack("<i", len(pay)) + pay)
    out.append(b"\x01#EndOfFile#\0" + struct.pack("<i", 0))
    out.append(struct.pack("<f", resolution))
    return b"".join(out)


# What collides, as in The Forest: trees by their trunk (the prefab's capsule collider), rocks, boulders and cliffs by
# their collision meshes (the prefab's mesh collider, else the lowest visual LOD); bushes, ferns, saplings and plants
# are walked through. The class is also the model's `map` property (map symbol and wind: trees and bushes sway).
CLASS_OF_SCRIPT = {"LOD_Trees": "tree", "LOD_Bush": "bush", "LOD_Sapling": "bush", "LOD_Plant": "bush",
                   "LOD_Rocks": "rock", "LOD_SmallRocks": "rock"}
COLLISION_MASS = 20000.0      # kg: static scenery, heavier than any vehicle that hits it
COLLISION_MAX_SIZE = 300.0    # m: the far background mountains get no collision (nobody reaches them)
COLLISION_MAX_PIECES = 24     # convex pieces per non-convex mesh (a 200 m cliff: ~9 m pieces)


# Rocks are bedded into the terrain. Half The Forest's rocks are open shells: the mesh stops at a rim meant to lie
# under the ground, and its terrain shader (Relief Terrain Pack) blends whatever still shows into the ground. The
# engine has no such blend, so where the rim stands above our terrain one sees under the shell -- the rock looks
# see-through and its edge a plate on the ground (39 % of the open rocks by more than 5 cm, 31 % by more than
# 30 cm). Each open rock is lowered by what its rim (the mesh's open edge, after welding UV seams) still stands
# above the terrain (95th percentile), at most BED_MAX or BED_SHARE of its height; a rim more than BED_SKIP up is
# resting on other rock (cliff caps on their cliffs) and is left. (Drawing the open shells two-sided as well
# turned them blown-out white in the engine; not done.)
BED_MAX = 0.6
BED_SHARE = 0.25
BED_SKIP = 1.0


def open_rim(v, tris):
    """The mesh's open-edge vertices (edges used by one triangle), UV-seam duplicates welded by position."""
    if not tris:
        return np.zeros((0, 3))
    T = np.vstack(tris)
    _, inv = np.unique(np.round(v, 4), axis=0, return_inverse=True)
    inv = inv.ravel()
    W = inv[T]
    e = np.sort(np.concatenate([W[:, [0, 1]], W[:, [1, 2]], W[:, [2, 0]]]), axis=1)
    edges, counts = np.unique(e, axis=0, return_counts=True)
    rim = np.unique(edges[counts == 1])
    first = np.zeros(inv.max() + 1, int)
    first[inv[::-1]] = np.arange(len(inv))[::-1]
    return v[first[rim]]


def build(extract, out, sea_level, origin_x, origin_z, prefix, ground=None):
    """Writes the models and textures; returns the placements [(3x4 world matrix, id, model name, bounds)].
    `ground(x, z)` (arrays, world metres) is the terrain height as the WRP stores it, for bedding the rocks."""
    placements = json.load(open(os.path.join(extract, "scenes", "level2.json")))["placements"]
    db = json.load(open(os.path.join(extract, "models.json")))
    materials = json.load(open(os.path.join(extract, "materials.json")))
    prefab_roots = json.load(open(os.path.join(extract, "prefab_roots.json")))
    colliders = json.load(open(os.path.join(extract, "colliders.json")))

    def class_of(p, entry):
        if p["kind"] == "prefab":
            return CLASS_OF_SCRIPT.get((p.get("scripts") or [""])[0], "rock")
        mats = [materials.get(k) or {} for lod in entry["lods"][:1] for k in lod["mats"]]
        foliage = mats and all("_BumpTransSpecMap" in m.get("tex", {}) for m in mats)
        return "bush" if foliage else "rock"
    os.makedirs(os.path.join(out, "o"), exist_ok=True)
    os.makedirs(os.path.join(out, "t"), exist_ok=True)
    chosen = [p for p in placements if p.get("active", True)
              and (p["path"].split("/") + ["", ""])[1] in SURFACE_FOLDERS and "WaterFalls" not in p["path"]]

    def texture_of(material_key):
        """The material's colour texture, written the way the engine must classify it (TextureWgpu::GetAlphaClass
        reads the alpha histogram). Unity's opaque materials keep SMOOTHNESS in the albedo alpha, which reads as
        a blend -- see-through rocks -- so it is dropped. Foliage (the materials with a translucency map) is an
        alpha-tested cutout: its alpha is hardened at the material's own _Cutoff, so it classifies as one."""
        material = materials.get(material_key) or {}
        tex = material.get("tex", {}).get("_MainTex")
        if not tex or not tex.get("file"):
            return ""
        src = os.path.join(extract, "textures", tex["file"])
        if not os.path.exists(src):
            return ""
        cutout = "_BumpTransSpecMap" in material["tex"]
        cutoff = round(material["floats"].get("_Cutoff", 0.5), 2)
        stem = os.path.splitext(tex["file"])[0]
        # one leaf image can serve materials with different cutoffs (resources_402: 0.18 and 0.46): one file each
        name = f"{stem}_{round(cutoff * 100):02d}_ca.png" if cutout else f"{stem}.png"
        dst = os.path.join(out, "t", name)
        if not os.path.exists(dst):
            image = Image.open(src)
            if cutout:
                rgba = np.array(image.convert("RGBA"))
                rgba[:, :, 3] = np.where(rgba[:, :, 3] >= cutoff * 255, 255, 0)
                Image.fromarray(rgba).save(dst)
            elif image.mode in ("RGBA", "LA", "P"):
                image.convert("RGB").save(dst)
            else:
                shutil.copyfile(src, dst)
        return f"{prefix}\\t\\{name}"

    # A Unity LODGroup whose levels are separate child objects arrives as several placements at one identical
    # transform (MassiveRock_Single + _LOD1 + _LOD2, High/Mid/Low, lod1/lod2: 506 groups, 1,361 placements). Drawn
    # as they come, every level shows at once; each group becomes ONE model whose LODs are its members.
    groups = {}
    for p in chosen:
        groups.setdefault((p["path"], tuple(np.round(p["m"], 3))), []).append(p)
    units = []
    for members in groups.values():
        unique = list({model_id(p): p for p in members}.values())  # exact duplicates (2 cliff rocks) draw once
        units.append(sorted(unique, key=lambda p: (_lod_rank(p["name"]), p["name"]))[:len(LOD_RESOLUTIONS)])

    def lod_sources(unit):
        """[(model id, LOD index within that model)] for the unit's model: a lone placement's own LODs, or the
        High level of each member of a LODGroup."""
        if len(unit) == 1:
            entry = db.get(model_id(unit[0]))
            return [(model_id(unit[0]), li) for li in range(len(entry["lods"][:len(LOD_RESOLUTIONS)]))] \
                if entry and entry["lods"] else []
        return [(model_id(p), 0) for p in unit if db.get(model_id(p)) and db[model_id(p)]["lods"]]

    written, tris_high, objects, skipped = {}, 0, [], 0
    collision_models, collision_pieces = {}, 0
    bedded = []
    for unit in units:
        p = unit[0]
        m = np.array(p["m"], np.float64).reshape(3, 4)
        axes = np.linalg.norm(m[:, :3], axis=0)
        bake = None  # a direct mesh placement's own non-uniform scale (11 cliff sections), baked into a variant
        if p["kind"] == "mesh" and axes.max() / axes.min() > 1.01:
            bake = tuple(round(float(a), 3) for a in axes)
            m[:, :3] /= axes
        sources = lod_sources(unit)
        key = (tuple(sources), bake)
        if key not in written:
            name, bounds = "", None
            base, base_h = None, 0.0
            lods = []
            cls = class_of(p, db[sources[0][0]]) if sources else "rock"
            props = [("autocenter", "0"), ("map", cls)]
            low = None  # the lowest visual LOD (vertices, triangles) in model space, for mesh-derived collision
            for level, (mid, li) in enumerate(sources):
                entry = db[mid]
                info = entry["lods"][li]
                arrays = np.load(os.path.join(extract, "models", mid + ".npz"))
                subs = sorted((k for k in arrays.files if k.startswith(f"l{li}_t")), key=lambda k: int(k.split("_t")[1]))
                tris = [arrays[k] for k in subs]
                textures = [texture_of(info["mats"][s] if s < len(info["mats"]) else None) for s in range(len(tris))]
                keep = [i for i, t in enumerate(tris) if len(t)]
                v, n = arrays[f"l{li}_v"], arrays[f"l{li}_n"]
                s = None
                if entry["kind"] == "prefab":
                    # the spawned prefab keeps its own root scale (see the module notes): baked in per LOD
                    s = np.array(prefab_roots[info["root"]]["scale"], np.float32)
                elif bake:
                    s = np.array(bake, np.float32)
                if s is not None:
                    v = v * s
                    n = n / s
                    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
                    if np.prod(s) < 0:
                        tris = [t[:, ::-1] for t in tris]
                if level == 0:
                    bounds = (v.min(axis=0), v.max(axis=0))
                    tris_high += info["nt"]
                foliage = ["_BumpTransSpecMap" in (materials.get(info["mats"][si] if si < len(info["mats"]) else None)
                                                   or {}).get("tex", {}) for si in range(len(tris))]
                rim = open_rim(v, [tris[i] for i in keep]) if cls == "rock" else np.zeros((0, 3))
                lods.append(_lod_bytes(v, n, arrays[f"l{li}_uv"], [tris[i] for i in keep], [textures[i] for i in keep],
                                       LOD_RESOLUTIONS[level], props if level == 0 else (),
                                       [foliage[i] for i in keep]))
                if level == 0:
                    base_h = float(np.ptp(v[:, 1]))
                    base = rim[np.random.default_rng(11).choice(len(rim), min(len(rim), 400), replace=False)] \
                        if len(rim) else None
                if any(len(tris[i]) for i in keep):
                    low = (v, np.vstack([tris[i] for i in keep]))
            if lods and cls != "bush" and low is not None and np.ptp(low[0], axis=0).max() < COLLISION_MAX_SIZE:
                pieces = []
                root = sources[0][0]
                cl = colliders.get(db[root]["lods"][0]["root"], []) if db[root]["kind"] == "prefab" else []
                rs = np.array(prefab_roots[db[root]["lods"][0]["root"]]["scale"]) if db[root]["kind"] == "prefab" else \
                    (np.array(bake) if bake else np.ones(3))
                for c in cl:
                    m4 = np.array(c["m"])
                    if c["type"] == "CapsuleCollider":
                        pts = col.capsule_points(c["radius"], c["height"], c["direction"], c["center"])
                    elif c["type"] == "SphereCollider":
                        pts = col.sphere_points(c["radius"], c["center"])
                    elif c["type"] == "BoxCollider":
                        pts = col.box_points(c["size"], c["center"])
                    else:
                        if len(c.get("v", [])) < 4:
                            continue
                        for vs, fs in col.mesh_components(col.transform(np.array(c["v"]), m4) * rs,
                                                          COLLISION_MAX_PIECES, convex=c.get("convex", False)):
                            pieces.append((vs, fs))
                        continue
                    h = col.convex_hull(col.transform(pts, m4) * rs)
                    if h is not None:
                        pieces.append(h)
                if not pieces and cls == "rock":
                    pieces = col.mesh_components(low[0], COLLISION_MAX_PIECES)
                if pieces:
                    lods.append(col.geometry_lod(pieces, COLLISION_MASS, props))
                    collision_models[cls] = collision_models.get(cls, 0) + 1
                    collision_pieces += len(pieces)
                if cls == "rock":
                    road = col.roadway_lod(*low)
                    if road is not None:
                        lods.append(road)
            if lods:
                file = model_id(p) if len(sources) < 2 or len(unit) == 1 else \
                    "g_" + hashlib.md5(repr(sources).encode()).hexdigest()[:10]
                if bake is not None:
                    file += "_" + hashlib.md5(repr(bake).encode()).hexdigest()[:6]
                with open(os.path.join(out, "o", file + ".p3d"), "wb") as f:
                    f.write(b"MLOD" + bytes([1, 1, 0, 0]) + struct.pack("<I", len(lods)) + b"".join(lods))
                name = f"{prefix}\\o\\{file}.p3d"
            written[key] = (name, bounds, cls, base, base_h)
        name, bounds, cls, base, base_h = written[key]
        if not name:
            skipped += 1
            continue
        if p["kind"] == "prefab":  # spawner position and rotation only; the scale is the prefab's, already baked
            m[:, :3] /= axes
        m[:, 3] -= (origin_x, sea_level, origin_z)
        if ground is not None and cls == "rock" and base is not None and len(base):
            w = base @ m[:, :3].T + m[:, 3]
            stands = np.percentile(w[:, 1] - ground(w[:, 0], w[:, 2]), 95)
            sink = float(np.clip(stands, 0.0, min(BED_MAX, BED_SHARE * base_h))) if stands <= BED_SKIP else 0.0
            if sink > 0:
                m[1, 3] -= sink
                bedded.append(sink)
        corners = np.array([[x, y, z] for x in (bounds[0][0], bounds[1][0]) for y in (bounds[0][1], bounds[1][1])
                            for z in (bounds[0][2], bounds[1][2])]) @ m[:, :3].T + m[:, 3]
        objects.append((m, len(objects), name, (corners.min(axis=0), corners.max(axis=0))))
    models = sum(1 for v in written.values() if v[0])
    print(f"objects: {len(objects)} placements of {models} models ({skipped} without a model; "
          f"{sum(1 for u in units if len(u) > 1)} LOD groups merged), {len(os.listdir(os.path.join(out, 't')))} "
          f"textures; the models' High LODs hold {tris_high} triangles")
    print(f"collision: {collision_models} models with a geometry LOD, {collision_pieces} convex pieces")
    if bedded:
        print(f"bedded: {len(bedded)} rocks lowered into the terrain, median {np.median(bedded):.2f} m, "
              f"max {max(bedded):.2f} m")
    return objects


def _lod_rank(name):
    """Order of a LODGroup member: High / Lod0 / the unsuffixed base first, then its number, Mid, Low."""
    lower = name.lower()
    if lower in ("high", "lod0"):
        return 0
    if lower == "mid":
        return 1
    if lower == "low":
        return 2
    digits = re.search(r"lod(\d)$", lower)
    return int(digits.group(1)) if digits else 0


def clear_of(objects, x, z, margin=2.0):
    """True when the world point (x, z) lies outside every object's footprint (by its bounding box)."""
    return all(not (lo[0] - margin < x < hi[0] + margin and lo[2] - margin < z < hi[2] + margin)
               for _, _, _, (lo, hi) in objects)


def write_objects(f, objects, name_length):
    """SingleObject4 records (LandFile.hpp): Matrix4P (aside, up, dir columns, then position), int id, name."""
    for m, oid, name, _ in objects:
        vals = [m[0, 0], m[1, 0], m[2, 0], m[0, 1], m[1, 1], m[2, 1], m[0, 2], m[1, 2], m[2, 2],
                m[0, 3], m[1, 3], m[2, 3]]
        raw = name.encode("ascii")
        if len(raw) >= name_length:
            raise ValueError(name)
        f.write(struct.pack("<12fi", *vals, oid) + raw.ljust(name_length, b"\0"))
