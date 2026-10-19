"""Tidewater's lobster boat as an OFP ship (owner, 2026-10-19: "port the boat from Tidewater").

Build-time only. Reads the mesh export_tidewater_boat.mjs wrote from Tidewater's procedural BoatModel
(dgreenheck/tidewater, MIT, (c) DRG Software Solutions LLC: the boat is code -- hull lines, lofts and
primitives -- not a model file) and writes, into --out:

* tidewater_boat.p3d   MLOD: three visual LODs, the helm view (1100), geometry (convex boxes carrying the
                       mass), fire geometry, memory points and a ROADWAY LOD -- the decks a man stands on;
* boat_palette.png     the flat colours of every part (Tidewater paints them per vertex: one 8x8 cell each);
* boat_hull.png        the hull's paint by height (antifouling, boot stripe, white topsides, cove stripe),
                       mapped by height so the bands are as sharp as Tidewater's shader draws them;
* config.cpp           CfgVehicles OP_TidewaterBoat with Tidewater's hydrostatics (class TidewaterHull,
                       read by ShipTidewater.cpp): its 32 waterplane patches, inertia, thrust, rudder.

Frames: Tidewater's boat frame is +Z forward, +Y up, +X PORT (right-handed); OFP's model space is +X
starboard, +Z forward. The engine's MLOD loader turns a model half round about y ((x, y, z) -> (-x, y, -z):
measured 2026-10-19 from where it put the centre of mass), so the file stores Tidewater's (x, y, -z), which
it loads as (-x, y, z): a mirror in x, every triangle's winding reversed with it. The memory point
"tw_origin" is Tidewater's origin (the design waterline amidships), so the engine places the config's
boat-frame numbers in the model whatever its centring does.
"""

import argparse
import gzip
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

PREFIX = "op_tidewater_boat"
MODEL = "tidewater_boat.p3d"
PALETTE = "boat_palette"
HULL = "boat_hull"
DISPLAYS = "boat_displays"
PROPS = "boat_props"
# Tidewater draws these double-sided (the trap mesh, the glass): every triangle gets a back face with the opposite
# normal
DOUBLE_SIDED = ("trap", "glass")


def VISUAL_ORDER(a, b, c):
    """A visual triangle's vertex order in the file. The engine draws the side from which the file's order runs
    clockwise -- Tidewater's (three.js) order, kept through the mirror into the file. Reversing it for the mirror
    (the first version) drew the whole boat inside out: the near side of the hull culled and the inside of the
    far side showing through it, the displays and traps seen from behind (owner, 2026-10-19)."""
    return (a, b, c)

RES_GEOMETRY = 1e13
RES_MEMORY = 1e15
RES_ROADWAY = 3e15
RES_FIRE = 7e15
RES_VIEW_PILOT = 1100.0

# Tidewater BoatController: the "effective waterplane" its integrator uses (samples 0.79 of the beam and
# 0.9 of the length out from the centre of buoyancy), thrust, the rudder, the keel's lateral stations
# and its grounding points -- all boat frame.
SX, SZ = 0.79, 0.9
MAX_THRUST, PITCH_SPEED, REVERSE = 26000.0, 16.0, 0.45
STATIONS = [(-3.4, 0.73), (-2.3, 0.78), (-1.2, 0.8), (-0.1, 0.77), (1.0, 0.62), (2.1, 0.33), (3.2, 0.14)]
LATERAL_Y = 0.06
CONTACTS = [(0, -0.7, 3.2), (0, -0.75, 0), (0, -0.72, -3.4), (1.1, -0.4, 1.5), (-1.1, -0.4, 1.5), (1.2, -0.35, -2.5),
            (-1.2, -0.35, -2.5), (0, 0.2, 4.2)]
# Where the water renderer keeps the sea out of the hull (tw_water.wgsl insideHull): the open cockpit and the
# wheelhouse -- half beam at the sheer inside the 7 cm shell, the transom (inside) and the house front along z, the
# sheer height above the waterline at both ends. Forward of the house the foredeck closes the hull, and the stem is
# too fine for any outline guess: a mask there cut the sea outside the bow (owner, 2026-10-19). HullLines.sheerX /
# sheerY, measured 2026-10-19 (1.28 m at the transom, 1.45 at 40 %; sheer 1.02 aft, ~1.16 at the house front).
WATER_MASK = (1.40, -3.83, 1.45, 1.02, 1.16)
# Tidewater widens the roll radius for the gear on deck (inertia.z *= 1.24)
ROLL_GEAR = 1.24


def load(path):
    data = gzip.open(path).read()
    n = struct.unpack_from("<I", data)[0]
    header = json.loads(data[4:4 + n])
    base = 4 + n
    meshes = {}
    for name, m in header["meshes"].items():
        v = m["vertices"]
        meshes[name] = dict(
            pos=np.frombuffer(data, np.float32, v * 3, base + m["position"]).reshape(-1, 3).astype(np.float64),
            nrm=np.frombuffer(data, np.float32, v * 3, base + m["normal"]).reshape(-1, 3).astype(np.float64),
            col=np.frombuffer(data, np.float32, v * 3, base + m["color"]).reshape(-1, 3).astype(np.float64),
            aux=np.frombuffer(data, np.float32, v * 4, base + m["aux"]).reshape(-1, 4).astype(np.float64),
            uv=np.frombuffer(data, np.float32, v * 2, base + m["uv"]).reshape(-1, 2).astype(np.float64),
            idx=np.frombuffer(data, np.uint32, m["triangles"] * 3, base + m["index"]).reshape(-1, 3).astype(np.int64))
    return header, meshes


def mirror(p):
    """Tidewater boat frame -> MLOD file coordinates (the engine loads them turned about y: -x, y, -z)."""
    p = np.asarray(p, dtype=np.float64)
    return p * np.array([1.0, 1.0, -1.0])


def srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def hexlin(h):
    c = np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255]) / 255.0
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4))


# ---- textures ---------------------------------------------------------------------------------------

