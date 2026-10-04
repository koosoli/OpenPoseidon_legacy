# Malprave test cave (Claude for Dec, 2 Oct 2026): a large underground chamber with two ways in, for the terrain hole
# patches (terrainHole1-7). One model, everything within 24 m of its origin (the engine finds hole objects within
# 25 m of their origin), 4 convex terrain-hole areas.
#   chamber   24 x 18 m inside, floor 5 m under the ground, ceiling 0.4 m under it (4.6 m high); 3 rock pillars,
#             3 boulders (waist-high cover), a 1 m ledge in the north-west corner with a ramp
#   north     open stairwell (ramp, 30 deg) from the chamber's north doorway up to the ground
#   tunnel    2.4 m wide, 2.5 m high: south from the chamber's south-west doorway (10 m), east along the south side
#             (15 m), then an open ramp up to the ground in the east (8.5 m)
# Model space: x east, y up, z north (azimuth 0); origin on the ground at the chamber centre; autocenter = 0.
# Face rule: in file order the normal cross(p1-p0, p2-p0) points INTO the solid / away from the viewer; roadway faces
# have their normal pointing down (see build_ug.py).
import os, sys, struct
import numpy as np
import mlod
from build_ug import Lod, quad, face_into, box, cross, pbo

FL = -5.0            # floor
CE = -0.4            # chamber ceiling (roof slab 0.4 m)
TC = -2.5            # tunnel ceiling (2.5 m high)
W = 0.3              # wall thickness
CX, CZ0, CZ1 = 12.0, -8.0, 10.0        # chamber inside: x -12..12, z -8..10
DOOR_N = 1.0                           # north doorway / stairwell half width (inside)
N_FOOT, N_TOP, N_END = 11.0, 19.5, 20.5  # north ramp foot, top (ground), landing end
LX0, LX1 = -9.2, -6.8                  # south leg inside x
CZ_S0, CZ_S1 = -18.0, -15.6            # east corridor inside z
E_FOOT, E_TOP, E_END = 6.0, 14.5, 15.5   # east ramp foot, top (ground), landing end
LINTEL_N = FL + 2.1                    # top of the north doorway
LEDGE = (-12.0, -9.0, 7.0, 10.0)       # x0, x1, z0, z1 of the ledge top
LEDGE_Y = FL + 1.0
LEDGE_RAMP_Z = 4.0                     # ramp from the ledge (z = 7) down to z = 4
PILLARS = [(-4.0, 3.0), (4.0, -2.5), (10.5, 3.0)]
BOULDERS = [(-4.0, -2.5), (4.0, 3.0), (10.5, -2.5)]
GRASS = r'eden\tn.paa'

def tex_face(lod, pts, tex, scale):
    """a face with its own texture and uv scale (metres per texture repeat)"""
    ids = lod.face(pts)
    lod.tex = getattr(lod, 'tex', {}); lod.tex[len(lod.faces) - 1] = (tex, scale)
    return ids

def solids():
    """convex boxes (x0, x1, y0, y1, z0, z1): what men and vehicles collide with"""
    b = []
    # chamber roof and walls
    b.append((-CX - W, CX + W, CE, 0.0, CZ0 - W, CZ1 + W))
    b.append((-CX - W, -CX, FL, CE, CZ0 - W, CZ1 + W))
    b.append((CX, CX + W, FL, CE, CZ0 - W, CZ1 + W))
    b.append((-CX - W, -DOOR_N, FL, CE, CZ1, CZ1 + W))
    b.append((DOOR_N, CX + W, FL, CE, CZ1, CZ1 + W))
    b.append((-DOOR_N, DOOR_N, LINTEL_N, CE, CZ1, CZ1 + W))
    b.append((-CX - W, LX0, FL, CE, CZ0 - W, CZ0))
    b.append((LX1, CX + W, FL, CE, CZ0 - W, CZ0))
    b.append((LX0, LX1, TC, CE, CZ0 - W, CZ0))
    # north stairwell walls (ground level to the floor)
    b.append((-DOOR_N - W, -DOOR_N, FL, 0.0, CZ1 + W, N_TOP))
    b.append((DOOR_N, DOOR_N + W, FL, 0.0, CZ1 + W, N_TOP))
    # south leg: walls + roof
    b.append((LX0 - W, LX0, FL, 0.0, CZ_S0 - W, CZ0 - W))
    b.append((LX1, LX1 + W, FL, 0.0, CZ_S1 + W, CZ0 - W))
    b.append((LX0, LX1, TC, 0.0, CZ_S1 + W, CZ0 - W))
    # east corridor: walls + roof (to the ramp foot + the lintel thickness)
    b.append((LX0 - W, E_TOP, FL, 0.0, CZ_S0 - W, CZ_S0))
    b.append((LX1, E_TOP, FL, 0.0, CZ_S1, CZ_S1 + W))
    b.append((LX0, E_FOOT + W, TC, 0.0, CZ_S0, CZ_S1 + W))
    # pillars (floor to ceiling) and boulders (cover)
    for x, z in PILLARS: b.append((x - 0.7, x + 0.7, FL, CE, z - 0.7, z + 0.7))
    for x, z in BOULDERS: b.append((x - 0.6, x + 0.6, FL, FL + 0.6, z - 0.6, z + 0.6))
    return b

