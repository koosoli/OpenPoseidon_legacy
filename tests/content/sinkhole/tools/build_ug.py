# Malprave terrainHole1 test basement (Claude for Dec, 2 Oct 2026; fire geometry added for terrainHole2, job ug2): a 6 x 6 m cellar 3 m under the ground with a
# straight stair (ramp) down from the north, a roof at ground level, and the memory LOD selections that cut the terrain.
# Model space: x east, y up, z north (azimuth 0); origin on the ground in the middle of the roof; autocenter = 0.
# Face rule (knowledge section 6): in file order the normal cross(p1-p0, p2-p0) points INTO the solid / away from the
# viewer; the engine reverses faces on load. Roadway faces therefore have their normal pointing down.
import struct, sys, os
import numpy as np
import mlod

FLOOR = -3.0      # cellar floor
CEIL = -0.3       # cellar ceiling (roof slab 0.3 m thick, top at ground level)
RIN, ROUT = 3.0, 3.3      # room half size inside / outside the walls
SW, SWO = 0.8, 1.1        # stair half width inside / outside its side walls
Z_LAND = 3.5              # ramp foot (the landing in front of the doorway is floor)
Z_TOP = 8.5               # ramp top, at ground level
Z_END = 9.5               # small landing outside the hole at ground level
LINTEL = -0.9             # top of the doorway

def cross(p): p = [np.array(q, float) for q in p]; return np.cross(p[1] - p[0], p[2] - p[0])

def face_into(pts, into):
    """order a planar polygon so cross(p1-p0,p2-p0) points along 'into' (a direction)"""
    return list(pts) if np.dot(cross(pts), into) > 0 else list(pts)[::-1]

class Lod:
    def __init__(self, res): self.res = res; self.pts = []; self.idx = {}; self.faces = []; self.sels = {}; self.props = []; self.mass = None
    def P(self, q):
        k = tuple(np.round(q, 4))
        if k not in self.idx: self.idx[k] = len(self.pts); self.pts.append(tuple(float(v) for v in q))
        return self.idx[k]
    def face(self, pts, sel=None):
        ids = [self.P(q) for q in pts]; self.faces.append(ids)
        if sel:
            s = self.sels.setdefault(sel, (set(), set())); s[0].update(ids); s[1].add(len(self.faces) - 1)
        return ids
    def point(self, q, sel):
        i = self.P(q); s = self.sels.setdefault(sel, (set(), set())); s[0].add(i)
    def build(self):
        nrm = []; F = []
        for f in self.faces:
            n = cross([self.pts[i] for i in f]); l = np.linalg.norm(n); n = -n / l if l > 1e-9 else np.array([0, 1.0, 0])
            vs = []
            for i in f:
                nrm.append(tuple(float(v) for v in n)); q = self.pts[i]
                ax = int(np.argmax(np.abs(n))); u, v = ((q[2], -q[1]) if ax == 0 else (q[0], q[2]) if ax == 1 else (q[0], -q[1]))
                vs.append((i, len(nrm) - 1, float(u) / 2, float(v) / 2))
            while len(vs) < 4: vs.append((0, 0, 0.0, 0.0))
            F.append(('', len(f), vs, 0))
        tags = []
        for name, (ps, fs) in self.sels.items():
            b = bytearray(len(self.pts) + len(F))
            for i in ps: b[i] = 1
            for i in fs: b[len(self.pts) + i] = 1
            tags.append((name, bytes(b)))
        if self.mass is not None: tags.append(('#Mass#', struct.pack('<%df' % len(self.pts), *([self.mass] * len(self.pts)))))
        for pn, pv in self.props: tags.append(('#Property#', pn.encode().ljust(64, b'\0') + pv.encode().ljust(64, b'\0')))
        tags.append(('#EndOfFile#', b''))
        return dict(pts=[(x, y, z, 0) for x, y, z in self.pts], nrm=nrm, faces=F, tags=tags, res=self.res, hs=28, ver=0x100)

def quad(x0, x1, y0, y1, z0, z1, axis):
    """axis-aligned rectangle: axis 'y' (horizontal at y0), 'x' (at x0), 'z' (at z0)"""
    if axis == 'y': return [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]
    if axis == 'x': return [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)]
    return [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)]

