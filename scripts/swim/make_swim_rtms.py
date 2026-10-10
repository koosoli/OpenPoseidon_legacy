# Sinkhole W3: author the swim moves procedurally -- soldiers swimming in full gear (owner, 2026-10-04:
# "adjust all swim animations to look like real soldiers swimming with gear on").
# Usage: python make_swim_rtms.py <extracted Malprave swim.pbo dir> <extracted DTA\Anim.pbo dir> <out dir>
#   then copy <out dir>/*.rtm to content/default-packs/swim/rtm and run scripts/Build-Swim.ps1.
#
# Every pose is built by forward kinematics on the CWA MC rest skeleton (Malprave's fitted joints.json,
# G:\Malprave\tools\swimanim, read only): each bone turns about its own rest pivot, children inherit, so no
# bone can come out twisted about its length (the fault behind "feet backwards" in the earlier moves).
# Malprave's moves are read only for what the engine is tuned to: each move's step, phase count and mean
# head position (the swimmer's waterline is set by the head), the slung rifle's offset from the chest, and
# swimout (climbing out), which keeps Malprave's motion with the leg repair of fix_swim_rtms.py.
#
# The technique, as soldiers swim loaded (vest, pouches, rifle slung -- they ride low and keep the head up):
#   tread           upright, a slight forward lean, breaststroke / eggbeater kick, hands sculling at chest
#                   depth, head level
#   swim            heads-up combat breaststroke: body inclined ~55 deg (the gear will not let it lie flat),
#                   a short outward-and-in arm pull to the chest, recovery forward, then the frog kick
#   swimlf / swimrf the same stroke angled 25 deg toward the side
#   swimb           upright, leaning back, hands pushing water forward, an alternating back-kick
#   swiml / swimr   upright, scissor kick sideways, the leading arm sculling to the side
#   swimlb / swimrb a blend of the back and side moves
#   swimturnl/r     tread kick, the arms sculling against each other, the body yawing into the turn
#   swimstart       tread -> stroke;  swimstop  stroke (glide) -> tread
#   swimdead        killed treading: goes limp into a face-down float;  swimdeadf  the same from the stroke
#   fall            falling: upright, rifle held lowered in both hands (the stock relaxed stance's arms and
#                   grip, savarestanistat.rtm), legs in a slow loose bicycle
#   fallland        the stock jump-down landing (sdoskok.rtm) in place, then up into the stance
#   swimin          into the water from a fall: the fall pose blends into treading, the rifle to slung
#
# Rest-frame conventions (J): x forward, y up, z the body's left. About z: + swings a hanging limb forward
# (hip / shoulder flexion, elbow flexion), - bends the knee and points the foot, - leans the body forward.
# About x: + takes the right arm/leg out to the side, - the left, and + rolls the body onto its left side.
# About y: + turns the body to its RIGHT (checked: lf/rf, side looks and turns came out mirrored with + as
# left). Model space: forward is -z, left +x.
import sys, os, json, numpy as np
sys.path.insert(0, r'G:\Malprave\tools\swimanim')
from rtm import read_rtm, write_rtm, mat4
J = {k: np.array(v) for k, v in json.load(open(r'G:\Malprave\tools\swimanim\joints.json')).items()}
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fix_swim_rtms as fix  # noqa: E402  (swimout keeps Malprave's motion with the leg repair)

BASE = np.array([[0.0, 0, 1], [0, 1, 0], [-1, 0, 0]])  # J frame -> model: x(fwd) -> -z, z(left) -> +x
PARENT = {'bricho': ('pzadek', 'pzadek>bricho'), 'zebra': ('bricho', 'bricho>zebra'),
          'hrudnik': ('zebra', 'zebra>hrudnik'), 'krk': ('hrudnik', 'hrudnik>krk'), 'hlava': ('krk', 'krk>hlava')}