def floors():
    """walkable surfaces as polygons (x, y, z), any orientation"""
    f = []
    f.append(quad(-CX, CX, FL, FL, CZ0, CZ1, 'y'))                        # chamber floor
    f.append(quad(-DOOR_N, DOOR_N, FL, FL, CZ1, N_FOOT, 'y'))             # north doorway landing
    f.append([(-DOOR_N, FL, N_FOOT), (DOOR_N, FL, N_FOOT), (DOOR_N, 0.0, N_TOP), (-DOOR_N, 0.0, N_TOP)])   # north ramp
    f.append(quad(-DOOR_N - W, DOOR_N + W, 0.0, 0.0, N_TOP, N_END, 'y'))  # north top landing
    f.append(quad(LX0, LX1, FL, FL, CZ_S1, CZ0, 'y'))                     # south leg floor
    f.append(quad(LX0, E_FOOT, FL, FL, CZ_S0, CZ_S1, 'y'))                # corridor floor
    f.append([(E_FOOT, FL, CZ_S0), (E_TOP, 0.0, CZ_S0), (E_TOP, 0.0, CZ_S1), (E_FOOT, FL, CZ_S1)])          # east ramp
    f.append(quad(E_TOP, E_END, 0.0, 0.0, CZ_S0 - W, CZ_S1 + W, 'y'))      # east top landing
    x0, x1, z0, z1 = LEDGE
    f.append(quad(x0, x1, LEDGE_Y, LEDGE_Y, z0, z1, 'y'))                 # ledge top
    f.append([(x0, FL, LEDGE_RAMP_Z), (x1, FL, LEDGE_RAMP_Z), (x1, LEDGE_Y, z0), (x0, LEDGE_Y, z0)])      # ledge ramp
    return f

def roofs():
    """roof tops at ground level (y = 0): the ground over the cave"""
    return [quad(-CX - W, CX + W, 0.0, 0.0, CZ0 - W, CZ1 + W, 'y'),
            quad(LX0 - W, LX1 + W, 0.0, 0.0, CZ_S1 + W, CZ0 - W, 'y'),
            quad(LX0 - W, E_FOOT + W, 0.0, 0.0, CZ_S0 - W, CZ_S1 + W, 'y'),
            quad(E_FOOT + W, E_TOP, 0.0, 0.0, CZ_S0 - W, CZ_S0, 'y'),       # wall tops along the ramp
            quad(E_FOOT + W, E_TOP, 0.0, 0.0, CZ_S1, CZ_S1 + W, 'y'),
            quad(-DOOR_N - W, -DOOR_N, 0.0, 0.0, CZ1 + W, N_TOP, 'y'),
            quad(DOOR_N, DOOR_N + W, 0.0, 0.0, CZ1 + W, N_TOP, 'y')]

HOLES = [  # convex footprints (x, z): chamber, north stairwell, south leg, corridor + east ramp
    [(-CX - W, CZ0 - W), (CX + W, CZ0 - W), (CX + W, CZ1 + W), (-CX - W, CZ1 + W)],
    [(-DOOR_N - W, CZ1 + W), (DOOR_N + W, CZ1 + W), (DOOR_N + W, N_TOP), (-DOOR_N - W, N_TOP)],
    [(LX0 - W, CZ_S1 + W), (LX1 + W, CZ_S1 + W), (LX1 + W, CZ0 - W), (LX0 - W, CZ0 - W)],
    [(LX0 - W, CZ_S0 - W), (E_TOP, CZ_S0 - W), (E_TOP, CZ_S1 + W), (LX0 - W, CZ_S1 + W)],
]