class Palette:
    """Flat colours, one 8x8 cell each in a 256x256 sheet (1024 cells)."""

    CELL, SIZE = 8, 256

    def __init__(self):
        self.index = {}
        self.cols = []

    def uv(self, rgb8):
        key = tuple(int(x) for x in rgb8)
        i = self.index.get(key)
        if i is None:
            i = len(self.cols)
            if i >= (self.SIZE // self.CELL) ** 2:
                raise RuntimeError("palette full")
            self.index[key] = i
            self.cols.append(key)
        per = self.SIZE // self.CELL
        x, y = i % per, i // per
        return ((x * self.CELL + self.CELL / 2) / self.SIZE, (y * self.CELL + self.CELL / 2) / self.SIZE)

    def image(self):
        img = np.zeros((self.SIZE, self.SIZE, 3), np.uint8)
        per = self.SIZE // self.CELL
        for i, c in enumerate(self.cols):
            x, y = i % per, i // per
            img[y * self.CELL:(y + 1) * self.CELL, x * self.CELL:(x + 1) * self.CELL] = c
        return Image.fromarray(img)


# hull paint (Tidewater BoatMaterials.createHull): bands by "boot" (height, sweeping up 6 cm to the bow) and
# a cove stripe 10-12 cm under the sheer. u = metres under the sheer, v = boot height.
HULL_U = (-0.2, 1.8)
HULL_V = (-0.9, 1.7)


def hull_coords(p, zaft, length):
    t = np.clip((p[:, 2] - zaft) / length, 0, 1)
    sheer = np.power(t, 2.2) * 0.62 + 0.98 + np.power(np.maximum(1 - t / 0.2, 0), 2) * 0.04
    lstep = np.clip((p[:, 2] - 0.8) / (4.3 - 0.8), 0, 1)
    boot = p[:, 1] - lstep * 0.06
    below = sheer - p[:, 1]
    u = (below - HULL_U[0]) / (HULL_U[1] - HULL_U[0])
    v = (HULL_V[1] - boot) / (HULL_V[1] - HULL_V[0])  # texture row 0 = the top
    return np.clip(u, 0.002, 0.998), np.clip(v, 0.002, 0.998)


def hull_image(size=512):
    u = (np.arange(size) + 0.5) / size * (HULL_U[1] - HULL_U[0]) + HULL_U[0]
    v = HULL_V[1] - (np.arange(size) + 0.5) / size * (HULL_V[1] - HULL_V[0])
    below, boot = np.meshgrid(u, v)
    red, navy, white = hexlin(0x7a1d15), hexlin(0x0f1a30), hexlin(0xf2efe6)
    c = np.where((boot >= 0.15)[..., None], white, np.where((boot >= 0.05)[..., None], navy, red))
    cove = (below >= 0.1) & (below <= 0.122)
    c = np.where(cove[..., None], navy, c)
    # waterline scum on the white just above the boot top, algae just under it (the shader's means)
    scum = ((boot >= 0.15) & (boot < 0.3))[..., None]
    c = np.where(scum, c * 0.9 + hexlin(0x8b7b57) * 0.1, c)
    algae = ((boot < 0.05) & (boot > -0.25))[..., None]
    c = np.where(algae, c * 0.85 + hexlin(0x2c3a1f) * 0.15, c)
    return Image.fromarray((srgb(c) * 255 + 0.5).astype(np.uint8))


def _value_noise(shape, cells, seed):
    """Smooth value noise in [0, 1] (bilinear over a random lattice), for the display art."""
    rng = np.random.default_rng(seed)
    g = rng.random((cells + 1, cells + 1))
    h, w = shape
    y = np.linspace(0, cells, h, endpoint=False)[:, None]
    x = np.linspace(0, cells, w, endpoint=False)[None, :]
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = g[y0, x0], g[y0, x0 + 1]
    c, d = g[y0 + 1, x0], g[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def _fractal(shape, seed):
    return sum(_value_noise(shape, 4 * 2 ** o, seed + o) * 0.5 ** o for o in range(4)) / 1.875


def display_atlas():
    """Tidewater's cockpit displays (BoatMaterials.createGlow) as one still frame, linear RGB in a 512 x 512 atlas:
    the radar PPI top left, the chart plotter top right, the gauge dial bottom left. OFP models carry no emissive
    channel, so the screens are painted at the brightness Tidewater gives them by day."""
    img = np.zeros((512, 512, 3))
    n = 256
    y, x = np.mgrid[0:n, 0:n]
    u, v = (x + 0.5) / n, 1 - (y + 0.5) / n
    # radar: head-up PPI, range rings, heading line, the sweep and its green trail, land and blips in amber
    qx, qy = (u - 0.5) * 2, (v - 0.5) * 2
    r = np.hypot(qx, qy)
    ang = np.arctan2(qx, qy)
    da = np.mod((0.9 - ang) / (2 * np.pi), 1.0)
    trail = np.exp(-da * 5.0)
    ring = np.clip(1 - (np.abs(np.mod(r * 3 + 0.5, 1) - 0.5) - 0.012) / 0.018, 0, 1)
    land = np.clip((_fractal((n, n), 7) + qx * 0.35 - 0.72) / 0.1, 0, 1) * np.clip((r - 0.3) / 0.15, 0, 1)
    heading = (np.abs(qx) < 0.01) & (qy > 0)
    inside = np.clip((0.99 - r) / 0.03, 0, 1)
    echoes = np.clip(land, 0, 1) * (trail * 0.75 + 0.15)
    radar = (np.array([0.0, 0.012, 0.04]) + np.array([0.05, 0.22, 0.28]) * (ring * 0.5 + heading * 0.6)[..., None]
             + np.array([1.0, 0.72, 0.12]) * echoes[..., None] + np.array([0.15, 0.9, 0.35]) * (trail * 0.05)[..., None])
    radar = (radar * inside[..., None] + 0.01) * 1.6
    img[0:n, 0:n] = radar
    # chart plotter: sea (deep / shallow), sand-coloured land, depth contours, magenta track, orange boat icon
    shore = _fractal((n, n), 21) * 1.4 - 0.75 + (u - 0.62) * 1.1
    is_land = shore > 0
    shallow = np.clip((shore + 0.25) / 0.25, 0, 1)
    contour = (np.abs(np.mod(shore * 9, 1) - 0.5) < 0.05) & ~is_land
    water = np.array([0.2, 0.42, 0.78]) * (1 - shallow[..., None]) + np.array([0.62, 0.82, 0.97]) * shallow[..., None]
    chart = np.where(is_land[..., None], np.array([0.88, 0.8, 0.55]), water) * (1 - contour[..., None] * 0.35)
    track = (np.abs(u - 0.5 - (v - 0.5) * 0.25) < 0.007) & (v > 0.5)
    chart = np.where(track[..., None], np.array([0.9, 0.1, 0.8]), chart)
    chart = np.where((np.hypot(u - 0.5, v - 0.5) < 0.022)[..., None], np.array([1.0, 0.35, 0.05]), chart)
    img[0:n, n:2 * n] = chart * 0.85
    # gauge dial: 24 ticks, an orange needle
    m = 128
    y, x = np.mgrid[0:m, 0:m]
    gx, gy = ((x + 0.5) / m - 0.5) * 2, (0.5 - (y + 0.5) / m) * 2
    gr, ga = np.hypot(gx, gy), np.arctan2(gx, gy)
    ticks = (np.abs(np.mod(ga / (2 * np.pi) * 24, 1) - 0.5) < 0.12) & (gr > 0.72) & (gr < 0.88) & (np.abs(ga) < 2.35)
    nd = np.array([np.sin(0.6), np.cos(0.6)])
    along = gx * nd[0] + gy * nd[1]
    perp = np.abs(gx * nd[1] - gy * nd[0])
    needle = (perp < 0.035) & (along > -0.1) & (along < 0.7)
    gauge = np.full((m, m, 3), 0.02)
    gauge[ticks] = [0.9, 0.95, 1.0]
    gauge[needle] = [1.0, 0.45, 0.08]
    gauge[gr > 0.97] = 0.08
    img[n:n + m, 0:m] = gauge
    return Image.fromarray((srgb(np.clip(img, 0, 1)) * 255 + 0.5).astype(np.uint8))


def _lstep(a, b, x):
    return np.clip((x - a) / (b - a), 0, 1)


def _hash21(x, y):
    return np.modf(np.abs(np.sin(x * 127.1 + y * 311.7) * 43758.5453))[0]


def props_atlas():
    """The wheelhouse's printed things and the ensign (BoatMaterials.createFittings patterns 2 and 5-8, which Tidewater
    draws in its fragment shader), baked as 256 x 256 tiles of a 1024 x 512 atlas: flag, tide-table page, folded
    chart, two photo prints, label tape. Tidewater's noise is stood in for by _fractal / _value_noise."""
    n = 256
    img = np.zeros((512, 1024, 3))
    y, x = np.mgrid[0:n, 0:n]
    u, v = (x + 0.5) / n, 1 - (y + 0.5) / n  # Tidewater's uv, v up

    def put(tile, rgb):
        tx, ty = tile % 4, tile // 4
        img[ty * n:(ty + 1) * n, tx * n:(tx + 1) * n] = rgb

    # 0: the ensign -- 13 stripes and the canton with its stars
    stripe = np.floor(np.clip(v, 0, 1) * 12.999)
    red = (1 - (stripe - 2 * np.floor(stripe / 2)))[..., None]
    canton = ((u <= 0.4) & (v >= 6 / 13))[..., None]
    sx = np.modf(u / 0.4 * 6.0)[0] - 0.5
    sy = np.modf((v - 6 / 13) / (7 / 13) * 5.0 + 5)[0] - 0.5
    star = (1 - _lstep(0.16, 0.24, np.hypot(sx, sy)))[..., None]
    stripes = hexlin(0xf4f1ea) * (1 - red) + hexlin(0xb3172a) * red
    put(0, np.where(canton, hexlin(0x1c2a5c) * (1 - star) + hexlin(0xf4f1ea) * star, stripes))
    # 1: printed page -- margins, a header block, rows of type, a table grid, a coffee ring
    row = np.modf(v * 34.0)[0]
    word = _hash21(np.floor(u * 16.0), np.floor(v * 34.0))
    in_text = (u > 0.08) & (u < 0.92) & (v > 0.06) & (v < 0.82)
    type_k = _lstep(0.35, 0.45, row) * (1 - _lstep(0.7, 0.8, row)) * (word > 0.18) * in_text
    header = ((v > 0.86) & (v < 0.93) & (u > 0.08) & (u < 0.6)).astype(float)
    grid = _lstep(0.478, 0.49, np.abs(np.modf(u * 4.0)[0] - 0.5)) * in_text
    ring = (1 - _lstep(0.0, 0.02, np.abs(np.hypot(u - 0.7, v - 0.3) - 0.16))) * 0.5
    ink = np.maximum(np.maximum(type_k * 0.7, header * 0.85), grid * 0.5)[..., None]
    paper = np.array([0.86, 0.84, 0.78]) * (1 - ink) + np.array([0.1, 0.1, 0.12]) * ink
    put(1, paper * (1 - ring[..., None]) + np.array([0.45, 0.3, 0.18]) * ring[..., None])
    # 2: folded paper chart -- sea and land, depth contours, fold creases, a pencilled course
    shore = _fractal((n, n), 41) * 2 - 1 + (u - 0.55) * 1.4
    land = _lstep(0.0, 0.02, shore)[..., None]
    contour = ((1 - _lstep(0.03, 0.06, np.abs(np.modf(np.abs(shore) * 7.0)[0] - 0.5))) * (1 - land[..., 0]))[..., None]
    crease = ((1 - _lstep(0, 0.006, np.abs(u - 0.5))) + (1 - _lstep(0, 0.006, np.abs(v - 0.5))))[..., None]
    course = ((1 - _lstep(0.001, 0.004, np.abs(v - 0.2 - u * 0.45))) * (u > 0.1) * (u < 0.7))[..., None]
    chart = np.array([0.72, 0.8, 0.84]) * (1 - land) + np.array([0.86, 0.78, 0.58]) * land
    chart = chart * (1 - contour * 0.25) * (1 - crease * 0.2)
    put(2, chart * (1 - course * 0.8) + np.array([0.2, 0.2, 0.22]) * course * 0.8)
    # 3, 4: photo prints -- a white border round a red boat on the sea under the sky (Tidewater's two seeds)
    for tile, w in ((3, 0.15), (4, 0.7)):
        in_pic = ((u > 0.07) & (u < 0.93) & (v > 0.07) & (v < 0.8))[..., None]
        horizon = 0.45 + np.sin(u * 3.0 + w * 6.0) * 0.03
        sky = (v >= horizon)[..., None]
        pic = np.array([0.08, 0.22, 0.35]) * (1 - sky) + np.array([0.45, 0.65, 0.85]) * sky
        hull = (1 - _lstep(0.06, 0.08, np.hypot(u - (0.4 + w * 0.2), (v - horizon) * 3.0)))[..., None]
        pic = pic * (1 - hull) + np.array([0.85, 0.25, 0.12]) * hull
        pic = pic * (_value_noise((n, n), 20, int(w * 100))[..., None] * 0.15 + 0.9)
        put(tile, np.where(in_pic, pic, np.array([0.92, 0.9, 0.86])))
    # 5: label tape over Tidewater's uv 0..0.3 -- white with blocks of black type
    lu, lv = u * 0.3, v * 0.3
    lt = (np.modf(lu * 7.0)[0] > 0.3) & (_hash21(np.floor(lu * 28.0), 3.0) > 0.3) & (lv > 0.075) & (lv < 0.225)
    put(5, np.where(lt[..., None], np.array([0.05, 0.05, 0.05]), np.array([0.85, 0.85, 0.82])))
    return Image.fromarray((srgb(np.clip(img, 0, 1)) * 255 + 0.5).astype(np.uint8))


# fittings pattern -> atlas tile (the two photo prints told apart by their seed, aux.w)
PROP_TILE = {2: 0, 5: 1, 6: 2, 7: 3, 8: 5}
PROP_UV_SCALE = {8: 1 / 0.3}


def prop_uv(pattern, uv, seed):
    tile = 4 if pattern == 7 and seed > 0.4 else PROP_TILE[pattern]
    k = PROP_UV_SCALE.get(pattern, 1.0)
    pad = 2 / 256
    u = np.clip(uv[0] * k, 0, 1) * (1 - 2 * pad) + pad
    v = (1 - np.clip(uv[1] * k, 0, 1)) * (1 - 2 * pad) + pad
    return ((tile % 4 + u) / 4, (tile // 4 + v) / 2)


# ---- surface relief (Tidewater draws it per pixel: BoatMaterials.createGelcoat / createWood) ------------------------
# OFP's boat has no normal maps (a face samples one palette cell, so a tiling normal map would have no uv to tile on);
# the relief is baked into tiling albedo textures with real uvs instead, shaded from a light up and to the left, one per
# kind and face colour: molded non-skid (gelcoat 1), the wheelhouse's painted panels with screwed seams (gelcoat 2),
# the perforated quilted headliner (gelcoat 3), varnished wood grain (wood 0) and the teak-and-holly sole (wood 2).

RELIEF_TILE = {  # kind: (texture w, h, metres across u, metres across v)
    "nonskid": (256, 256, 16 / 55, 16 / 55),
    "panel": (128, 256, 0.61, 1.4),
    "headliner": (256, 256, 0.5, 1.0),
    "wood": (256, 256, 1.25, 5 * 2 * np.pi / 150),
    "teak": (256, 256, 1.8, 0.4),
}


def _periodic_noise(h, w, cells_x, cells_y, seed):
    """Smooth value noise in [0, 1] that wraps at the tile's edges (a tiling texture has no seam)."""
    rng = np.random.default_rng(seed)
    g = rng.random((cells_y, cells_x))
    y = np.linspace(0, cells_y, h, endpoint=False)[:, None]
    x = np.linspace(0, cells_x, w, endpoint=False)[None, :]
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    x1, y1 = (x0 + 1) % cells_x, (y0 + 1) % cells_y
    a, b, c, d = g[y0, x0], g[y0, x1], g[y1, x0], g[y1, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def _relief_shade(height, scale):
    """Light from up and to the left across a height field (pixels): a baked bump."""
    gy, gx = np.gradient(height)
    return np.clip(1 + (-gx - gy) * scale, 0.6, 1.4)


def relief_image(kind, rgb):
    """Tidewater's pattern for `kind` over its tile, times the face colour (linear rgb)."""
    w, h, su, sv = RELIEF_TILE[kind]
    y, x = np.mgrid[0:h, 0:w]
    u, v = (x + 0.5) / w * su, (y + 0.5) / h * sv  # metres
    rgb = np.asarray(rgb, dtype=np.float64)
    if kind == "nonskid":
        q = np.stack([u * 55, v * 55], -1)
        c = np.floor(q)
        f = q - c - 0.5
        jx = (_hash21(c[..., 0] % 16, c[..., 1] % 16) - 0.5) * 0.3
        jy = (_hash21(c[..., 0] % 16 + 17.31, c[..., 1] % 16 + 17.31) - 0.5) * 0.3
        pebble = 1 - _lstep(0.16, 0.36, np.hypot(f[..., 0] - jx, f[..., 1] - jy))
        dirt = _periodic_noise(h, w, 4, 4, 3) * 0.22 * 0.6
        out = rgb * ((1 - pebble * 0.07) * (1 - dirt))[..., None] * _relief_shade(pebble, 1.6)[..., None]
    elif kind == "panel":
        # one 0.61 m panel from the seam at its left edge; v from 0.9 m up to 2.3 m (seams at 1.17 m and 1.52 m)
        y_m = 0.9 + v
        seam_d = np.minimum(np.minimum(u, su - u), np.minimum(np.abs(y_m - 1.17), np.abs(y_m - 1.52)))
        seam = 1 - _lstep(0.0015, 0.004, seam_d)
        sv_ = np.hypot((np.modf(y_m / 0.15)[0] - 0.5) * 0.15, np.minimum(u, su - u) - 0.012)
        sh_ = np.hypot((np.modf(u / 0.15)[0] - 0.5) * 0.15, np.minimum(np.abs(y_m - 1.17), np.abs(y_m - 1.52)) - 0.012)
        screw = 1 - _lstep(0.0035, 0.0055, np.minimum(sv_, sh_))
        mottle = _periodic_noise(h, w, 2, 4, 11)
        low = 1 - _lstep(1.15, 1.6, y_m)
        grime = np.clip(low * (0.35 + mottle * 0.3) + (1 - _lstep(0, 0.035, seam_d)) * 0.18 + mottle * 0.08, 0, 1)
        pc = rgb * np.array([0.97, 0.94, 0.87])
        pc = pc * (1 - grime * 0.6)[..., None]
        pc = pc * (1 - seam * 0.7)[..., None]
        pc = pc * (1 - screw[..., None]) + np.array([0.55, 0.55, 0.53]) * screw[..., None]
        out = pc * _relief_shade(screw - seam, 1.2)[..., None]
    elif kind == "headliner":
        perf = (1 - _lstep(0.12, 0.3, np.hypot(np.modf(u / 0.006)[0] - 0.5, np.modf(v / 0.006)[0] - 0.5))) * 0.5
        quilt = np.sin((v + 0.1) / 0.5 * np.pi) * 0.5 + 0.5
        mottle = _periodic_noise(h, w, 3, 6, 21)
        out = (np.array([0.72, 0.69, 0.63]) * ((1 - perf * 0.18) * (quilt * 0.12 + 0.88) * (mottle * 0.08 + 0.96))[..., None]
               * _relief_shade(quilt * 8, 0.5)[..., None])
    else:
        g1 = _periodic_noise(h, w, 1, 6, 31)  # Tidewater: noise(u * 0.8, v * 30)
        g2 = _periodic_noise(h, w, 12, 36, 32)
        rings = np.sin(v * 150 + g1 * 5 + u * 0.6 * 0) * 0.5 + 0.5
        grain = np.clip(rings * 0.45 + g2 * 0.22 + g1 * 0.22 + 0.28, 0, 1)[..., None]
        if kind == "wood":
            out = (hexlin(0x4a230f) * (1 - grain) + hexlin(0x9c5b2b) * grain) * rgb
        else:
            pv = v / 0.1
            plank = np.floor(pv)
            pf = np.abs(np.modf(pv)[0] - 0.5)
            holly = _lstep(0.5 - 0.06 - 0.01, 0.5 - 0.06, pf)[..., None]
            caulk = _lstep(0.5 - 0.012 - 0.008, 0.5 - 0.012, pf)
            butt = 1 - _lstep(0, 0.006, np.abs(np.modf(u / 1.8 + _hash21(plank, 3.0))[0] - 0.5) * 1.8)
            tone = (_hash21(plank, 7.0) * 0.25 + 0.85)[..., None]
            teak = (hexlin(0x6b3f1d) * (1 - grain) + hexlin(0xa87445) * grain) * tone
            teak = teak * (1 - holly * (1 - caulk[..., None])) + hexlin(0xd9c9a6) * holly * (1 - caulk[..., None])
            dark = np.maximum(caulk, butt)[..., None]
            out = (teak * (1 - dark) + hexlin(0x1c140e) * dark) * rgb * _relief_shade(-dark[..., 0], 1.5)[..., None]
    return Image.fromarray((srgb(np.clip(out, 0, 1)) * 255 + 0.5).astype(np.uint8))


relief_textures = {}


def relief_texture(kind, rgb8):
    key = (kind, tuple(int(c) for c in rgb8))
    if key not in relief_textures:
        relief_textures[key] = f"boat_{kind}_{len([k for k in relief_textures if k[0] == kind])}"
    return relief_textures[key]


RELIEF_KIND = {("gelcoat", 1): "nonskid", ("gelcoat", 2): "panel", ("gelcoat", 3): "headliner", ("wood", 0): "wood",
               ("wood", 2): "teak"}


def relief_uv(kind, uv, pos, nrm):
    """The face's uv on its relief tile: Tidewater's uv (metres) for the non-skid and the wood, the model position for
    the panels (along the wall, up) and the headliner (across, along)."""
    w, h, su, sv = RELIEF_TILE[kind]
    if kind == "panel":
        along = pos[2] if abs(nrm[0]) >= abs(nrm[2]) else pos[0]
        return ((along + 0.13) / su, (pos[1] - 0.9) / sv)
    if kind == "headliner":
        return (pos[0] / su, pos[2] / sv)
    return (uv[0] / su, uv[1] / sv)

# display uv rectangles in the atlas (u0, v0, size); Tidewater's uv has v up
DISPLAY_RECT = {1: (0.0, 0.0, 0.5), 2: (0.5, 0.0, 0.5), 3: (0.0, 0.5, 0.25)}


def display_uv(mode, uv):
    u0, v0, size = DISPLAY_RECT[mode]
    pad = 2 / 512
    u = np.clip(uv[0], 0, 1) * (size - 2 * pad) + u0 + pad
    v = (1 - np.clip(uv[1], 0, 1)) * (size - 2 * pad) + v0 + pad
    return (u, v)


# the traps' vinyl-coated wire (pattern 0: a 38 mm square grid of 9 mm wires) and twine netting (pattern 1: 50 mm
# diamonds): one tile repeats over the trap faces, whose uv is in metres (BoatMaterials.createTrap)
TRAP_TILE = {0: 0.152, 1: 0.1414}


def trap_tile(rgb8, pattern, size=64):
    y, x = np.mgrid[0:size, 0:size]
    u, v = (x + 0.5) / size * TRAP_TILE[pattern], (y + 0.5) / size * TRAP_TILE[pattern]
    if pattern == 0:
        fu, fv = np.abs(np.mod(u / 0.038, 1) - 0.5), np.abs(np.mod(v / 0.038, 1) - 0.5)
        solid = (fu > 0.5 - 0.0045 / 0.038) | (fv > 0.5 - 0.0045 / 0.038)
    else:
        a, b = (u + v) * 0.7071 / 0.05, (u - v) * 0.7071 / 0.05
        fa, fb = np.abs(np.mod(a, 1) - 0.5), np.abs(np.mod(b, 1) - 0.5)
        solid = (fa > 0.5 - 0.06) | (fb > 0.5 - 0.06)
    img = np.zeros((size, size, 4), np.uint8)
    img[..., :3] = rgb8
    img[..., 3] = np.where(solid, 255, 0)
    return Image.fromarray(img)


WOOD = (hexlin(0x4a230f) + hexlin(0x9c5b2b)) * 0.5
TEAK = (hexlin(0x6b3f1d) + hexlin(0xa87445)) * 0.5


def face_colours(name, m):
    """Linear albedo per triangle, the shader's base colour without its noise."""
    col, aux, idx = m["col"], m["aux"], m["idx"]
    c = col[idx].mean(axis=1)
    pat = np.rint(aux[idx[:, 0], 2])
    if name == "wood":
        c = np.where((pat == 1)[:, None], c, np.where((pat == 2)[:, None], TEAK * c, WOOD * c))
    elif name == "gelcoat":
        c = np.where((pat == 2)[:, None], c * np.array([0.97, 0.94, 0.87]) * 0.85, c)
        c = np.where((pat == 3)[:, None], np.array([0.72, 0.69, 0.63]), c)
        c = np.where((pat == 1)[:, None], c * 0.9, c)  # non-skid: grimier
    elif name == "glow":
        # nav lights, flood and dome lights, LCDs: their colour at full strength (no emissive channel here)
        c = np.clip(c * 1.2, 0, 1)
    elif name == "glass":
        c = np.tile(hexlin(0xa9bec4), (len(idx), 1))
    return c


# ---- P3D writer -------------------------------------------------------------------------------------

def lod_bytes(points, normals, faces, resolution, selections=None, mass=None, properties=(), flags=None):
    """One P3DM 28/256 LOD. faces: [(point indices, normal indices, uvs, texture)]; selections:
    {name: (points, faces)}."""
    np_, nf = len(points), len(faces)
    out = [b"P3DM", struct.pack("<iiiiii", 28, 256, np_, max(len(normals), 1), nf, 0)]
    flags = flags or {}
    out.append(b"".join(struct.pack("<fffi", float(p[0]), float(p[1]), float(p[2]), flags.get(i, 0)) for i, p in enumerate(points)))
    out.append(b"".join(struct.pack("<fff", float(n[0]), float(n[1]), float(n[2])) for n in normals) if len(normals)
               else struct.pack("<fff", 0.0, 1.0, 0.0))
    for pidx, nidx, uvs, tex in faces:
        rec = [struct.pack("<i", 3)]
        for i in range(4):
            if i < 3:
                rec.append(struct.pack("<iiff", int(pidx[i]), int(nidx[i]), float(uvs[i][0]), float(uvs[i][1])))
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


class Lod:
    def __init__(self):
        self.points, self.normals, self.faces, self.sel = [], [], [], {}
        self.flags = {}  # point index -> MLOD point flags

    def add_points(self, pts):
        base = len(self.points)
        self.points.extend(pts)
        return base

    def select(self, name, pts=(), faces=()):
        sp, sf = self.sel.setdefault(name, ([], []))
        sp.extend(pts)
        sf.extend(faces)

    def bytes(self, resolution, **kw):
        return lod_bytes(self.points, self.normals, self.faces, resolution, self.sel, flags=self.flags, **kw)


def trap_texture(rgb8, pattern, registry):
    """The name of the netting texture of this colour and pattern (registered for writing)."""
    key = (tuple(int(c) for c in rgb8), min(int(pattern), 1))
    if key not in registry:
        registry[key] = f"boat_trap_{len(registry)}"
    return registry[key]


trap_textures = {}


def add_mesh(lod, name, m, keep, palette, hull_frame):
    """Visual triangles of one Tidewater bucket (triangles where keep is true)."""
    tris = m["idx"][keep]
    if not len(tris):
        return
    used = np.unique(tris)
    remap = np.full(len(m["pos"]), -1, np.int64)
    base = len(lod.points)
    remap[used] = np.arange(len(used)) + base
    pos = m["pos"][used]
    if name == "glow":
        # every glowing part (screens, dials, lamps) is "shining" (POINT_NOLIGHT, Data3D.h): lit by itself, as
        # Tidewater's emissive is
        for k in range(len(used)):
            lod.flags[base + k] = 0x10
    lod.points.extend(mirror(pos))
    # MLOD stores normals pointing into the surface (O2's convention: with Tidewater's outward normals the whole
    # boat was lit from behind, black, once the winding was right)
    lod.normals.extend(-mirror(m["nrm"][used]))
    count = len(used)
    if name in DOUBLE_SIDED:
        # the back faces: the same points again, their normals facing the other way (point and normal lists stay
        # index for index)
        lod.points.extend(mirror(m["pos"][used]))
        lod.normals.extend(mirror(m["nrm"][used]))
        count *= 2
    first_face = len(lod.faces)
    if name == "hull":
        u, v = hull_coords(m["pos"], *hull_frame)
        tex = f"{PREFIX}\\{HULL}.paa"
        for a, b, c in tris:
            order = VISUAL_ORDER(a, b, c)
            lod.faces.append(([remap[i] for i in order], [remap[i] for i in order], [(u[i], v[i]) for i in order], tex))
    else:
        cols = face_colours(name, m)[keep]
        rgb8 = np.clip(srgb(cols) * 255 + 0.5, 0, 255).astype(int)
        pattern = np.rint(m["aux"][tris[:, 0], 2]).astype(int)
        for (a, b, c), col, pat in zip(tris, rgb8, pattern):
            order = VISUAL_ORDER(a, b, c)
            if name == "glass":
                tex, uvs = f"{PREFIX}\\{PALETTE}_glass.paa", [(0.5, 0.5)] * 3
            elif name == "glow" and pat in DISPLAY_RECT:
                tex, uvs = f"{PREFIX}\\{DISPLAYS}.paa", [display_uv(pat, m["uv"][i]) for i in order]
            elif name == "fittings" and pat in PROP_TILE:
                tex = f"{PREFIX}\\{PROPS}.paa"
                uvs = [prop_uv(pat, m["uv"][i], m["aux"][i][3]) for i in order]
            elif name == "trap":
                tex = f"{PREFIX}\\{trap_texture(col, pat, trap_textures)}.paa"
                uvs = [(m["uv"][i][0] / TRAP_TILE[min(pat, 1)], m["uv"][i][1] / TRAP_TILE[min(pat, 1)]) for i in order]
            elif (name, int(pat)) in RELIEF_KIND:
                kind = RELIEF_KIND[(name, int(pat))]
                tex = f"{PREFIX}\\{relief_texture(kind, col)}.paa"
                uvs = [relief_uv(kind, m["uv"][i], mirror(m["pos"][i]) * [1, 1, -1], m["nrm"][i]) for i in order]
            else:
                tex, uvs = f"{PREFIX}\\{PALETTE}.paa", [palette.uv(col)] * 3
            pidx = [remap[i] for i in order]
            lod.faces.append((pidx, pidx, uvs, tex))
            if name in DOUBLE_SIDED:
                back = [pidx[0], pidx[2], pidx[1]]
                back = [i + len(used) for i in back]
                lod.faces.append((back, back, [uvs[0], uvs[2], uvs[1]], tex))
    sel = {"wheel": "tw_wheel", "radar": "radar"}.get(name)
    if sel:
        lod.select(sel, range(base, base + count), range(first_face, len(lod.faces)))
    if name == "fittings":
        # the ensign (pattern 2): Ship.cpp flies it in the apparent wind as Tidewater's vertex shader does
        flag = np.rint(m["aux"][tris[:, 0], 2]).astype(int) == 2
        if flag.any():
            pts = sorted({int(remap[i]) for t in tris[flag] for i in t})
            lod.select("tw_flag", pts, [first_face + k for k in np.nonzero(flag)[0]])


def small_parts(m, limit):
    """Triangles of connected parts whose bounding box diagonal is under limit (m)."""
    idx = m["idx"]
    parent = np.arange(len(m["pos"]))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b, c in idx:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    roots = np.array([find(i) for i in range(len(parent))])
    lo, hi = {}, {}
    for r, p in zip(roots, m["pos"]):
        lo[r] = np.minimum(lo.get(r, p), p)
        hi[r] = np.maximum(hi.get(r, p), p)
    small = {r for r in lo if np.linalg.norm(hi[r] - lo[r]) < limit}
    return np.array([roots[a] in small for a, _, _ in idx])


def box(lo, hi):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2), (0, 4, 7), (0, 7, 3), (1, 2, 6),
         (1, 6, 5)]
    return v, f  # outward-facing (b - a) x (c - a)


def geometry_boxes(header, thick=True):
    """Convex boxes (OFP frame lo/hi): Tidewater's character colliders that are solid, and the hull below
    the cockpit sole, which Tidewater never needed (its sea is never walked on)."""
    out = []
    for c in header["colliders"]:
        if not c["solid"]:
            continue
        # The deck gear (traps, bait barrel, hauler, bench, helm seat) is not collision for OFP's man: his
        # geometry is far wider than Tidewater's capsule, and in the cockpit's narrow gaps every step out of
        # the helm wedged him between gear and wall and pushed him up and over the side (2026-10-19). The
        # walls that hold him on the deck stay; the gear can be walked through.
        if thick and c["tag"] in ("trap", "baitBarrel", "hauler", "bench", "helmSeat"):
            continue
        ctr, half = np.array(c["center"]), np.array(c["half"])
        lo0, hi0 = ctr - half, ctr + half
        # The walls that hold a man on the deck reach 0.4 m outboard (their inner faces stay): Tidewater's are
        # its character controller's thin boxes (bulwarks 0.26 m, house walls 0.04 m), and OFP's man, running
        # at 3.8 m/s, passed the middle of one in a few frames and was pushed out of its far side -- overboard
        # (2026-10-19). Not further: they also hold a swimmer off the hull (0.8 m kept him a hand's reach away).
        if not thick:
            pass
        elif c["tag"] in ("bulwark", "houseWall"):
            if ctr[0] > 0:
                hi0[0] += 0.4
            else:
                lo0[0] -= 0.4
        elif c["tag"] == "transom":
            lo0[2] -= 0.4
        lo, hi = mirror(lo0), mirror(hi0)
        out.append((c["tag"], np.minimum(lo, hi), np.maximum(lo, hi)))
    for z0, z1, hx, y0 in [(-3.86, -1.0, 1.24, -0.75), (-1.0, 1.5, 1.22, -0.75), (1.5, 3.0, 0.95, -0.55), (3.0, 4.1, 0.5, -0.25)]:
        # (top well under the 0.35 m sole: a man standing on the deck must not touch it)
        lo, hi = mirror([-hx, y0, z0]), mirror([hx, 0.10, z1])
        out.append(("hull", np.minimum(lo, hi), np.maximum(lo, hi)))
    return out


def solve_masses(points, total, com):
    """Point masses as even as possible (maximum entropy: w_i = exp(lambda . (1, x, y, z)), all positive)
    whose sum is total and whose centre is com."""
    p = np.asarray(points, dtype=np.float64)
    phi = np.hstack([np.ones((len(p), 1)), p])
    b = np.array([total, total * com[0], total * com[1], total * com[2]])
    lam = np.array([np.log(total / len(p)), 0.0, 0.0, 0.0])
    for _ in range(200):
        w = np.exp(phi @ lam)
        r = phi.T @ w - b
        if np.abs(r).max() < 1e-6 * total:
            return w
        lam -= np.linalg.solve(phi.T @ (phi * w[:, None]), r)
    raise RuntimeError("mass distribution did not converge")


def roadway_quads(header):
    """Top faces a man stands on: the cockpit sole and every walkable box top (Tidewater's 'walkable')."""
    quads = []
    for c in header["colliders"]:
        if not c["walkable"]:
            continue
        ctr, half = np.array(c["center"]), np.array(c["half"])
        y = ctr[1] + half[1]
        x0, x1 = ctr[0] - half[0], ctr[0] + half[0]
        z0, z1 = ctr[2] - half[2], ctr[2] + half[2]
        quads.append(mirror(np.array([[x0, y, z0], [x1, y, z0], [x1, y, z1], [x0, y, z1]])))
    return quads


def build(header, meshes):
    palette = Palette()
    zaft = -3.9
    hull_frame = (zaft, header["dimensions"]["length"])
    a = header["anchors"]
    lods = []

    # ---- the driver proxy: at the helm, standing on the sole facing forward (p0 origin, p0->p1 the
    # shortest edge = forward, p0->p2 = up)
    helm = np.array(a["helmEye"])
    # OFP's ship-driver pose (ManActShipDriver) puts the eye 0.32 m to the helmsman's right, 1.34 m up and 0.16 m
    # forward of the proxy (WGR_PROXYCAM_TRACE pilotPos). The proxy goes where that eye lands on Tidewater's helm
    # eye: with the proxy on the helm itself the eye hung straight over the wheel, which then showed only as its
    # hub at the bottom of the view (owner: "the steering wheel is missing").
    EYE_OFFSET = np.array([-0.32, 1.34, 0.16])  # Tidewater's frame (its +x is the helmsman's left)
    feet = helm - EYE_OFFSET
    # ... but sideways on the wheel's centre line: the body behind the wheel, not half a metre to port of it with the
    # eye over the hub (owner, 2026-10-19: "make sure the player is behind the wheel when he is driving")
    feet[0] = a["wheelCenter"][0]
    # (a proxy's frame is taken from the file's points without the loader's half turn: its forward edge points to
    # the file's +z, which is Tidewater's -z -- measured with WGR_PROXYCAM_TRACE, the helmsman faced aft)
    def crew_proxy(at, forward):
        """p0 at `at`, the forward edge along `forward` and the up edge, Tidewater's frame. Measured: the proxy's
        frame is built from the file's points without the loader's half turn, so its edges are written as the
        engine should read them -- a file +z edge is forward (Tidewater's -z), a file +x edge is engine +x
        (Tidewater's -x, inboard from the port side)."""
        at = np.asarray(at, dtype=np.float64)
        f = np.asarray(forward, dtype=np.float64) * 0.1
        return [mirror(at), mirror(at) + np.array([f[0], 0.0, f[2]]), mirror(at + [0, 0.2, 0])]

    proxies = {"proxy:Driver.01": crew_proxy(feet, [0, 0, 1])}
    # Passengers (driver -> passenger with the engine's "move to cargo"): on the bench along the port side,
    # facing inboard, and on the bait barrel's lid aft of it
    for i, z in enumerate((0.25, 0.65, -0.8)):
        proxies[f"proxy:Cargo.{i + 1:02d}"] = crew_proxy([0.86, header["dimensions"]["deckHeight"], z], [1, 0, 0])

    def visual(level_keep, resolution, with_view_points=False):
        lod = Lod()
        for name, m in meshes.items():
            keep = level_keep(name, m)
            if keep is None:
                continue
            add_mesh(lod, name, m, keep, palette, hull_frame)
        for pname, tri in proxies.items():
            base = lod.add_points(tri)
            lod.normals.extend([(0, 1, 0)] * 3)
            lod.faces.append(([base, base + 1, base + 2], [base, base + 1, base + 2], [(0, 0)] * 3, ""))
            lod.select(pname, [base, base + 1, base + 2], [len(lod.faces) - 1])
        if with_view_points:
            add_named_points(lod, view_points(a))
        lods.append((lod, resolution, {}))
        return len(lod.faces)

    everything = lambda name, m: np.ones(len(m["idx"]), bool)
    fittings_small = small_parts(meshes["fittings"], 0.15)
    detail = lambda name, m: (None if name in ("radar", "throttle", "glow") else
                              ~fittings_small if name == "fittings" else np.ones(len(m["idx"]), bool))
    hull_only = lambda name, m: np.ones(len(m["idx"]), bool) if name in ("hull", "gelcoat", "wood", "wheel") else None
    counts = [visual(everything, 1.0), visual(detail, 3.0), visual(hull_only, 10.0), visual(everything, RES_VIEW_PILOT, True)]

    # ---- geometry (mass) and fire geometry: convex boxes
    boxes = geometry_boxes(header)

    def box_lod(boxes=boxes):
        lod = Lod()
        for i, (_, lo, hi) in enumerate(boxes):
            v, f = box(lo, hi)
            base = lod.add_points(v)
            first = len(lod.faces)
            for t in f:  # the engine's geometry convention: faces written inward
                lod.faces.append(([base + t[0], base + t[2], base + t[1]], [0, 0, 0], [(0, 0)] * 3, ""))
            lod.select(f"Component{i + 1:02d}", range(base, base + 8), range(first, len(lod.faces)))
        return lod

    geo = box_lod()
    com = mirror(header["hydro"]["centerOfMass"])
    mass = solve_masses(geo.points, header["hydro"]["suggestedMass"], com)
    lods.append((geo, RES_GEOMETRY, dict(mass=list(mass), properties=[("class", "vehicle"), ("map", "")])))
    # bullets hit the walls where they are (the geometry's are thickened outboard for the man on deck)
    lods.append((box_lod(geometry_boxes(header, thick=False)), RES_FIRE, {}))

    # ---- roadway: both windings, so the from-above test finds every quad whatever the loader's order
    road = Lod()
    for q in roadway_quads(header):
        base = road.add_points(list(q))
        for t in [(0, 1, 2), (0, 2, 3), (0, 2, 1), (0, 3, 2)]:
            road.faces.append(([base + t[0], base + t[1], base + t[2]], [0, 0, 0], [(0, 0)] * 3, ""))
    lods.append((road, RES_ROADWAY, {}))

    # ---- memory
    mem = Lod()
    add_named_points(mem, memory_points(header))
    add_named_points(mem, nav_lights(meshes))
    lods.append((mem, RES_MEMORY, {}))

    # Written already centred on the box of every LOD's points, which is where the engine puts the model
    # origin: an MLOD authored off-centre is drawn shifted against its own memory points and collision
    # (measured 2026-10-19: the boat rendered 2.05 m under the waterline its physics floated it on).
    allp = np.array([p for lod, _, _ in lods for p in lod.points], dtype=np.float64)
    centre = (allp.min(axis=0) + allp.max(axis=0)) * 0.5
    for lod, _, _ in lods:
        lod.points = [np.asarray(p, dtype=np.float64) - centre for p in lod.points]
    blobs = [lod.bytes(res, **kw) for lod, res, kw in lods]
    data = b"MLOD" + bytes([1, 1, 0, 0]) + struct.pack("<I", len(blobs)) + b"".join(blobs)
    return data, palette, counts, boxes, mass, centre


def add_named_points(lod, named):
    for name, pts in named.items():
        base = lod.add_points([mirror(p) for p in pts])
        lod.select(name, range(base, base + len(pts)))


def view_points(a):
    c, ax = np.array(a["wheelCenter"]), np.array(a["wheelAxis"])
    return {"pilot": [a["helmEye"]], "osaVolantZac": [c - ax * 0.15], "osaVolantKon": [c]}


def nav_lights(meshes):
    """The navigation lights' lenses (Tidewater's glow parts of pattern 0, coloured per light): red to port, green to
    starboard, white at the masthead and on the stern -- the engine's marker lights (memory "cerveny / zeleny / bily
    pozicni", VehicleAI.cpp), lit with the boat's lights (an AI skipper at night, the player's lights action)."""
    gl = meshes["glow"]
    nav = np.rint(gl["aux"][:, 2]).astype(int) == 0
    pos, col = gl["pos"][nav], gl["col"][nav]
    red = pos[(col[:, 0] > 0.8) & (col[:, 1] < 0.3)]
    green = pos[(col[:, 1] > 0.8) & (col[:, 0] < 0.3)]
    white = pos[(col[:, 0] > 0.8) & (col[:, 1] > 0.8)]
    mast, stern = white[white[:, 1] > white[:, 1].max() - 0.2], white[white[:, 2] < white[:, 2].min() + 0.2]
    return {"cerveny pozicni": [red.mean(axis=0)], "zeleny pozicni": [green.mean(axis=0)],
            "bily pozicni": [mast.mean(axis=0), stern.mean(axis=0)]}


def memory_points(header):
    a = header["anchors"]
    # Tidewater boards at (0, deck, -1.75), between the lobster traps; OFP's man is wider than its capsule, so the
    # door is half a metre forward on open deck, toward the helm (starboard, -x)
    board = np.array(a["boardPoint"]) + [-0.25, 0, 0.5]
    radar = np.array(a["radarPivot"])
    out = dict(view_points(a))
    out.update({
        "tw_origin": [[0.0, 0.0, 0.0]],
        "pos driver": [board], "pos driver dir": [board + [0, 0, 0.7]],
        "pos cargo": [board], "pos cargo dir": [board + [0, 0, 0.7]],
        "osa radaru": [radar, radar + [0, 0.3, 0]],
        # the helm wheel's axle, for its own rotation (Ship::_twWheel)
        "osa volantu": [np.array(a["wheelCenter"]) - np.array(a["wheelAxis"]) * 0.15, a["wheelCenter"]],
        "propeller": [a["propeller"]], "rudder": [a["rudder"]],
    })
    return out


def fmt(xs):
    return ",".join(f"{x:.4f}".rstrip("0").rstrip(".") if abs(x) > 1e-9 else "0" for x in xs)


def config(header):
    h = header["hydro"]
    cbz = h["centerOfBuoyancy"][2]
    samples = []
    for s in header["hullSamples"]:
        x, y, z = s["position"]
        # Tidewater BoatController: the effective waterplane (x * 0.79, z toward the centre of buoyancy * 0.9)
        samples += [x * SX, y, cbz + (z - cbz) * SZ, s["area"]]
    inertia = list(h["inertia"])
    inertia[2] *= ROLL_GEAR
    return f"""// Tidewater's lobster boat (dgreenheck/tidewater, MIT, (c) DRG Software Solutions LLC) as an OFP ship.
// GENERATED by scripts/default-content/generate_tidewater_boat.py -- edit the generator, not this file.
// The model is Tidewater's procedural BoatModel (code, no model files), exported once by
// export_tidewater_boat.mjs; the class TidewaterHull below is its BoatController's hydrostatics, read by
// ShipTidewater.cpp. Every vector is in Tidewater's boat frame (+Z forward, +Y up, +X port, y = 0 the
// design waterline), placed in the model at the memory point "tw_origin".
class CfgPatches
{{
	class {PREFIX}
	{{
		units[] = {{"OP_TidewaterBoat"}};
		weapons[] = {{}};
		requiredVersion = 0;
		requiredAddons[] = {{}};
	}};
}};
class CfgVehicles
{{
	class SmallShip;
	class OP_TidewaterBoat: SmallShip
	{{
		scope = 2;
		side = 3;
		vehicleClass = "Ship";
		displayName = "Lobster boat (Tidewater)";
		model = "\\{PREFIX}\\{Path(MODEL).stem}";
		picture = "ipbr";
		crew = "Civilian";
		accuracy = 0.5;
		cost = 20000;
		armor = 60;
		fuelCapacity = 300;
		maxSpeed = 37;
		hasGunner = 0;
		hasCommander = 0;
		driverIsCommander = 1;
		transportSoldier = 3;
		cargoAction[] = {{"ManActCargo"}};
		weapons[] = {{}};
		magazines[] = {{}};
		driverAction = "ManActShipDriver";
		class Reflectors {{}};
		// the nav lights (memory cerveny / zeleny / bily pozicni) are real lamps: they light the deck and the water
		// round the boat (VehicleAI.cpp); on with the boat's lights
		markerLightScale = 15;
		class TransportMagazines {{}};
		class TidewaterHull
		{{
			origin = "tw_origin";
			// waterplane patches x, y, z, area (m^2): y is the patch's mean hull depth, so the sum of
			// area * depth is the canoe body's volume ({h['canoeVolume']:.3f} m^3, {h['suggestedMass']} kg afloat)
			samples[] = {{{fmt(samples)}}};
			inertia[] = {{{fmt(inertia)}}}; // kg m^2 about x (pitch), y (yaw), z (roll)
			maxThrust = {MAX_THRUST:g}; // N, bollard pull at full rpm
			pitchSpeed = {PITCH_SPEED:g}; // m/s, the propeller's pitch speed (thrust falls to 0 there)
			reverseFactor = {REVERSE:g};
			propeller[] = {{{fmt(header['anchors']['propeller'])}}};
			rudder[] = {{{fmt([0, -0.15, header['anchors']['rudder'][2]])}}};
			stations[] = {{{fmt([v for s in STATIONS for v in s])}}}; // z, lateral area (m^2) along the keel
			lateralY = {LATERAL_Y:g};
			contacts[] = {{{fmt([v for c in CONTACTS for v in c])}}}; // grounding points
			// no sea drawn in the cockpit and wheelhouse: half beam, transom z, house front z, sheer at both
			waterMask[] = {{{fmt(WATER_MASK)}}};
		}};
	}};
}};
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesh", type=Path, required=True, help="tidewater_boat_mesh.bin.gz")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    header, meshes = load(args.mesh)
    data, palette, counts, boxes, mass, centre = build(header, meshes)
    (args.out / MODEL).write_bytes(data)
    palette.image().save(args.out / f"{PALETTE}.png")
    Image.new("RGBA", (8, 8), tuple(int(x) for x in np.clip(srgb(hexlin(0xa9bec4)) * 255, 0, 255)) + (70,)).save(
        args.out / f"{PALETTE}_glass.png")
    hull_image().save(args.out / f"{HULL}.png")
    display_atlas().save(args.out / f"{DISPLAYS}.png")
    props_atlas().save(args.out / f"{PROPS}.png")
    for (rgb8, pattern), name in trap_textures.items():
        trap_tile(rgb8, pattern).save(args.out / f"{name}.png")
    for (kind, rgb8), name in relief_textures.items():
        lin = hexlin((rgb8[0] << 16) | (rgb8[1] << 8) | rgb8[2])
        relief_image(kind, lin).save(args.out / f"{name}.png")
    (args.out / "textures.json").write_text(json.dumps(
        {"DXT1": [PALETTE, HULL, DISPLAYS, PROPS] + sorted(relief_textures.values()), "DXT5": [f"{PALETTE}_glass"] + sorted(trap_textures.values())}, indent=1))
    (args.out / "config.cpp").write_text(config(header), newline="\n")
    print(f"{MODEL}: visual LOD faces {counts}, {len(boxes)} collision boxes, {len(palette.cols)} colours, "
          f"mass {mass.sum():.0f} kg, Tidewater origin at model {np.round(-centre, 3)}")


if __name__ == "__main__":
    main()
