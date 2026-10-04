"""Textured perspective preview of ugcave.p3d (visual LOD), software z-buffer (Claude, 2 Oct 2026)."""
import numpy as np, struct, sys
from PIL import Image
import mlod, make_tex

def load_dds(path):
    d = open(path, 'rb').read(); h, w = struct.unpack_from('<II', d, 12)
    return make_tex.undxt1(d[128:128 + w * h // 2], w, h)

TEX = {}
def tex(name):
    if name not in TEX:
        if name.startswith('ugcave'): TEX[name] = load_dds('addon/' + name.split('\\')[1])
        else: TEX[name] = np.full((64, 64, 3), (96, 118, 64), float)   # grass stand-in
    return TEX[name]

def look(eye, at):
    f = np.asarray(at, float) - eye; f /= np.linalg.norm(f); r = np.cross(f, [0, 1.0, 0]); r /= np.linalg.norm(r); u = np.cross(r, f)
    return np.array([r, u, -f])

def render(m, eye, at, W=640, H=400, fov=75, hide_roof=False):
    L = m['lods'][0]; P = np.array([p[:3] for p in L['pts']]); eye = np.asarray(eye, float); Rm = look(eye, at)
    fpx = (W / 2) / np.tan(np.radians(fov) / 2)
    img = np.zeros((H, W, 3)); img[:] = (150, 175, 205); zb = np.full((H, W), np.inf)
    sun = np.array([0.4, 0.8, 0.3]); sun /= np.linalg.norm(sun)
    for t, nv, vs, fl in L['faces']:
        ids = [v[0] for v in vs[:nv]]; uv = np.array([(v[2], v[3]) for v in vs[:nv]])
        Q = P[ids]
        if hide_roof and Q[:, 1].min() > -0.01: continue
        n = np.cross(Q[1] - Q[0], Q[2] - Q[0]); n /= np.linalg.norm(n) + 1e-9
        shade = 0.55 + 0.45 * abs(n @ sun)
        C = (Q - eye) @ Rm.T                       # camera space, looking down -z
        T = tex(t); th, tw = T.shape[:2]
        # clip the polygon against the near plane z = -NEAR (camera looks down -z), keeping uv
        NEAR = 0.05; poly = [(C[i], uv[i]) for i in range(nv)]; clipped = []
        for i in range(len(poly)):
            (pa, ua), (pb, ub) = poly[i], poly[(i + 1) % len(poly)]
            ina, inb = pa[2] <= -NEAR, pb[2] <= -NEAR
            if ina: clipped.append((pa, ua))
            if ina != inb:
                t_ = (-NEAR - pa[2]) / (pb[2] - pa[2]); clipped.append((pa + t_ * (pb - pa), ua + t_ * (ub - ua)))
        if len(clipped) < 3: continue
        CC = np.array([p for p, _ in clipped]); UU = np.array([q for _, q in clipped])
        for k_ in range(1, len(clipped) - 1):
            tri = [0, k_, k_ + 1]; c = CC[tri]
            sx = W / 2 + fpx * c[:, 0] / -c[:, 2]; sy = H / 2 - fpx * c[:, 1] / -c[:, 2]; iz = 1 / -c[:, 2]
            x0, x1 = int(max(0, np.floor(sx.min()))), int(min(W - 1, np.ceil(sx.max())))
            y0, y1 = int(max(0, np.floor(sy.min()))), int(min(H - 1, np.ceil(sy.max())))
            if x0 > x1 or y0 > y1: continue
            yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
            d = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
            if abs(d) < 1e-9: continue
            b1 = ((xx - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (yy - sy[0])) / d
            b2 = ((sx[1] - sx[0]) * (yy - sy[0]) - (xx - sx[0]) * (sy[1] - sy[0])) / d
            b0 = 1 - b1 - b2; inside = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
            if not inside.any(): continue
            z = b0 * iz[0] + b1 * iz[1] + b2 * iz[2]
            UV = UU[tri]
            u = (b0 * UV[0, 0] * iz[0] + b1 * UV[1, 0] * iz[1] + b2 * UV[2, 0] * iz[2]) / z
            v = (b0 * UV[0, 1] * iz[0] + b1 * UV[1, 1] * iz[1] + b2 * UV[2, 1] * iz[2]) / z
            depth = 1 / z; sub = zb[y0:y1 + 1, x0:x1 + 1]; ok = inside & (depth < sub)
            if not ok.any(): continue
            tx = ((u % 1) * tw).astype(int) % tw; ty = ((v % 1) * th).astype(int) % th
            col = T[ty, tx] * shade
            sub[ok] = depth[ok]; img[y0:y1 + 1, x0:x1 + 1][ok] = col[ok]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))

if __name__ == '__main__':
    m = mlod.read('addon/ugcave.p3d')
    views = [((-10, -3.3, -6.5), (8, -4, 8), False, 'chamber from the south-west corner'),
             ((9, -3.4, 8), (-6, -4.2, -5), False, 'chamber from the north-east'),
             ((-8, -3.4, -9), (-8, -3.6, -17), False, 'tunnel, south leg'),
             ((-7.5, -3.4, -16.8), (14, -1.5, -16.8), False, 'tunnel, east leg to the exit ramp'),
             ((0, 1.7, 26), (0, -3, 10), False, 'north stairwell from outside'),
             ((-30, 25, 30), (0, -3, -2), True, 'cutaway from above (roof hidden)')]
    ims = [render(m, e, a, hide_roof=h) for e, a, h, _ in views]
    S = Image.new('RGB', (640 * 2, 400 * 3))
    for i, im in enumerate(ims): S.paste(im, (640 * (i % 2), 400 * (i // 2)))
    S.save(sys.argv[1] if len(sys.argv) > 1 else 'cave_textured.png')
