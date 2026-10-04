"""Procedural textures for the ugcave test cave (Claude for Dec, 2 Oct 2026), written as DXT1 DDS with mipmaps
(Malprave loads DDS: ddsTextures). All tile seamlessly in u; floor and ceiling also in v.
  cave_wall.dds   layered rock, v = 0 at ground level (dark, under the roof) .. 1 at 5.2 m down (the floor)
  cave_ceil.dds   dark rough rock for ceilings and lintels
  cave_floor.dds  packed earth with gravel and small stones
  cave_rock.dds   lighter rock for pillars' and boulders' tops"""
import numpy as np, struct, os
from scipy import ndimage

R = np.random.default_rng(7)

def noise(n, cells, wrap_v=True):
    """tileable value noise: random grid of cells x cells, cubic upsampled to n x n"""
    g = R.random((cells, cells))
    z = ndimage.zoom(np.tile(g, (3, 3)), n / cells, order=3, mode='grid-wrap')
    return z[n:2 * n, n:2 * n]

def fbm(n, base=4, octaves=6, gain=0.55):
    out = np.zeros((n, n)); amp = 1.0; tot = 0
    for o in range(octaves):
        c = base * 2 ** o
        if c > n // 2: break
        out += amp * noise(n, c); tot += amp; amp *= gain
    out /= tot
    return (out - out.min()) / (out.max() - out.min() + 1e-9)

def to_rgb(v, c0, c1):
    v = v[..., None]; return np.asarray(c0) * (1 - v) + np.asarray(c1) * v

def relief(h, k=2.2):
    """fake top-left lighting from a height field (wrapping gradients)"""
    gx = np.roll(h, -1, 1) - np.roll(h, 1, 1); gy = np.roll(h, -1, 0) - np.roll(h, 1, 0)
    return np.clip(1 + k * (-gx - gy) * h.shape[0] / 64, 0.45, 1.6)

def ridged(n, base, octaves):
    out = np.zeros((n, n)); amp = 1.0; tot = 0
    for o in range(octaves):
        c = base * 2 ** o
        if c > n // 2: break
        out += amp * (1 - np.abs(2 * noise(n, c) - 1)); tot += amp; amp *= 0.5
    out /= tot; return (out - out.min()) / (out.max() - out.min() + 1e-9)

def wall(n=512):
    y = np.linspace(0, 1, n)[:, None]
    warp = fbm(n, 3, 4)
    strata = 0.5 + 0.5 * np.sin((y * 5 + 0.9 * warp) * 2 * np.pi)
    h = 0.45 * ridged(n, 4, 7) + 0.25 * fbm(n, 8, 6) + 0.3 * strata
    cracks = np.clip((ridged(n, 6, 4) - 0.88) * 9, 0, 1)
    col = to_rgb(np.clip(0.35 * fbm(n, 4, 5) + 0.65 * strata * 0.6 + 0.2, 0, 1), (70, 60, 50), (150, 134, 112))
    rgb = col * relief(h)[..., None] * (1 - 0.55 * cracks)[..., None]
    shade = 0.5 + 0.5 * np.clip(y * 1.3, 0, 1)
    return rgb * shade[..., None]

def ceil(n=512):
    h = 0.6 * ridged(n, 4, 7) + 0.4 * fbm(n, 8, 6)
    col = to_rgb(fbm(n, 4, 5), (36, 32, 28), (84, 76, 66))
    return col * relief(h, 2.6)[..., None]

def floor(n=512):
    base = fbm(n, 4, 7)
    rgb = to_rgb(base, (70, 58, 44), (128, 110, 86))
    # gravel: small stones as bright/dark blobs
    st = np.zeros((n, n)); pts = R.integers(0, n, (900, 2)); rad = R.uniform(2, 6, 900); val = R.uniform(-1, 1, 900)
    yy, xx = np.mgrid[0:n, 0:n]
    for (py, px), r, s in zip(pts, rad, val):
        y0, y1, x0, x1 = py - 8, py + 9, px - 8, px + 9
        ys, xs = np.mgrid[y0:y1, x0:x1]; d = np.hypot(ys - py, xs - px)
        m = np.clip(1 - d / r, 0, 1) ** 0.6
        st[ys % n, xs % n] += m * s
    rgb = rgb * (1 + 0.25 * np.clip(st, -1, 1))[..., None]
    rgb *= (0.85 + 0.3 * fbm(n, 32, 3))[..., None]
    rgb *= relief(fbm(n, 16, 5) * 0.6 + np.clip(st, 0, 1) * 0.4, 0.7)[..., None]
    return rgb

