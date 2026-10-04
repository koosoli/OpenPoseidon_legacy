"""Sinkhole W5: The Forest's island terrain -> an Open Poseidon world (raw 4WVR + CfgWorlds add-on).

Input: the terrain extracted from the owner's own copy of The Forest (M:\\ForestClaude\\extract\\terrain:
height_u16.npy, terrain.json, alpha0/1.png, splat*.png; see extract_terrain.py). The extracted data is
never committed -- only this converter.

Output (staging folder, packed by Build-ForestIsland.ps1 as forestisland.pbo, prefix "forestisland"):
  theforest.wrp   2048 x 2048 cells, 1.708984375 m (LandGrid from CfgWorlds; 3500 m square)
  t1..t8.png      the eight ground layers (+ t*_n.png normals with the RTP data)
  m\\XX_ZZ.rvmat   + _s.png/_m.png: one terrain material per 219 m tile
  o\\*.p3d, t\\*.png  the surface nature's models and their textures (forest_objects.py), placed in the WRP
  config.cpp      CfgPatches + CfgWorlds >> TheForest
Facts this relies on (checked 2026-10-04):
  * height_u16.npy is [z][x] as saved: trees in the main scene sit within 0.5 m of it (61 %), transposed 1 %.
  * alpha0/1.png are stored upside down (row 0 = north): flipped, the cliff layer follows steep slopes
    (r = +0.18), the next best orientation r = 0.10.
  * Unity and Poseidon are both left-handed with y up, x east, z north: no mirroring. World origin: the
    terrain's corner, Unity (-1750, -1742.63) -> Poseidon (0, 0).
  * Sea level: The Forest's ocean is a separate system, not in the scene. SEA_LEVEL = 40 m: the yacht's keel
    is at 37.2 m, the flat beach shelf peaks at 40-43 m. Heights are written relative to it (the engine's sea is
    at 0). Retune with --sea-level once the shoreline can be seen.
"""
import argparse
import json
import os
import shutil
import struct
import sys

import numpy as np
from PIL import Image

import forest_objects

GRID = 2048
LANDDATA_SCALE = 0.03 * 1.5  # LandFile.hpp: heights are int16 * this
LAND_TEXTURES_MAX = 512
LEN_OBJNAME_4 = 96 - 20
PREFIX = "forestisland"
WORLD = "theforest"
TILE = 128              # cells per terrain material tile (219 m); 16 x 16 tiles
MASK_PX_PER_CELL = 2    # selector mask / satellite texels per cell (0.85 m)
SURFACE_TILE_M = 4.0    # metres one repeat of a ground layer texture covers


def write_rvmat(path, x0, z_top, size, satellite, mask, surfaces, normals):
    """An Arma 2 TerrainX material (Stage0 satellite, Stage1 mask, surface k: normal Stage 3+2k, colour Stage 4+2k),
    every stage mapped from the world position. terrain.wgsl terrain_uv: u = X*aside[0] + Y*up[0] + Z*dir[0] + pos[0],
    v the same with index 1 (source = world x, height, z); image row 0 is the tile's north edge."""
    def stage(index, texture, sx, pos):
        return (f"class Stage{index}\n{{\n\ttexture=\"{texture}\";\n\tuvSource=\"worldPos\";\n"
                f"\tclass uvTransform\n\t{{\n\t\taside[]={{{sx!r},0,0}};\n\t\tup[]={{0,0,0}};\n"
                f"\t\tdir[]={{0,{-sx!r},0}};\n\t\tpos[]={{{pos[0]!r},{pos[1]!r},0}};\n\t}};\n}};\n")
    out = ["ambient[]={1,1,1,1};\ndiffuse[]={1,1,1,1};\nforcedDiffuse[]={0,0,0,0};\nemmisive[]={0,0,0,0};\n"
           "specular[]={0,0,0,0};\nspecularPower=1;\nPixelShaderID=\"TerrainX\";\nVertexShaderID=\"Terrain\";\n"]
    out.append(stage(0, satellite, 1.0 / size, (-x0 / size, z_top / size)))
    out.append(stage(1, mask, 1.0 / size, (-x0 / size, z_top / size)))
    for k, (surface, normal) in enumerate(zip(surfaces, normals)):
        if normal:
            out.append(stage(3 + 2 * k, normal, 1.0 / SURFACE_TILE_M, (0.0, 0.0)))
        out.append(stage(4 + 2 * k, surface, 1.0 / SURFACE_TILE_M, (0.0, 0.0)))
    with open(path, "w", newline="\n") as f:
        f.write("".join(out))