def box(lod, x0, x1, y0, y1, z0, z1, sel):
    c = np.array([(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2])
    for q in (quad(x0, x1, y0, y0, z0, z1, 'y'), quad(x0, x1, y1, y1, z0, z1, 'y'), quad(x0, x0, y0, y1, z0, z1, 'x'),
              quad(x1, x1, y0, y1, z0, z1, 'x'), quad(x0, x1, y0, y1, z0, z0, 'z'), quad(x0, x1, y0, y1, z1, z1, 'z')):
        fc = np.mean(np.array(q), 0); lod.face(face_into(q, c - fc), sel)

def two_sided(lod, q):
    lod.face(q); lod.face(q[::-1])

def ramp(x0, x1, z0, y0, z1, y1):
    return [(x0, y0, z0), (x1, y0, z0), (x1, y1, z1), (x0, y1, z1)]

def build():
    lods = []
    # ---- visual (two-sided, so the winding can't hide anything) ----
    v = Lod(1.0)
    two_sided(v, quad(-RIN, RIN, FLOOR, FLOOR, -RIN, RIN, 'y'))                 # floor
    two_sided(v, quad(-SW, SW, FLOOR, FLOOR, RIN, Z_LAND, 'y'))                 # landing in the doorway
    two_sided(v, quad(-RIN, RIN, CEIL, CEIL, -RIN, RIN, 'y'))                   # ceiling
    two_sided(v, quad(-ROUT, ROUT, 0.02, 0.02, -ROUT, ROUT, 'y'))               # roof top at ground level
    two_sided(v, quad(-RIN, RIN, FLOOR, CEIL, -RIN, -RIN, 'z'))                 # south wall
    two_sided(v, quad(-RIN, -RIN, FLOOR, CEIL, -RIN, RIN, 'x'))                 # west wall
    two_sided(v, quad(RIN, RIN, FLOOR, CEIL, -RIN, RIN, 'x'))                   # east wall
    two_sided(v, quad(-RIN, -SW, FLOOR, CEIL, RIN, RIN, 'z'))                   # north wall, west of the door
    two_sided(v, quad(SW, RIN, FLOOR, CEIL, RIN, RIN, 'z'))                     # north wall, east of the door
    two_sided(v, quad(-SW, SW, LINTEL, CEIL, RIN, RIN, 'z'))                    # lintel
    two_sided(v, ramp(-SW, SW, Z_LAND, FLOOR, Z_TOP, 0.0))                      # the stair
    two_sided(v, quad(-SW, -SW, FLOOR, 0.0, RIN, Z_TOP, 'x'))                   # stair walls
    two_sided(v, quad(SW, SW, FLOOR, 0.0, RIN, Z_TOP, 'x'))
    two_sided(v, quad(-SWO, -SW, 0.02, 0.02, ROUT, Z_TOP, 'y'))                 # stair wall tops
    two_sided(v, quad(SW, SWO, 0.02, 0.02, ROUT, Z_TOP, 'y'))
    v.props.append(('lodnoshadow', '1'))
    lods.append(v.build())
    # ---- geometry: convex boxes (walls, roof, lintel, stair walls); no floor box, the roadway carries men ----
    g = Lod(1e13); g.mass = 2000.0; n = 0
    def gb(*a):
        nonlocal n; n += 1; box(g, *a, 'component%02d' % n)
    gb(-ROUT, ROUT, CEIL, 0.0, -ROUT, ROUT)              # roof slab
    gb(-ROUT, ROUT, FLOOR, CEIL, -ROUT, -RIN)            # south wall
    gb(-ROUT, -RIN, FLOOR, CEIL, -RIN, RIN)              # west wall
    gb(RIN, ROUT, FLOOR, CEIL, -RIN, RIN)                # east wall
    gb(-ROUT, -SW, FLOOR, CEIL, RIN, ROUT)               # north wall west
    gb(SW, ROUT, FLOOR, CEIL, RIN, ROUT)                 # north wall east
    gb(-SW, SW, LINTEL, CEIL, RIN, ROUT)                 # lintel
    gb(-SWO, -SW, FLOOR, 0.0, ROUT, Z_TOP)               # stair wall west
    gb(SW, SWO, FLOOR, 0.0, ROUT, Z_TOP)                 # stair wall east
    g.props += [('autocenter', '0'), ('class', 'house')]
    lods.append(g.build())
    # ---- fire geometry (ug2): the same boxes + the floor slab and the stair as solids, so bullets and grenades stop
    # on them (the roadway is not hit by shots). Men collide with the geometry LOD above, which has no floor.
    f = Lod(7e15); n2 = 0
    def fb(*a):
        nonlocal n2; n2 += 1; box(f, *a, 'component%02d' % n2)
    for a in ((-ROUT, ROUT, CEIL, 0.0, -ROUT, ROUT), (-ROUT, ROUT, FLOOR, CEIL, -ROUT, -RIN), (-ROUT, -RIN, FLOOR, CEIL, -RIN, RIN),
              (RIN, ROUT, FLOOR, CEIL, -RIN, RIN), (-ROUT, -SW, FLOOR, CEIL, RIN, ROUT), (SW, ROUT, FLOOR, CEIL, RIN, ROUT),
              (-SW, SW, LINTEL, CEIL, RIN, ROUT), (-SWO, -SW, FLOOR - 0.3, 0.0, ROUT, Z_TOP), (SW, SWO, FLOOR - 0.3, 0.0, ROUT, Z_TOP),
              (-ROUT, ROUT, FLOOR - 0.3, FLOOR, -ROUT, ROUT), (-SW, SW, FLOOR - 0.3, FLOOR, RIN, Z_LAND)):
        fb(*a)
    # the stair: a convex wedge under the ramp
    n2 += 1; sel = 'component%02d' % n2
    w = [(-SW, FLOOR, Z_LAND), (SW, FLOOR, Z_LAND), (SW, 0.0, Z_TOP), (-SW, 0.0, Z_TOP),
         (-SW, FLOOR - 0.3, Z_LAND), (SW, FLOOR - 0.3, Z_LAND), (SW, FLOOR - 0.3, Z_TOP), (-SW, FLOOR - 0.3, Z_TOP)]
    c = np.mean(np.array(w), 0)
    for q in ([w[0], w[1], w[2], w[3]], [w[4], w[5], w[6], w[7]], [w[0], w[1], w[5], w[4]], [w[3], w[2], w[6], w[7]],
              [w[0], w[3], w[7], w[4]], [w[1], w[2], w[6], w[5]]):
        fc = np.mean(np.array(q), 0); f.face(face_into(q, c - fc), sel)
    f.mass = 2000.0
    fire_lod = f.build()  # appended last: LODs stay in rising resolution order
    # ---- memory: the terrain hole footprints (room incl. walls; stairwell incl. its walls) ----
    m = Lod(1e15)
    for x, z in ((-ROUT, -ROUT), (ROUT, -ROUT), (ROUT, ROUT), (-ROUT, ROUT)): m.point((x, 0.0, z), 'terrain_hole1')
    for x, z in ((-SWO, ROUT), (SWO, ROUT), (SWO, Z_TOP), (-SWO, Z_TOP)): m.point((x, 0.0, z), 'terrain_hole2')
    lods.append(m.build())
    # ---- roadway: floor, doorway landing, stair, roof, top landing (normal down) ----
    r = Lod(3e15); down = np.array([0, -1.0, 0])
    for q in (quad(-RIN, RIN, FLOOR, FLOOR, -RIN, RIN, 'y'), quad(-SW, SW, FLOOR, FLOOR, RIN, Z_LAND, 'y'),
              ramp(-SW, SW, Z_LAND, FLOOR, Z_TOP, 0.0), quad(-SW, SW, 0.0, 0.0, Z_TOP, Z_END, 'y'),
              quad(-ROUT, ROUT, 0.0, 0.0, -ROUT, ROUT, 'y')):
        r.face(face_into(q, down))
    lods.append(r.build())
    # ---- paths: In1 at the top landing, down the stair, positions in the cellar ----
    p = Lod(4e15)
    In1 = (0.0, 0.0, 9.2); S1 = (0.0, 0.0, 8.3); S1b = (0.4, 0.0, 8.8)
    S2 = (0.0, FLOOR, 3.7); S2b = (0.4, FLOOR, 3.2)
    P1 = (0.0, FLOOR, 0.0); P2 = (-2.0, FLOOR, -2.0); P3 = (2.0, FLOOR, -2.0)
    for tri in ((In1, S1, S1b), (S1, S2, S2b), (S2, P1, S2b), (P1, P2, P3)): p.face(face_into(tri, down))
    p.point(In1, 'in1'); p.point(P1, 'pos1'); p.point(P2, 'pos2'); p.point(P3, 'pos3')
    lods.append(p.build())
    lods.append(fire_lod)
    return dict(ver=0x101, lods=lods, tail=b'')

def pbo(path, prefix, files):
    def hdr(name, method, orig, ts, size): return name.encode() + b'\0' + struct.pack('<5I', method, orig, 0, ts, size)
    out = hdr('', 0x56657273, 0, 0, 0) + b'prefix\0' + prefix.encode() + b'\0' + b'\0'
    for name, data in files: out += hdr(name, 0, 0, 0, len(data))
    out += hdr('', 0, 0, 0, 0)
    for name, data in files: out += data
    open(path, 'wb').write(out)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'addon'
    os.makedirs(out, exist_ok=True)
    m = build(); mlod.write(os.path.join(out, 'ugbasement.p3d'), m)
    for L in m['lods']: print('res %-7g pts %4d faces %4d tags %s' % (L['res'], len(L['pts']), len(L['faces']), [t[0] for t in L['tags']]))