for s in 'pl':
    PARENT.update({s + 'rameno': ('hrudnik', 'hrudnik>' + s + 'rameno'), s + 'biceps': (s + 'rameno', s + 'rameno>' + s + 'biceps'),
                   s + 'loket': (s + 'biceps', s + 'biceps>' + s + 'loket'), s + 'ruka': (s + 'loket', s + 'loket>' + s + 'ruka'),
                   s + 'stehno': ('pzadek', s + 'zadek>' + s + 'stehno'), s + 'holen': (s + 'stehno', s + 'stehno>' + s + 'holen'),
                   s + 'chodidlo': (s + 'holen', s + 'holen>' + s + 'chodidlo'), s + 'prsty': (s + 'chodidlo', s + 'chodidlo>' + s + 'prsty')})
ORDER = ['bricho', 'zebra', 'hrudnik', 'krk', 'hlava'] + [s + b for s in 'pl' for b in
         ('rameno', 'biceps', 'loket', 'ruka', 'stehno', 'holen', 'chodidlo', 'prsty')]


def R(axis, deg):
    a = np.radians(deg); c, s = np.cos(a), np.sin(a)
    if axis == 'x': return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == 'y': return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def about(Rm, p):
    M = np.eye(4); M[:3, :3] = Rm; M[:3, 3] = p - Rm @ p; return M


def unmat(M): return np.concatenate([M[:3, 0], M[:3, 1], M[:3, 2], M[:3, 3]])


def wave(u, lo, hi, phase=0.0, k=1):
    """lo..hi and back over the cycle, smooth (cosine), starting at lo when u = phase."""
    return lo + (hi - lo) * 0.5 * (1 - np.cos(2 * np.pi * k * (u - phase)))


def keys(u, pts):
    """Smooth periodic key interpolation: pts = [(u, value), ...] covering 0..1 (wraps)."""
    pts = sorted(pts); us = [p[0] for p in pts] + [pts[0][0] + 1]; vs = [p[1] for p in pts] + [pts[0][1]]
    u = u % 1.0
    if u < us[0]: u += 1
    for i in range(len(pts)):
        if us[i] <= u <= us[i + 1]:
            f = (u - us[i]) / (us[i + 1] - us[i]); f = f * f * (3 - 2 * f)
            return vs[i] + (vs[i + 1] - vs[i]) * f
    return vs[0]


def pose(p):
    """p: angles in degrees -> {bone: 3x3 local rotation}, root rotation (J frame)."""
    g = lambda k, d=0.0: p.get(k, d)
    loc = {}
    root = R('y', g('yaw')) @ R('x', g('roll')) @ R('z', -g('pitch'))
    loc['bricho'] = R('z', -g('spine') * 0.4); loc['zebra'] = R('z', -g('spine') * 0.3)
    loc['hrudnik'] = R('z', -g('spine') * 0.3)
    loc['krk'] = R('z', g('neck') * 0.6) @ R('y', g('look') * 0.5); loc['hlava'] = R('z', g('neck') * 0.4) @ R('y', g('look') * 0.5)
    for s, sign in (('p', 1), ('l', -1)):
        loc[s + 'rameno'] = R('z', g(s + 'sh_flex') * 0.15) @ R('x', sign * g(s + 'sh_abd') * 0.15)
        loc[s + 'biceps'] = R('z', g(s + 'sh_flex') * 0.85) @ R('x', sign * g(s + 'sh_abd') * 0.85) @ R('y', sign * g(s + 'sh_rot'))
        loc[s + 'loket'] = R('z', g(s + 'elbow'))
        loc[s + 'ruka'] = R('z', g(s + 'wrist')) @ R('y', sign * g(s + 'palm'))
        loc[s + 'stehno'] = R('z', g(s + 'hip')) @ R('x', sign * g(s + 'hip_abd')) @ R('y', sign * g(s + 'hip_rot'))
        loc[s + 'holen'] = R('z', -g(s + 'knee'))
        loc[s + 'chodidlo'] = R('z', -g(s + 'ankle'))
        loc[s + 'prsty'] = np.eye(3)
    return root, loc


def fk(p, offset):
    root, loc = pose(p)
    G = {}
    Mroot = np.eye(4); Mroot[:3, :3] = BASE @ root; Mroot[:3, 3] = offset
    G['pzadek'] = G['lzadek'] = Mroot
    for b in ORDER:
        par, joint = PARENT[b]
        G[b] = G[par] @ about(loc[b], J[joint])
    return G


def head_of(G): return (G['hlava'] @ np.append(J['krk>hlava'], 1))[:3]


