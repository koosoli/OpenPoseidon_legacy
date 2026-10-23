"""Build the whole world's moon height map (assets/earth/moon/moon_height.pmoh) from NASA LRO LOLA data.

Source: LRO LOLA gridded data record LDEM_16 (16 px/degree, 5760 x 2880 LSB int16, heights in 0.5 m units
above the 1737.4 km reference sphere), NASA PDS Geosciences Node:
  https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/lola_gdr/cylindrical/img/ldem_16.img
NASA data, public domain (recorded in THIRD_PARTY_NOTICES.md).

The source runs from longitude 0 at its left edge; it is rolled half a turn so selenographic longitude 0
(the near side's centre) is in the middle, like moon_albedo.pmoon, and resampled to 4096 x 2048.

Format (raw, no image decoder in the renderer):
    0   u8[4]   magic "PMOH"
    4   u32 LE  version (1)
    8   u32 LE  width
    12  u32 LE  height
    16  u32 LE  mip levels
    20  f32 LE  height of value 0 (metres above 1737.4 km)
    24  f32 LE  height of value 65535
    28  u16[]   level 0, level 1, ... (each width>>i x height>>i), row 0 at latitude +90

Usage: python tools/sky/make_moon_relief.py <ldem_16.img> <out .pmoh>
"""

import struct
import sys

import numpy as np

W, H = 4096, 2048


def main():
    src, out = sys.argv[1], sys.argv[2]
    dem = np.fromfile(src, dtype='<i2').reshape(2880, 5760).astype(np.float32) * 0.5
    dem = np.roll(dem, 5760 // 2, axis=1)  # longitude 0 to the middle
    # Resample to W x H by bilinear sampling at texel centres (longitude wraps, latitude clamps).
    ys = (np.arange(H) + 0.5) * dem.shape[0] / H - 0.5
    xs = (np.arange(W) + 0.5) * dem.shape[1] / W - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, dem.shape[0] - 1)
    y1 = np.clip(y0 + 1, 0, dem.shape[0] - 1)
    fy = (ys - np.floor(ys))[:, None]
    x0 = np.floor(xs).astype(int) % dem.shape[1]
    x1 = (x0 + 1) % dem.shape[1]
    fx = (xs - np.floor(xs))[None, :]
    top = dem[y0][:, x0] * (1 - fx) + dem[y0][:, x1] * fx
    bottom = dem[y1][:, x0] * (1 - fx) + dem[y1][:, x1] * fx
    heights = top * (1 - fy) + bottom * fy
    lo, hi = float(heights.min()), float(heights.max())
    levels = [heights]
    while min(levels[-1].shape) > 1:
        a = levels[-1]
        levels.append(0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]))
    with open(out, 'wb') as f:
        f.write(b'PMOH' + struct.pack('<4I2f', 1, W, H, len(levels), lo, hi))
        for level in levels:
            q = np.clip(np.round((level - lo) / (hi - lo) * 65535.0), 0, 65535).astype('<u2')
            f.write(q.tobytes())
    print(f'{out}: {W}x{H}, {len(levels)} levels, heights {lo:.0f} .. {hi:.0f} m')


if __name__ == '__main__':
    main()