def write_4wvr(path, heights_m, tex_idx, tex_names, objects=()):
    """Raw 4WVR (Landscape::LoadData, LandFile.hpp): '4WVR', xRange, zRange, int16 heights[z][x],
    int16 tex[z][x], 512 x 32-byte texture names, SingleObject4 records ended by an empty name."""
    q = np.round(np.asarray(heights_m, np.float64) / LANDDATA_SCALE)
    if q.min() < -32768 or q.max() > 32767:
        raise ValueError("height out of range")
    with open(path, "wb") as f:
        f.write(b"4WVR" + struct.pack("<ii", GRID, GRID))
        f.write(q.astype("<i2").tobytes())
        f.write(np.asarray(tex_idx, "<i2").tobytes())
        for i in range(LAND_TEXTURES_MAX):
            n = tex_names[i].encode("ascii") if i < len(tex_names) else b""
            if len(n) >= 32:
                raise ValueError(tex_names[i])
            f.write(n.ljust(32, b"\0"))
        forest_objects.write_objects(f, objects, LEN_OBJNAME_4)
        f.write(b"\0" * 96)  # terminator: a record with an empty name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", default=r"M:\ForestClaude\extract\terrain")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sea-level", type=float, default=40.0)
    ap.add_argument("--no-objects", action="store_true", help="terrain only (no trees, rocks or cliffs)")
    a = ap.parse_args()

    t = json.load(open(os.path.join(a.extract, "terrain.json")))
    cell = t["cell"][0]
    raw = np.load(os.path.join(a.extract, "height_u16.npy")).astype(np.float64)
    h = raw / t["heightmax_units"] * t["hscale"] + t["pos"][1] - a.sea_level
    h = h[:GRID, :GRID]  # 2049 samples -> 2048 cells (the last row/column is the far edge)

    alpha = np.concatenate([np.asarray(Image.open(os.path.join(a.extract, f"alpha{i}.png")), np.float32)
                            for i in (0, 1)], axis=2)[::-1] / 255.0  # stored upside down; rows now run south->north
    layers = alpha.shape[2]

    os.makedirs(os.path.join(a.out, "m"), exist_ok=True)
    surfaces, normals, colours, colour_map = [], [], [], None
    rtp = os.path.join(os.path.dirname(a.extract), "terrain_rtp")
    if os.path.exists(os.path.join(rtp, "rtp.json")):
        # The Forest's real ground: its Relief Terrain Pack textures (export_terrain_rtp.py). Layer n of an atlas
        # sits at UV offset ((n % 2) / 2, (n // 2) / 2) with Unity's origin bottom-left, so in the image layers
        # 0..3 are bottom-left, bottom-right, top-left, top-right (checked: layer 1 and layer 4 match their Unity
        # placeholders best). Normals pack two layers per texture, the even one in RG and the odd one in BA (the
        # sand layer, 4, is the flat one); Oli's surface normals want X in alpha and Y in green.
        rtp_info = json.load(open(os.path.join(rtp, "rtp.json")))
        quad = [(1, 0), (1, 1), (0, 0), (0, 1)]  # (row half, column half) of layer n % 4
        for n in range(layers):
            atlas = Image.open(os.path.join(rtp, f"Atlas{n // 4}.png")).convert("RGB")
            half = atlas.size[0] // 2
            r, c = quad[n % 4]
            atlas.crop((c * half, r * half, (c + 1) * half, (r + 1) * half)).save(os.path.join(a.out, f"t{n + 1}.png"))
            packed = np.asarray(Image.open(os.path.join(rtp, f"N_Atlas{n // 2}.png")).convert("RGBA"))
            x, y = (packed[:, :, 0], packed[:, :, 1]) if n % 2 == 0 else (packed[:, :, 2], packed[:, :, 3])
            nohq = np.stack([np.full_like(x, 128), y, np.full_like(x, 128), x], axis=2)
            Image.fromarray(nohq).save(os.path.join(a.out, f"t{n + 1}_n.png"))
            surfaces.append(f"{PREFIX}\\t{n + 1}.png")
            normals.append(f"{PREFIX}\\t{n + 1}_n.png")
            colours.append(np.asarray(Image.open(os.path.join(a.out, f"t{n + 1}.png")), np.float32).reshape(-1, 3).mean(0))
        # the island colour map, stored upside down like the blend maps (flipped it matches the layer colours,
        # r = 0.68; any other orientation <= 0.23)
        colour_map = np.asarray(Image.open(os.path.join(rtp, "CustomColorMap.png")).convert("RGB"), np.float32)[::-1]
        global SURFACE_TILE_M
        SURFACE_TILE_M = GRID * cell / rtp_info["floats"].get("_SplatTiling", GRID * cell / SURFACE_TILE_M)
    else:
        for s in t["splats"][:layers]:
            src = [f for f in os.listdir(a.extract) if f.startswith(f"splat{s['i']}_")]
            if not src:
                raise FileNotFoundError(f"splat{s['i']}")
            dst = f"t{s['i'] + 1}.png"
            shutil.copyfile(os.path.join(a.extract, src[0]), os.path.join(a.out, dst))
            surfaces.append(f"{PREFIX}\\{dst}")
            normals.append("")
            colours.append(np.asarray(Image.open(os.path.join(a.extract, src[0])).convert("RGB"), np.float32)
                           .reshape(-1, 3).mean(0))
    colours = np.array(colours)
    print(f"ground layers: {'Relief Terrain Pack' if colour_map is not None else 'Unity splat placeholders'}, "
          f"{SURFACE_TILE_M:.2f} m repeats")

    # The ground as authored terrain materials (Landscape::LoadRvmatTerrainMaterials, the Arma 2 "TerrainX" family):
    # one RVMAT per TILE x TILE cells with a satellite colour image, an LCA selector mask (terrain.wgsl decode_mask:
    # alpha 255 + RGB one-hot = slots 0..3, alpha 128 = slot 4, alpha 0 = slot 5) and the tile's six most used Forest
    # layers. Texture slot 0 is the engine's animated sea (Landscape::SetTexture forces it) and stays unnamed; tile k is
    # slot k + 1.
    tile_px = TILE * MASK_PX_PER_CELL
    tiles_per_side = GRID // TILE
    size_m = TILE * cell
    names = []
    tex = np.zeros((GRID, GRID), np.int16)
    rng = np.random.default_rng(4900)
    # the weights at mask resolution: bilinear up-sampling of the 512^2 blend maps over the whole island
    res = GRID * MASK_PX_PER_CELL
    src = alpha.shape[0]
    coord = (np.arange(res) + 0.5) * src / res - 0.5
    i0 = np.clip(np.floor(coord).astype(int), 0, src - 1)
    i1 = np.clip(i0 + 1, 0, src - 1)
    f = np.clip(coord - i0, 0, 1).astype(np.float32)
    layer_use = np.zeros(layers)
    if colour_map is not None:  # the same bilinear lookup into the colour map
        csrc = colour_map.shape[0]
        ccoord = (np.arange(res) + 0.5) * csrc / res - 0.5
        ci0 = np.clip(np.floor(ccoord).astype(int), 0, csrc - 1)
        ci1 = np.clip(ci0 + 1, 0, csrc - 1)
        cf = np.clip(ccoord - ci0, 0, 1).astype(np.float32)
    for tz in range(tiles_per_side):
        rows = slice(tz * tile_px, (tz + 1) * tile_px)
        rz0, rz1, fz = i0[rows], i1[rows], f[rows][:, None, None]
        band0 = alpha[rz0]
        band1 = alpha[rz1]
        for tx in range(tiles_per_side):
            cols = slice(tx * tile_px, (tx + 1) * tile_px)
            cx0, cx1, fx = i0[cols], i1[cols], f[cols][None, :, None]
            w = ((band0[:, cx0] * (1 - fx) + band0[:, cx1] * fx) * (1 - fz) +
                 (band1[:, cx0] * (1 - fx) + band1[:, cx1] * fx) * fz)  # [z][x][layer], z rows south->north
            use = w.sum(axis=(0, 1))
            chosen = list(np.argsort(-use)[:6])
            ws = w[:, :, chosen]
            ws = ws / np.maximum(ws.sum(axis=2, keepdims=True), 1e-6)
            # dithered selection: the filtered mask then crossfades the surfaces over about a metre
            r = rng.random(ws.shape[:2], dtype=np.float32)[:, :, None]
            slot = np.argmax(np.cumsum(ws, axis=2) > r, axis=2)
            mask = np.zeros(ws.shape[:2] + (4,), np.uint8)
            mask[:, :, 3] = 255
            mask[slot == 1, 0] = 255
            mask[slot == 2, 1] = 255
            mask[slot == 3, 2] = 255
            mask[slot == 4, 3] = 128
            mask[slot == 5, 3] = 0
            if colour_map is not None:
                cz0, cz1, cfz = ci0[rows], ci1[rows], cf[rows][:, None, None]
                cx0, cx1, cfx = ci0[cols], ci1[cols], cf[cols][None, :, None]
                sat = ((colour_map[cz0][:, cx0] * (1 - cfx) + colour_map[cz0][:, cx1] * cfx) * (1 - cfz) +
                       (colour_map[cz1][:, cx0] * (1 - cfx) + colour_map[cz1][:, cx1] * cfx) * cfz)
            else:
                sat = np.tensordot(w / np.maximum(w.sum(axis=2, keepdims=True), 1e-6), colours, axes=1)
            satellite = np.clip(sat, 0, 255).astype(np.uint8)
            k = tz * tiles_per_side + tx
            base = f"m\\{tx:02d}_{tz:02d}"
            Image.fromarray(mask[::-1]).save(os.path.join(a.out, "m", f"{tx:02d}_{tz:02d}_m.png"))  # row 0 = north
            Image.fromarray(satellite[::-1]).save(os.path.join(a.out, "m", f"{tx:02d}_{tz:02d}_s.png"))
            x0, z0 = tx * size_m, tz * size_m
            write_rvmat(os.path.join(a.out, "m", f"{tx:02d}_{tz:02d}.rvmat"), x0, z0 + size_m, size_m,
                        f"{PREFIX}\\{base}_s.png", f"{PREFIX}\\{base}_m.png", [surfaces[c] for c in chosen],
                        [normals[c] for c in chosen])
            names.append(f"{PREFIX}\\{base}.rvmat")
            tex[tz * TILE:(tz + 1) * TILE, tx * TILE:(tx + 1) * TILE] = k + 1
            layer_use += use
    hq = np.round(h / LANDDATA_SCALE) * LANDDATA_SCALE  # the heights as the WRP stores them

    def ground(x, z):  # bilinear over the stored grid, world metres (x east, z north)
        fx = np.clip(np.asarray(x) / cell, 0, GRID - 1.001)
        fz = np.clip(np.asarray(z) / cell, 0, GRID - 1.001)
        i, j = fx.astype(int), fz.astype(int)
        u, v = fx - i, fz - j
        i1, j1 = np.minimum(i + 1, GRID - 1), np.minimum(j + 1, GRID - 1)
        return (hq[j, i] * (1 - u) * (1 - v) + hq[j, i1] * u * (1 - v) + hq[j1, i] * (1 - u) * v + hq[j1, i1] * u * v)

    objects = [] if a.no_objects else forest_objects.build(os.path.dirname(a.extract), a.out, a.sea_level,
                                                             t["pos"][0], t["pos"][2], PREFIX, ground)
    write_4wvr(os.path.join(a.out, f"{WORLD}.wrp"), h, tex, [""] + names, objects)

    land = h > 0
    print(f"heights {h.min():.1f} .. {h.max():.1f} m over the sea, land {land.mean() * 100:.0f} %, cell {cell} m")
    print(f"{len(names)} terrain materials, {tile_px} px masks ({cell / MASK_PX_PER_CELL:.2f} m per texel)")
    print("layer share:", {t['splats'][i]['name']: round(float(layer_use[i] / layer_use.sum()), 3) for i in range(layers)})

    # a beach spawn for the test mission: the land cell nearest the island centre-south within 2-6 m of the sea that
    # is clear of every object (the first pick sat inside a 10 m boulder)
    zz, xx = np.nonzero((h > 2) & (h < 6))
    c = np.array([GRID / 2, GRID * 0.3])
    spawn = None
    for k in np.argsort(np.hypot(xx - c[0], zz - c[1]))[:5000]:
        if forest_objects.clear_of(objects, xx[k] * cell, zz[k] * cell):
            spawn = (float(xx[k] * cell), float(zz[k] * cell))
            break
    if spawn is None:
        raise RuntimeError("no clear beach cell for the spawn")
    with open(os.path.join(a.out, "config.cpp"), "w", newline="\n") as f:
        f.write(f"""// The Forest's island for Open Poseidon (Sinkhole W5) -- generated by scripts/forest/forest_terrain.py
// from the owner's own copy of The Forest; not distributable.
class CfgPatches
{{
\tclass {PREFIX}
\t{{
\t\tunits[] = {{}};
\t\tweapons[] = {{}};
\t\trequiredAddons[] = {{}};
\t}};
}};
class CfgWorlds
{{
\tclass DefaultWorld;
\tclass TheForest: DefaultWorld
\t{{
\t\tworldName = "\\{PREFIX}\\{WORLD}.wrp";
\t\tdescription = "The Forest";
\t\tLandGrid = {cell!r};
\t\tstartTime = "10:00";
\t\tstartDate = "21/6/2014";
\t\tstartWeather = 0.2;
\t\tforecastWeather = 0.2;
\t\tstartFog = 0;
\t\tforecastFog = 0;
\t\tcenterPosition[] = {{{GRID * cell / 2:.0f}, {GRID * cell / 2:.0f}, 0}};
\t\t// what missions read from every world (Eden defines them; DefaultWorld does not): without them a mission
\t\t// fails to start ("'/' not an array"). No airfield on the island: the ILS points are unused.
\t\tseagullPos[] = {{{spawn[0]:.0f}, {spawn[1]:.0f}}};
\t\tilsPosition[] = {{100, 100}};
\t\tilsDirection[] = {{0, 0.08, -1}};
\t\tilsTaxiIn[] = {{100, 300, 100, 100}};
\t\tilsTaxiOff[] = {{100, 100, 100, 300}};
\t\tclass Sounds {{ sounds[] = {{}}; }};
\t\tclass Animation {{ vehicles[] = {{}}; }};
\t\tclass Names {{}};
\t}};
}};
""")
    json.dump({"spawn": spawn, "sea_level": a.sea_level, "cell": cell}, open(os.path.join(a.out, "forest_info.json"), "w"))
    print("spawn", [round(v, 1) for v in spawn])


if __name__ == "__main__":
    sys.exit(main())