def box_faces(x0, x1, y0, y1, z0, z1, skip_top=False):
    c = np.array([(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2]); out = []
    for k, q in enumerate((quad(x0, x1, y0, y0, z0, z1, 'y'), quad(x0, x1, y1, y1, z0, z1, 'y'), quad(x0, x0, y0, y1, z0, z1, 'x'),
                           quad(x1, x1, y0, y1, z0, z1, 'x'), quad(x0, x1, y0, y1, z0, z0, 'z'), quad(x0, x1, y0, y1, z1, z1, 'z'))):
        if k == 1 and skip_top and abs(y1) < 1e-6: continue
        fc = np.mean(np.array(q), 0); out.append(face_into(q, c - fc))
    return out

def paths():
    """house path network: nodes and links; in1 = north top, in2 = east top"""
    N = {}
    N['in1'] = (0.0, 0.0, 20.0); N['s1'] = (0.0, 0.0, 19.0); N['s2'] = (0.0, FL, 10.6)
    grid = {}
    for i, x in enumerate((-8.0, 0.0, 8.0)):
        for j, z in enumerate((6.0, 0.0, -5.0)):
            grid[(i, j)] = 'g%d%d' % (i, j); N[grid[(i, j)]] = (x, FL, z)
    N['lr'] = (-10.5, FL, 3.3); N['ld'] = (-10.5, LEDGE_Y, 8.5)
    N['t0'] = (-8.0, FL, -7.6); N['t1'] = (-8.0, FL, -12.0); N['t2'] = (-8.0, FL, -16.8)
    N['t3'] = (0.0, FL, -16.8); N['t4'] = (5.7, FL, -16.8); N['t5'] = (14.2, 0.0, -16.8); N['in2'] = (15.0, 0.0, -16.8)
    L = [('in1', 's1'), ('s1', 's2'), ('s2', grid[(1, 0)])]
    for i in range(3):
        for j in range(3):
            if i < 2: L.append((grid[(i, j)], grid[(i + 1, j)]))
            if j < 2: L.append((grid[(i, j)], grid[(i, j + 1)]))
    L += [('lr', grid[(0, 0)]), ('lr', grid[(0, 1)]), ('lr', 'ld'),
          (grid[(0, 2)], 't0'), ('t0', 't1'), ('t1', 't2'), ('t2', 't3'), ('t3', 't4'), ('t4', 't5'), ('t5', 'in2')]
    pos = [grid[(i, j)] for j in range(3) for i in range(3)] + ['ld', 't1', 't3']
    return N, L, pos

def build():
    lods = []
    # ---- visual (two-sided) ----
    v = Lod(1.0); v.tex = {}
    def vis(q, tex='', scale=2.0):
        for qq in (q, q[::-1]):
            v.face(qq); v.tex[len(v.faces) - 1] = (tex, scale)
    for bx in solids():
        for q in box_faces(*bx, skip_top=True): vis(q)
    for q in floors(): vis(q)
    x0, x1, z0, z1 = LEDGE        # ledge sides
    vis(quad(x1, x1, FL, LEDGE_Y, z0, z1, 'x')); vis(quad(x0, x1, FL, LEDGE_Y, z1, z1, 'z'))
    vis([(x1, FL, LEDGE_RAMP_Z), (x1, FL, z0), (x1, LEDGE_Y, z0)])
    vis([(x0, FL, LEDGE_RAMP_Z), (x0, LEDGE_Y, z0), (x0, FL, z0)])
    for q in roofs():
        q = [(a, b + 0.0, c) for a, b, c in q]; vis(q, GRASS, 12.0)
    v.props.append(('lodnoshadow', '1'))
    lods.append(finish(v))
    # ---- geometry: the convex solids (no floor: the roadway carries men) ----
    g = Lod(1e13); g.mass = 5000.0
    for k, bx in enumerate(solids()): box(g, *bx, 'component%02d' % (k + 1))
    g.props += [('autocenter', '0'), ('class', 'house')]
    lods.append(g.build())
    # ---- memory: hole footprints ----
    m = Lod(1e15)
    for k, hole in enumerate(HOLES):
        for x, z in hole: m.point((x, 0.0, z), 'terrain_hole%d' % (k + 1))
    lods.append(m.build())
    # ---- roadway ----
    r = Lod(3e15); down = np.array([0, -1.0, 0])
    for q in floors() + roofs(): r.face(face_into(q, down))
    lods.append(r.build())
    # ---- paths ----
    p = Lod(4e15); N, L, pos = paths()
    for a, b in L:
        A, B = np.array(N[a]), np.array(N[b]); d = B - A; side = np.cross(d, [0, 1.0, 0]); side = side / (np.linalg.norm(side) + 1e-9) * 0.25
        p.face(face_into([tuple(A), tuple(B), tuple(B + side)], down))
    p.point(N['in1'], 'in1'); p.point(N['in2'], 'in2')
    for k, n in enumerate(pos): p.point(N[n], 'pos%d' % (k + 1))
    lods.append(p.build())
    # ---- fire geometry: solids + floor slabs + wedges under the ramps and the ledge ----
    f = Lod(7e15); f.mass = 5000.0; k = 0
    slabs = [(-CX, CX, FL - 0.3, FL, CZ0, CZ1), (-DOOR_N, DOOR_N, FL - 0.3, FL, CZ1, N_FOOT),
             (LX0, LX1, FL - 0.3, FL, CZ_S1, CZ0), (LX0, E_FOOT, FL - 0.3, FL, CZ_S0, CZ_S1),
             (LEDGE[0], LEDGE[1], FL, LEDGE_Y, LEDGE[2], LEDGE[3])]
    for bx in solids() + slabs:
        k += 1; box(f, *bx, 'component%02d' % k)
    def wedge(pts_top, pts_bot):
        nonlocal k; k += 1; sel = 'component%02d' % k; w = list(pts_top) + list(pts_bot); c = np.mean(np.array(w), 0)
        for q in ([w[0], w[1], w[2], w[3]], [w[4], w[5], w[6], w[7]], [w[0], w[1], w[5], w[4]], [w[3], w[2], w[6], w[7]],
                  [w[0], w[3], w[7], w[4]], [w[1], w[2], w[6], w[5]]):
            fc = np.mean(np.array(q), 0); f.face(face_into(q, c - fc), sel)
    B = FL - 0.3
    wedge([(-DOOR_N, FL, N_FOOT), (DOOR_N, FL, N_FOOT), (DOOR_N, 0.0, N_TOP), (-DOOR_N, 0.0, N_TOP)],
          [(-DOOR_N, B, N_FOOT), (DOOR_N, B, N_FOOT), (DOOR_N, B, N_TOP), (-DOOR_N, B, N_TOP)])
    wedge([(E_FOOT, FL, CZ_S0), (E_FOOT, FL, CZ_S1), (E_TOP, 0.0, CZ_S1), (E_TOP, 0.0, CZ_S0)],
          [(E_FOOT, B, CZ_S0), (E_FOOT, B, CZ_S1), (E_TOP, B, CZ_S1), (E_TOP, B, CZ_S0)])
    x0, x1, z0, z1 = LEDGE
    wedge([(x0, FL, LEDGE_RAMP_Z), (x1, FL, LEDGE_RAMP_Z), (x1, LEDGE_Y, z0), (x0, LEDGE_Y, z0)],
          [(x0, B, LEDGE_RAMP_Z), (x1, B, LEDGE_RAMP_Z), (x1, B, z0), (x0, B, z0)])
    lods.append(f.build())
    return dict(ver=0x101, lods=lods, tail=b'')

T_WALL, T_CEIL, T_FLOOR, T_ROCK = r'ugcave\cave_wall.dds', r'ugcave\cave_ceil.dds', r'ugcave\cave_floor.dds', r'ugcave\cave_rock.dds'

def pick(q):
    """texture and uv mapping for a visual face from its shape and height"""
    q = np.asarray(q, float); n = np.cross(q[1] - q[0], q[2] - q[0]); n = n / (np.linalg.norm(n) + 1e-9); y = q[:, 1].mean()
    if abs(n[1]) < 0.7: return T_WALL, 'wall'
    if y > -0.01: return GRASS, 'grass'
    if abs(n[1]) < 0.99: return T_FLOOR, 'flat'           # ramps
    if y >= -3.0: return T_CEIL, 'flat'                   # ceilings, lintel and roof undersides
    if -4.5 < y < -4.3: return T_ROCK, 'flat'             # boulder tops
    return T_FLOOR, 'flat'                                # floors, ledge

def finish(lod):
    """Lod.build() with per-face textures: walls run v from ground level (0) to 5.2 m down (1), u every 4 m along
    the wall; floors / ceilings tile every 4 m; the grass on the roofs every 12 m"""
    L = lod.build(); F = []; P = np.array([p[:3] for p in L['pts']])
    for k, (t, nv, vs, fl) in enumerate(L['faces']):
        q = P[[a for a, b, u, w in vs[:nv]]]; tex, mode = pick(q); out = []
        n = np.cross(q[1] - q[0], q[2] - q[0])
        for a, b, u, w in vs:
            x, y, z = P[a]
            if mode == 'wall': u, w = ((z if abs(n[0]) > abs(n[2]) else x) / 4.0, -y / 5.2)
            elif mode == 'grass': u, w = x / 12.0, z / 12.0
            else: u, w = x / 4.0, z / 4.0
            out.append((a, b, float(u), float(w)))
        F.append((tex, nv, out, fl))
    L['faces'] = F; return L

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'addon'
    os.makedirs(out, exist_ok=True)
    m = build(); mlod.write(os.path.join(out, 'ugcave.p3d'), m)
    for L in m['lods']: print('res %-7g pts %4d faces %4d tags %s' % (L['res'], len(L['pts']), len(L['faces']), [t[0] for t in L['tags']][:12]))