def ease(u): return u * u * (3 - 2 * u)


def lerp(a, b, f): return {k: a.get(k, 0.0) * (1 - f) + b.get(k, 0.0) * f for k in set(a) | set(b)}


def both(d, **kw):
    for k, v in kw.items(): d['p' + k] = v; d['l' + k] = v
    return d


# ---- the moves: each returns the pose parameters at cycle position u ------------------------------------
def tread(u):
    p = {'pitch': 12, 'neck': 10, 'spine': 4}
    for s, ph in (('p', 0.0), ('l', 0.5)):             # eggbeater: legs alternate
        p[s + 'hip'] = wave(u, 35, 55, ph); p[s + 'hip_abd'] = wave(u, 18, 32, ph + 0.25)
        p[s + 'knee'] = wave(u, 45, 100, ph); p[s + 'ankle'] = wave(u, -15, 10, ph + 0.1)
        p[s + 'hip_rot'] = wave(u, -5, 10, ph)
    for s in 'pl':                                       # sculling: hands sweep in and out together, twice a cycle
        p[s + 'sh_flex'] = 45; p[s + 'sh_abd'] = wave(u, 20, 55, 0, 2); p[s + 'elbow'] = wave(u, 75, 55, 0, 2)
        p[s + 'palm'] = wave(u, -20, 20, 0.25, 2); p[s + 'wrist'] = 0; p[s + 'sh_rot'] = 10
    return p


def breast(u):
    """Heads-up combat breaststroke, one cycle: pull (0-.3), hands to chin (.3-.45), reach (.45-.7), glide."""
    p = {'pitch': keys(u, [(0, 52), (0.35, 46), (0.65, 58)]), 'neck': keys(u, [(0, 42), (0.35, 48), (0.65, 38)]), 'spine': 6}
    for s in 'pl':
        p[s + 'sh_flex'] = keys(u, [(0.0, 165), (0.15, 120), (0.3, 70), (0.42, 45), (0.6, 140), (0.75, 168)])
        p[s + 'sh_abd'] = keys(u, [(0.0, 12), (0.15, 45), (0.3, 50), (0.42, 15), (0.6, 8), (0.75, 10)])
        p[s + 'elbow'] = keys(u, [(0.0, 10), (0.15, 40), (0.3, 95), (0.42, 135), (0.6, 50), (0.75, 8)])
        p[s + 'sh_rot'] = keys(u, [(0.0, 0), (0.2, 25), (0.42, 10), (0.75, 0)])
        p[s + 'palm'] = keys(u, [(0.0, 30), (0.3, -10), (0.6, 0)])
        p[s + 'wrist'] = 0
        # frog kick: legs long through the pull, knees up as the hands come in, kick as the arms reach
        p[s + 'hip'] = keys(u, [(0.0, 8), (0.3, 12), (0.45, 55), (0.62, 15), (0.8, 6)])
        p[s + 'hip_abd'] = keys(u, [(0.0, 6), (0.3, 10), (0.45, 32), (0.62, 20), (0.8, 4)])
        p[s + 'knee'] = keys(u, [(0.0, 8), (0.3, 20), (0.45, 115), (0.62, 25), (0.8, 6)])
        p[s + 'ankle'] = keys(u, [(0.0, 40), (0.3, 20), (0.45, -15), (0.62, 25), (0.8, 45)])
        p[s + 'hip_rot'] = keys(u, [(0.0, 0), (0.45, 25), (0.62, 10), (0.8, 0)])
    return p


def back(u):
    p = {'pitch': -10, 'neck': -6, 'spine': -4}
    for s, ph in (('p', 0.0), ('l', 0.5)):
        p[s + 'hip'] = wave(u, 10, 45, ph); p[s + 'knee'] = wave(u, 15, 70, ph + 0.15); p[s + 'hip_abd'] = 10
        p[s + 'ankle'] = wave(u, 30, 5, ph + 0.15)
    for s in 'pl':                                       # hands push water forward, recover close to the body
        p[s + 'sh_flex'] = keys(u, [(0.0, 15), (0.45, 70), (0.6, 60), (0.9, 20)])
        p[s + 'sh_abd'] = keys(u, [(0.0, 35), (0.45, 25), (0.9, 40)])
        p[s + 'elbow'] = keys(u, [(0.0, 95), (0.45, 25), (0.6, 40), (0.9, 100)])
        p[s + 'palm'] = 70; p[s + 'wrist'] = -20
    return p


