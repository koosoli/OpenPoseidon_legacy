"""Build the whole world's moon surface map (assets/earth/moon/moon_albedo.pmoon) from NASA LRO imagery.

Source: the LRO Wide Angle Camera global mosaic, 303 px/degree (LRO_WAC_Mosaic_Global_303ppd_v02), served
by NASA's Moon Trek as WMTS tiles. NASA/GSFC/Arizona State University. NASA imagery is public domain;
credit "NASA/GSFC/Arizona State University" (recorded in THIRD_PARTY_NOTICES.md).

Level 3 of the tile pyramid is 16 x 8 tiles of 256 px: a 4096 x 2048 equirectangular map, longitude -180
at u = 0, latitude +90 at row 0, selenographic 0/0 (the near side's centre) in the middle. ~2.7 km/texel
at the equator; the sky shader's procedural craters carry the detail below that.

The renderer has no image decoder (see sky/milkyway.rs), so this writes the full mip chain as raw R8:

    0   u8[4]   magic "PMON"
    4   u32 LE  version (1)
    8   u32 LE  width
    12  u32 LE  height
    16  u32 LE  mip levels
    20  u32 LE  mean (0..255) of level 0, which the shader divides by: the map gives RELATIVE albedo
    24  u8[]    level 0, level 1, ... each width>>i x height>>i, row 0 at latitude +90

Usage: python tools/sky/make_moon_albedo.py <tile dir or "download"> <out .pmoon>
"""

import os
import struct
import sys
import urllib.request

import numpy as np
from PIL import Image

URL = ("https://trek.nasa.gov/tiles/Moon/EQ/LRO_WAC_Mosaic_Global_303ppd_v02/1.0.0/default/default028mm/"
       "3/{row}/{col}.jpg")
COLS, ROWS, TILE = 16, 8, 256


def tiles(source):
    for row in range(ROWS):
        for col in range(COLS):
            if source == "download":
                with urllib.request.urlopen(URL.format(row=row, col=col), timeout=60) as r:
                    data = r.read()
                from io import BytesIO
                img = Image.open(BytesIO(data))
            else:
                img = Image.open(os.path.join(source, f"{row}_{col}.jpg"))
            yield row, col, np.asarray(img.convert("L"), dtype=np.uint8)


def main():
    source, out = sys.argv[1], sys.argv[2]
    full = np.zeros((ROWS * TILE, COLS * TILE), dtype=np.uint8)
    for row, col, t in tiles(source):
        full[row * TILE:(row + 1) * TILE, col * TILE:(col + 1) * TILE] = t
    # The mosaic has no data in the last rows at each pole (black): take the nearest row that has.
    means = full.mean(axis=1)
    good = np.where(means > 0.4 * np.median(means))[0]
    full[:good[0]] = full[good[0]]
    full[good[-1] + 1:] = full[good[-1]]
    levels = [full]
    while min(levels[-1].shape) > 1:
        a = levels[-1].astype(np.float32)
        h, w = a.shape
        levels.append(((a[0:h:2, 0:w:2] + a[1:h:2, 0:w:2] + a[0:h:2, 1:w:2] + a[1:h:2, 1:w:2]) * 0.25 + 0.5)
                      .astype(np.uint8))
    mean = int(round(float(full.mean())))
    with open(out, "wb") as f:
        f.write(b"PMON" + struct.pack("<5I", 1, full.shape[1], full.shape[0], len(levels), mean))
        for level in levels:
            f.write(level.tobytes())
    print(f"{out}: {full.shape[1]}x{full.shape[0]}, {len(levels)} levels, mean {mean}")


if __name__ == "__main__":
    main()
