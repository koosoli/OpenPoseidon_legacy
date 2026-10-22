"""Place an OFP island into whole-world mode: its heights and its ground textures, at a spot in the open ocean.

    python scripts/Prepare-EarthIsland.py --wrp "<game>/Worlds/intro.wrp" --name desert --lat 45.40 --lon -7.80

Reads the island from YOUR installed game (the .wrp and DTA/LandText.pbo) with PoseidonTools and writes into
%LOCALAPPDATA%/OpenPoseidon/world-stream/islands/<name>/ (or --cache):
  * island.json   centre, grid, cell size, the image's geographic bounds;
  * heights.f32   the island's heights (little-endian float32, row 0 = south, metres above sea level);
  * surface.jpg   the cells' ground textures, tiled per cell over the land plus a margin, shallow water tinted.
The engine (EarthIslands.cpp) blends the heights into the streamed elevation (the island's own sea floor fades
into the real one over its outer cells) and lays the image over the streamed imagery. Nothing of the game's
data enters the repository; the output stays in the local cache, like the other prepared caches.

The default place for Desert Island is the open Bay of Biscay, ~200 km off Galicia and ~500 km off the French
coast over the 4,000+ m abyssal plain: open Atlantic water, the setting of OFP's Malden archipelago.
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

LANDDATA_SCALE = 0.03 * 1.5  # Landscape.hpp: a stored height unit in metres


def tools_exe(arg):
    if arg:
        return arg
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for p in (os.path.join(root, 'dist', 'x64-win-rwdi', 'PoseidonTools.exe'),
              os.path.join(root, 'build', 'win-x64-clang-rwdi', 'apps', 'tools', 'Tools', 'PoseidonTools.exe')):
        if os.path.exists(p):
            return p
    sys.exit('PoseidonTools.exe not found; build the PoseidonTools target or pass --tools')


def read_4wvr(path):
    data = open(path, 'rb').read()
    if data[:4] != b'4WVR':
        sys.exit(f'{path}: not a 4WVR world')
    w, h = struct.unpack_from('<ii', data, 4)
    off = 12
    heights = np.frombuffer(data, '<i2', w * h, off).reshape(h, w).astype(np.float32) * LANDDATA_SCALE
    off += w * h * 2
    tex = np.frombuffer(data, '<i2', w * h, off).reshape(h, w).astype(np.int32)
    off += w * h * 2
    names = [data[off + i * 32: off + (i + 1) * 32].split(b'\0')[0].decode('latin-1') for i in range(512)]
    return heights, tex, names


def open_sea(heights, cell):
    """The island in open water. OFP maps carry shelves and stray sandbanks out to their edges (Desert Island
    has a shallow shelf along its whole north edge), which in the open ocean read as a straight line of
    surf and sand far from the island. Keep the land within 15 cells of the largest island's bounding box
    and the sea floor within 300 m of it as authored; beyond that the floor falls away to 60 m."""
    land = heights > 0.3
    n = heights.shape[0]
    # Largest connected landmass (4-neighbour flood fill), then everything in its padded bounding box.
    seen = np.zeros_like(land)
    best = None
    for z0, x0 in np.argwhere(land):
        if seen[z0, x0]:
            continue
        stack, cells = [(z0, x0)], []
        seen[z0, x0] = True
        while stack:
            z, x = stack.pop()
            cells.append((z, x))
            for dz, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                a, b = z + dz, x + dx
                if 0 <= a < n and 0 <= b < n and land[a, b] and not seen[a, b]:
                    seen[a, b] = True
                    stack.append((a, b))
        if best is None or len(cells) > len(best):
            best = cells
    keep = np.zeros_like(land)
    if best:
        zs, xs = zip(*best)
        z0, z1 = max(min(zs) - 15, 0), min(max(zs) + 16, n)
        x0, x1 = max(min(xs) - 15, 0), min(max(xs) + 16, n)
        keep[z0:z1, x0:x1] = land[z0:z1, x0:x1]
    # Distance (cells) from the kept land, chessboard metric by repeated dilation.
    dist = np.full(heights.shape, 1e9, np.float32)
    front = keep.copy()
    dist[front] = 0
    d = 0
    while front.any() and d < n:
        d += 1
        grown = front.copy()
        grown[1:, :] |= front[:-1, :]
        grown[:-1, :] |= front[1:, :]
        grown[:, 1:] |= front[:, :-1]
        grown[:, :-1] |= front[:, 1:]
        new = grown & (dist > d)
        dist[new] = d
        front = grown
    metres = dist * cell
    floor = -np.clip(2.0 + (metres - 300.0) * 0.04, 2.0, 60.0)
    out = np.where(metres <= 300.0, heights, np.minimum(heights, floor))
    return np.where(keep | (metres <= 300.0), out, np.minimum(out, -2.0)).astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wrp', required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--lat', type=float, required=True)
    ap.add_argument('--lon', type=float, required=True)
    ap.add_argument('--game', default=None, help='game folder (default: two levels above the .wrp)')
    ap.add_argument('--tools', default=None)
    ap.add_argument('--cache', default=os.path.join(os.environ.get('LOCALAPPDATA', '.'), 'OpenPoseidon', 'world-stream'))
    ap.add_argument('--cell', type=float, default=50.0, help='metres per grid cell (OFP: 50)')
    ap.add_argument('--max-image', type=int, default=4096)
    args = ap.parse_args()
    if not args.name.isidentifier():
        sys.exit('--name must be a simple identifier')
    if abs(args.lat) > 80:
        sys.exit('--lat out of range')
    tools = tools_exe(args.tools)
    game = args.game or os.path.dirname(os.path.dirname(os.path.abspath(args.wrp)))
    out = os.path.join(args.cache, 'islands', args.name)
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix='earth-island-')
    try:
        world = os.path.join(work, 'world.wrp')
        subprocess.run([tools, 'terrain', 'export-rvw4', args.wrp, world], check=True, capture_output=True)
        heights, tex, names = read_4wvr(world)
        n = heights.shape[0]
        print(f'{args.wrp}: {n}x{heights.shape[1]} cells, heights {heights.min():.1f}..{heights.max():.1f} m')
        heights = open_sea(heights, args.cell)

        # Ground textures: extract LandText.pbo once, convert the used PAAs to PNG, average-safe fallbacks.
        landtext = os.path.join(game, 'DTA', 'LandText.pbo')
        ext = os.path.join(work, 'landtext')
        subprocess.run([tools, 'pbo', 'extract', landtext, ext], check=True, capture_output=True)
        cache_tex = {}

        def texture(index, px):
            key = (index, px)
            if key in cache_tex:
                return cache_tex[key]
            name = names[index].replace('\\', '/').lower() if 0 <= index < len(names) else ''
            tile = None
            if name.startswith('landtext/'):
                src = os.path.join(ext, name.split('/', 1)[1])
                if not os.path.exists(src):
                    src = os.path.join(ext, name)
                if os.path.exists(src):
                    png = os.path.join(work, f'tex{index}.png')
                    r = subprocess.run([tools, 'image', 'convert', src, png], capture_output=True)
                    if r.returncode == 0 and os.path.exists(png):
                        tile = Image.open(png).convert('RGB').resize((px, px), Image.LANCZOS)
            if tile is None:  # sea / missing: a sand-grey the water tint goes over
                tile = Image.new('RGB', (px, px), (150, 140, 115))
            cache_tex[key] = tile
            return tile

        # Crop to the land plus a margin of shallow water.
        land = np.argwhere(heights > 0.3)
        if land.size == 0:
            sys.exit('no land in this world')
        z0, x0 = np.maximum(land.min(axis=0) - 10, 0)
        z1, x1 = np.minimum(land.max(axis=0) + 11, n)
        cells = int(max(z1 - z0, x1 - x0))
        z1, x1 = min(z0 + cells, n), min(x0 + cells, n)
        px = max(8, min(64, min(args.max_image, 4096) // cells))  # the engine reads <= 4096 px
        img = Image.new('RGB', ((x1 - x0) * px, (z1 - z0) * px))
        for z in range(z0, z1):
            for x in range(x0, x1):
                # Row 0 of the image is NORTH: grid row z grows northward.
                img.paste(texture(int(tex[z, x]), px), ((x - x0) * px, (z1 - 1 - z) * px))
        # Water over the sea floor, per pixel from bilinear heights (vertices sit on cell corners), so the
        # coast follows the terrain instead of the 50 m cell grid: clear shallows, deep blue past ~12 m.
        corner = heights[z0:z1 + 1, x0:x1 + 1] if z1 < n and x1 < n else np.pad(heights, 1, mode='edge')[z0:z1 + 1, x0:x1 + 1]
        field = Image.fromarray(np.ascontiguousarray(corner[::-1]).astype(np.float32), mode='F')
        w, h = img.size
        depth = np.maximum(-np.asarray(field.resize((w + px, h + px), Image.BILINEAR))[px // 2:px // 2 + h, px // 2:px // 2 + w], 0)
        t = np.clip((depth - 0.2) / 12.0, 0, 1)
        t = (t * t * (3 - 2 * t) * 0.92)[..., None]
        water = np.array([18, 58, 78], np.float32)
        rgb = np.asarray(img, np.float32) * (1 - t) + water * t
        Image.fromarray(rgb.astype(np.uint8)).save(os.path.join(out, 'surface.jpg'), quality=92)

        heights.astype('<f4').tofile(os.path.join(out, 'heights.f32'))
        metres = 6378137.0 * math.pi / 180.0
        half = n * args.cell / 2.0
        lon_scale = metres * math.cos(math.radians(args.lat))

        def geo(x_m, z_m):
            return args.lat + (z_m - half) / metres, args.lon + (x_m - half) / lon_scale

        south, west = geo(x0 * args.cell, z0 * args.cell)
        north, east = geo(x1 * args.cell, z1 * args.cell)
        meta = {
            'earth_island': 1, 'name': args.name, 'source': os.path.basename(args.wrp),
            'latitude': args.lat, 'longitude': args.lon, 'cells': n, 'cell_size': args.cell,
            'heights': 'heights.f32', 'image': 'surface.jpg',
            'image_bounds': [south, west, north, east], 'image_size': list(img.size),
        }
        with open(os.path.join(out, 'island.json'), 'w') as f:
            json.dump(meta, f, indent=2)
        print(f'wrote {out}: image {img.size[0]}x{img.size[1]} ({px} px/cell), bounds {south:.5f},{west:.5f}..{north:.5f},{east:.5f}')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