def side(u, lead):
    """Sideways (lead 'l' or 'p'): scissor kick to the side, the leading arm sculling out and pulling in."""
    trail = 'p' if lead == 'l' else 'l'
    p = {'pitch': 8, 'neck': 6, 'roll': -8 if lead == 'p' else 8, 'look': -15 if lead == 'l' else 15}
    p[lead + 'hip_abd'] = keys(u, [(0.0, 10), (0.4, 40), (0.6, 12)]); p[trail + 'hip_abd'] = keys(u, [(0.0, 5), (0.4, -5), (0.6, 15)])
    for s in 'pl':
        p[s + 'hip'] = keys(u, [(0.0, 25), (0.4, 45), (0.6, 15)]); p[s + 'knee'] = keys(u, [(0.0, 40), (0.4, 95), (0.6, 20)])
        p[s + 'ankle'] = keys(u, [(0.0, 20), (0.4, -5), (0.6, 35)])
    p[lead + 'sh_flex'] = 30; p[lead + 'sh_abd'] = keys(u, [(0.0, 30), (0.35, 85), (0.7, 35)])
    p[lead + 'elbow'] = keys(u, [(0.0, 90), (0.35, 20), (0.7, 100)]); p[lead + 'palm'] = 60
    p[trail + 'sh_flex'] = 40; p[trail + 'sh_abd'] = wave(u, 20, 45, 0, 2); p[trail + 'elbow'] = 70
    return p


def turn(u, to):
    p = tread(u)
    sgn = -1 if to == 'l' else 1
    p['yaw'] = sgn * wave(u, 0, 12)
    for s, ph in (('p', 0.0), ('l', 0.5)):               # the arms scull against each other
        p[s + 'sh_flex'] = wave(u, 20, 75, ph); p[s + 'sh_abd'] = 40; p[s + 'elbow'] = wave(u, 90, 35, ph)
        p[s + 'palm'] = 50 if (s == 'p') == (to == 'l') else -50
    return p


def float_down(u):
    p = {'pitch': 82 + wave(u, 0, 4), 'neck': -25, 'spine': 10, 'roll': wave(u, -3, 3)}
    for s in 'pl':
        p[s + 'sh_flex'] = 95; p[s + 'sh_abd'] = 50; p[s + 'elbow'] = 40; p[s + 'palm'] = 30
        p[s + 'hip'] = -5 + wave(u, 0, 4); p[s + 'hip_abd'] = 10; p[s + 'knee'] = 30; p[s + 'ankle'] = 40
    return p


MOVES = {
    'tread.rtm': tread, 'swim.rtm': breast,
    'swimlf.rtm': lambda u: dict(breast(u), yaw=-25), 'swimrf.rtm': lambda u: dict(breast(u), yaw=25),
    'swimb.rtm': back, 'swiml.rtm': lambda u: side(u, 'l'), 'swimr.rtm': lambda u: side(u, 'p'),
    'swimlb.rtm': lambda u: lerp(back(u), side(u, 'l'), 0.5), 'swimrb.rtm': lambda u: lerp(back(u), side(u, 'p'), 0.5),
    'swimturnl.rtm': lambda u: turn(u, 'l'), 'swimturnr.rtm': lambda u: turn(u, 'p'),
    # config: SwimStart is tread -> stroke (ends where SwimF starts, arms reaching), SwimStop stroke -> tread
    'swimstart.rtm': lambda u: lerp(tread(0.0), breast(0.0), ease(u)),
    'swimstop.rtm': lambda u: lerp(breast(0.75), tread(0.0), ease(u)),
    # killed (config: one-shot, terminal): the living pose goes limp into a face-down float, held at the end
    'swimdead.rtm': lambda u: lerp(tread(0.0), float_down(0.0), ease(u)),
    'swimdeadf.rtm': lambda u: lerp(breast(0.75), float_down(0.0), ease(u)),
}