def rock(n=256):
    h = 0.6 * ridged(n, 4, 6) + 0.4 * fbm(n, 8, 5)
    col = to_rgb(fbm(n, 4, 5), (92, 84, 72), (166, 154, 136))
    return col * relief(h, 2.0)[..., None]

# ---- DXT1 ------------------------------------------------------------------------------------------------------
def rgb565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)

def unpack565(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(float)

def dxt1(img):
    """img (h, w, 3) float 0..255 -> DXT1 bytes (4-colour mode, endpoints along the block's principal axis)"""
    h, w, _ = img.shape
    B = img.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    mean = B.mean(1, keepdims=True); X = B - mean
    cov = np.einsum('bki,bkj->bij', X, X)
    axis = np.ones((len(B), 3)) / np.sqrt(3)
    for _ in range(8):
        axis = np.einsum('bij,bj->bi', cov, axis); axis /= np.linalg.norm(axis, axis=1, keepdims=True) + 1e-9
    t = np.einsum('bki,bi->bk', X, axis)
    lo = mean[:, 0] + axis * t.min(1)[:, None]; hi = mean[:, 0] + axis * t.max(1)[:, None]
    c0 = rgb565(hi); c1 = rgb565(lo)
    swap = c0 < c1; c0[swap], c1[swap] = c1[swap].copy(), c0[swap].copy()
    eq = c0 == c1
    c0e = np.where(eq & (c0 < 0xFFFF), c0 + 1, c0); c0 = c0e    # keep 4-colour mode (c0 > c1)
    c1 = np.where(c0 == c1, np.maximum(c1 - 1, 0), c1)
    p0, p1 = unpack565(c0), unpack565(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)             # (b, 4, 3)
    d = ((B[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)                   # (b, 16, 4)
    idx = d.argmin(-1).astype(np.uint32)
    bits = np.zeros(len(B), np.uint32)
    for k in range(16): bits |= idx[:, k] << (2 * k)
    out = np.zeros((len(B), 8), np.uint8)
    out[:, 0] = c0 & 255; out[:, 1] = c0 >> 8; out[:, 2] = c1 & 255; out[:, 3] = c1 >> 8
    for k in range(4): out[:, 4 + k] = (bits >> (8 * k)) & 255
    return out.tobytes()

def undxt1(data, w, h):
    a = np.frombuffer(data, np.uint8).reshape(-1, 8).astype(np.uint32)
    c0 = a[:, 0] | (a[:, 1] << 8); c1 = a[:, 2] | (a[:, 3] << 8)
    bits = a[:, 4] | (a[:, 5] << 8) | (a[:, 6] << 16) | (a[:, 7] << 24)
    p0, p1 = unpack565(c0), unpack565(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    idx = np.stack([(bits >> (2 * k)) & 3 for k in range(16)], 1)
    px = np.take_along_axis(pal, idx[..., None].repeat(3, -1).astype(np.int64), 1)
    return px.reshape(h // 4, w // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3)

def mips(img):
    out = [img]
    while min(out[-1].shape[:2]) > 4:
        m = out[-1]; out.append(0.25 * (m[0::2, 0::2] + m[1::2, 0::2] + m[0::2, 1::2] + m[1::2, 1::2]))
    return out

def write_dds(path, img):
    levels = mips(np.asarray(img, float)); h, w = img.shape[:2]
    body = b''.join(dxt1(m) for m in levels)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000            # caps, height, width, pixelformat, mipmapcount, linearsize
    pf = struct.pack('<II4sIIIII', 32, 0x4, b'DXT1', 0, 0, 0, 0, 0)
    hdr = struct.pack('<IIIIIII', 124, flags, h, w, w * h // 2, 0, len(levels)) + b'\0' * 44 + pf + \
          struct.pack('<IIIII', 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    open(path, 'wb').write(b'DDS ' + hdr + body)
    return levels

TEX = {'cave_wall': wall, 'cave_ceil': ceil, 'cave_floor': floor, 'cave_rock': rock}

if __name__ == '__main__':
    out = 'addon'; os.makedirs(out, exist_ok=True)
    from PIL import Image
    sw = []
    for name, fn in TEX.items():
        img = np.clip(fn(), 0, 255); lv = write_dds(f'{out}/{name}.dds', img)
        back = undxt1(dxt1(lv[0]), *img.shape[1::-1])
        print(name, img.shape, 'levels', len(lv), 'mean abs err', round(float(np.abs(back - lv[0]).mean()), 2), os.path.getsize(f'{out}/{name}.dds'))
        sw.append(Image.fromarray(back.astype(np.uint8)).resize((256, 256)))
    S = Image.new('RGB', (256 * len(sw), 256))
    for i, im in enumerate(sw): S.paste(im, (256 * i, 0))
    S.save('tex_swatches.png')