# ---- falling, landing, falling into the water --------------------------------------------------------------
# The rifle stays in the hands while falling: the upper body (arms, rifle, launcher) is the stock relaxed stance
# exactly (savarestanistat.rtm, rifle lowered across the body -- the game's own grip), the legs are posed here.
LEG_BONES = [s + b for s in 'pl' for b in ('stehno', 'holen', 'chodidlo', 'prsty')]


def legs_fk(G_pelvis, p):
    root, loc = pose(p)
    G = {'pzadek': G_pelvis, 'lzadek': G_pelvis}
    for b in LEG_BONES:
        par, joint = PARENT[b]
        G[b] = G[par] @ about(loc[b], J[joint])
    return G


def fall_legs(u):
    p = {}
    for s, ph in (('p', 0.0), ('l', 0.5)):              # a slow alternating bicycle, knees soft, feet loose
        p[s + 'hip'] = wave(u, 12, 38, ph); p[s + 'knee'] = wave(u, 25, 60, ph + 0.1)
        p[s + 'hip_abd'] = 8; p[s + 'ankle'] = wave(u, 0, 18, ph)
    return p


def quat(Rm):
    """Rotation matrix -> quaternion (w, x, y, z), from the largest component so it holds near 180 deg too
    (dividing by w alone tore the blends of half-turned bones apart)."""
    tr = Rm[0, 0] + Rm[1, 1] + Rm[2, 2]
    if tr > 0:
        s = 2 * np.sqrt(tr + 1)
        return np.array([s / 4, (Rm[2, 1] - Rm[1, 2]) / s, (Rm[0, 2] - Rm[2, 0]) / s, (Rm[1, 0] - Rm[0, 1]) / s])
    i = int(np.argmax([Rm[0, 0], Rm[1, 1], Rm[2, 2]])); j, k = (i + 1) % 3, (i + 2) % 3
    s = 2 * np.sqrt(max(1e-12, 1 + Rm[i, i] - Rm[j, j] - Rm[k, k]))
    q = np.zeros(4); q[0] = (Rm[k, j] - Rm[j, k]) / s; q[1 + i] = s / 4
    q[1 + j] = (Rm[j, i] + Rm[i, j]) / s; q[1 + k] = (Rm[k, i] + Rm[i, k]) / s
    return q


def unquat(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def blend_pose(A, B, f):
    """A whole pose part way from A to B: each bone's rotation slerped, its position then rebuilt from its
    parent's joint, so the skeleton stays joined (blending bones one by one opened a knee 0.25 m mid-blend).
    Bones outside the skeleton (the rifle, the launcher) blend on their own."""
    G = {b: blend(A[b], B[b], f) for b in A}
    for b in ORDER:
        par, joint = PARENT[b]
        j = J[joint]
        M = G[b].copy(); M[:3, 3] = (G[par] @ np.append(j, 1))[:3] - M[:3, :3] @ j
        G[b] = M
    G['lzadek'] = G['pzadek']
    return G


def blend(A, B, f):
    """A bone matrix part way from A to B: positions lerped, rotations slerped (both with uniform scale 1)."""
    qa, qb = quat(A[:3, :3]), quat(B[:3, :3])
    if qa @ qb < 0: qb = -qb
    d = np.clip(qa @ qb, -1, 1); th = np.arccos(d)
    q = qa if th < 1e-5 else (np.sin((1 - f) * th) * qa + np.sin(f * th) * qb) / np.sin(th)
    M = np.eye(4); M[:3, :3] = unquat(q); M[:3, 3] = A[:3, 3] * (1 - f) + B[:3, 3] * f
    return M


def make_fall_moves(stock_dir, dst, names, tread0):
    stock = read_rtm(os.path.join(stock_dir, 'savarestanistat.rtm'))
    sm = {b: mat4(m) for b, m in zip(stock['names'], stock['phases'][0][1])}
    upper = {b: M for b, M in sm.items() if b not in LEG_BONES and b not in ('pzadek', 'lzadek')}

    def fall_pose(u):
        G = dict(upper); G.update(legs_fk(sm['pzadek'], fall_legs(u)))
        return G

    def write(name, count, frame, step=(0.0, 0.0, 0.0)):
        phases = []
        for i in range(count):
            t = i / (count - 1); G = frame(t)
            phases.append((t, [unmat(G[b]) for b in names]))
        write_rtm(os.path.join(dst, name), step, names, phases)
        print(f'{name:14s} {count} phases')

    write('fall.rtm', 21, fall_pose)

    # The touchdown is the game's own: the landing of the stock "jump down from a tank with the rifle"
    # (sdoskok.rtm, CombatGetOutTank) from the moment the feet reach the ground (t 0.36: knees take the
    # weight, down to the deepest crouch at 0.51, up to 0.80), then blended up into the relaxed stance. A
    # procedural squat stuck on the stance's upper body read as a man squashed through a pipe (owner). The
    # stock move travels out of the tank (its pelvis drifts 0.5 m forward and back); here he lands in place,
    # so every frame is moved level to keep the pelvis over the stance's.
    jump = read_rtm(os.path.join(stock_dir, 'sdoskok.rtm'))
    jump_frames = [(t, {b: mat4(m) for b, m in zip(jump['names'], mats)}) for t, mats in jump['phases']]

    def jump_at(t):
        for (t0, A), (t1, B) in zip(jump_frames, jump_frames[1:]):
            if t0 <= t <= t1:
                f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
                G = blend_pose({b: A[b] for b in names}, {b: B[b] for b in names}, f)
                shift = np.eye(4); shift[[0, 2], 3] = sm['pzadek'][[0, 2], 3] - G['pzadek'][[0, 2], 3]
                return {b: shift @ M for b, M in G.items()}
        raise ValueError(t)

    def land(u):
        if u <= 0.75:
            return jump_at(0.36 + (0.80 - 0.36) * u / 0.75)
        A = jump_at(0.80); f = ease((u - 0.75) / 0.25)
        return blend_pose(A, {b: sm[b] for b in names}, f)
    write('fallland.rtm', 21, land)

    # into the water: from the fall (rifle in the hands) to treading (rifle slung), every bone blended
    F0 = fall_pose(0.0)
    write('swimin.rtm', 21, lambda u: blend_pose({b: F0[b] for b in names}, {b: tread0[b] for b in names}, ease(u)))


def main():
    src, stock_dir, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(dst, exist_ok=True)
    chest_to_gun = None
    tread0 = None
    for name in sorted(os.listdir(src)):
        if not name.endswith('.rtm'): continue
        ref = read_rtm(os.path.join(src, name)); names = ref['names']
        if chest_to_gun is None:                         # the slung rifle (zbran, roura) rides with the chest
            m0 = ref['phases'][0][1]; H = mat4(m0[names.index('hrudnik')])
            chest_to_gun = {b: np.linalg.inv(H) @ mat4(m0[names.index(b)]) for b in ('zbran', 'roura')}
        if name not in MOVES:
            fix.fix_legs(names, ref['phases'], []); fix.fix_feet(names, ref['phases'], []); fix.fix_shoulders(names, ref['phases'], [])
            write_rtm(os.path.join(dst, name), ref['step'], names, ref['phases'])
            print(f'{name:14s} kept (Malprave, leg repair)')
            continue
        # where the engine expects the head: Malprave's mean head position over the move
        want = np.mean([(mat4(m[names.index('hlava')]) @ np.append(J['krk>hlava'], 1))[:3] for t, m in ref['phases']], axis=0)
        n = len(ref['phases'])
        params = [MOVES[name](t) for t, m in ref['phases']]
        heads = [head_of(fk(p, np.zeros(3))) for p in params]
        offset = want - np.mean(heads, axis=0)
        phases = []
        for (t, m), p in zip(ref['phases'], params):
            G = fk(p, offset)
            mats = []
            for b in names:
                if b in G: mats.append(unmat(G[b]))
                else: mats.append(unmat(G['hrudnik'] @ chest_to_gun[b]))
            phases.append((t, mats))
        if name == 'tread.rtm':
            tread0 = {b: mat4(m) for b, m in zip(names, phases[0][1])}
            tread_names = names
        write_rtm(os.path.join(dst, name), ref['step'], names, phases)
        print(f'{name:14s} {n} phases, head {np.round(want, 2)}')
    make_fall_moves(stock_dir, dst, tread_names, tread0)


if __name__ == '__main__':
    main()
