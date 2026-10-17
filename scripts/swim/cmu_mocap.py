# CMU Graphics Lab motion capture (mocap.cs.cmu.edu, ASF/AMC) -> arm motion for the swim moves. The CMU data "may be
# copied, modified, or redistributed without permission" (the database's FAQ); the swimming takes are subjects 125
# and 126. Owner, 2026-10-17: "see if our animations can be improved with these real motion captures ... only for
# the arms". Plain numpy: it runs inside Blender (surface_moves.py) and outside it.
#
#   mc = cmu_mocap.load(r'<dir>\125.asf', r'<dir>\125_01.amc')     # forward kinematics, every frame
#   cyc = cmu_mocap.cycles(mc)                                       # the strokes in the take, cleanest first
#   arms = cmu_mocap.arm_targets(mc, cyc[0], 41)                     # per phase, per side: upper arm, forearm, palm
#
# The arm targets are directions in the chest's own frame (x across to the right shoulder, y up the spine, z out of
# the back), so they carry onto any skeleton whatever the body around them does.
import math
import os

import numpy as np


def _rot(axis, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    if axis == 'x':
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == 'y':
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def _euler(order, angles):
    """ASF/AMC rotations: applied about the fixed axes in <order>, so the matrix is R_last ... R_first."""
    m = np.eye(3)
    for ax, ang in zip(order, angles):
        m = _rot(ax, ang) @ m
    return m


def parse_asf(path):
    bones, hier, root = {}, {}, {}
    lines = open(path).read().splitlines()
    i, section = 0, None
    while i < len(lines):
        ln = lines[i].strip()
        if ln.startswith(':'):
            section = ln.split()[0]
            if section == ':root':
                pass
        elif section == ':root' and ln:
            k, *v = ln.split()
            root[k] = v
        elif section == ':bonedata' and ln == 'begin':
            b = {'dof': []}
            i += 1
            while lines[i].strip() != 'end':
                k, *v = lines[i].split()
                if k == 'name':
                    b['name'] = v[0]
                elif k == 'direction':
                    b['dir'] = np.array([float(x) for x in v])
                elif k == 'length':
                    b['len'] = float(v[0])
                elif k == 'axis':
                    b['axis'] = ([float(x) for x in v[:3]], v[3].lower())
                elif k == 'dof':
                    b['dof'] = [d.lower() for d in v]
                i += 1
            bones[b['name']] = b
        elif section == ':hierarchy' and ln and ln not in ('begin', 'end'):
            p, *kids = ln.split()
            for k in kids:
                hier[k] = p
        i += 1
    return bones, hier, root


def parse_amc(path):
    frames, cur = [], None
    for ln in open(path):
        ln = ln.strip()
        if not ln or ln.startswith('#') or ln.startswith(':'):
            continue
        if ln.isdigit():
            cur = {}
            frames.append(cur)
            continue
        k, *v = ln.split()
        cur[k] = [float(x) for x in v]
    return frames


class Mocap:
    """Joint positions per frame: start[name] / end[name] arrays (frames, 3), and the bone matrices."""


def load(asf, amc, fps=120.0):
    bones, hier, root = parse_asf(asf)
    frames = parse_amc(amc)
    order = []  # parents before children

    def visit(n):
        for k, p in hier.items():
            if p == n and k not in order:
                order.append(k)
                visit(k)

    visit('root')
    C = {n: _euler(b['axis'][1], b['axis'][0]) for n, b in bones.items()}
    rorder = [o[1].lower() for o in root['order'] if o.startswith('R')]
    raxis = root.get('axis', ['XYZ'])[0].lower()
    n = len(frames)
    start = {k: np.zeros((n, 3)) for k in ['root', *order]}
    end = {k: np.zeros((n, 3)) for k in ['root', *order]}
    mats = {k: np.zeros((n, 3, 3)) for k in ['root', *order]}
    for f, fr in enumerate(frames):
        rv = fr['root']
        pos = np.array(rv[:3])
        rang = dict(zip(rorder, rv[3:6]))
        M = {'root': _euler(raxis, [rang.get(a, 0.0) for a in raxis])}
        E = {'root': pos}
        start['root'][f] = end['root'][f] = pos
        mats['root'][f] = M['root']
        for nme in order:
            b = bones[nme]
            par = hier[nme]
            vals = dict(zip(b['dof'], fr.get(nme, [0.0] * len(b['dof']))))
            ang = [vals.get('r' + a, 0.0) for a in 'xyz']
            Cb = C[nme]
            # the dof rotation acts in the bone's own axis frame C; direction is given in the rest (global) pose
            M[nme] = M[par] @ Cb @ _euler('xyz', ang) @ Cb.T
            s = E[par]
            E[nme] = s + M[nme] @ b['dir'] * b['len']
            start[nme][f], end[nme][f], mats[nme][f] = s, E[nme], M[nme]
    mc = Mocap()
    mc.start, mc.end, mc.mats, mc.fps, mc.n, mc.bones = start, end, mats, fps, n, bones
    return mc


def chest_frame(mc):
    """Per frame: (origin, 3x3 with columns x right, y up the spine, z = x cross y, out of the back) from the joints."""
    r, l = mc.start['rhumerus'], mc.start['lhumerus']
    x = r - l
    y = mc.start['lowerneck'] - mc.start['lowerback']
    x /= np.linalg.norm(x, axis=1, keepdims=True)
    y = y - x * np.sum(y * x, axis=1, keepdims=True)
    y /= np.linalg.norm(y, axis=1, keepdims=True)
    z = np.cross(x, y)
    return (r + l) / 2, np.stack([x, y, z], axis=2)


def _local(mc, v):
    _, R = chest_frame(mc)
    return np.einsum('fji,fj->fi', R, v)  # R^T v per frame


def arm_series(mc):
    """Per frame and side ('r', 'l'): upper arm, forearm and palm normal as unit vectors in the chest frame."""
    out = {}
    for s in 'rl':
        sh, el, wr = mc.start[s + 'humerus'], mc.start[s + 'radius'], mc.start[s + 'hand']
        hand = mc.end[s + 'hand'] - mc.start[s + 'hand']
        thumb = mc.end[s + 'thumb'] - mc.start[s + 'thumb']
        up, fo = el - sh, wr - el
        # the palm: checked against the stroke itself -- with this sign it faces out at the glide and down and back on
        # the pull (the other sign had it facing the head while the hands push the water back)
        palm = np.cross(thumb, hand) if s == 'r' else np.cross(hand, thumb)
        u = _local(mc, up / np.linalg.norm(up, axis=1, keepdims=True))
        f = _local(mc, fo / np.linalg.norm(fo, axis=1, keepdims=True))
        p = _local(mc, palm / np.maximum(np.linalg.norm(palm, axis=1, keepdims=True), 1e-9))
        out[s] = (u, f, p)
    return out


def cycles(mc, lo=0.8, hi=3.0):
    """The stroke cycles in a take: starts at the glide (both wrists furthest up the spine, i.e. reaching ahead), as
    (start frame, end frame, closure error), cleanest first."""
    o, R = chest_frame(mc)
    reach = sum(np.einsum('fj,fj->f', mc.start[s + 'hand'] - o, R[:, :, 1]) for s in 'rl')
    # peaks of the reach, at least lo seconds apart
    k = int(lo * mc.fps)
    peaks = [i for i in range(k, mc.n - k) if reach[i] == reach[i - k:i + k + 1].max()]
    ser = arm_series(mc)
    out = []
    for a, b in zip(peaks, peaks[1:]):
        if not (lo * mc.fps <= b - a <= hi * mc.fps):
            continue
        err = sum(float(np.linalg.norm(ser[s][j][a] - ser[s][j][b])) for s in 'rl' for j in range(3))
        out.append((a, b, err))
    return sorted(out, key=lambda c: c[2])


def arm_targets(mc, cyc, phases):
    """The cycle resampled to <phases> evenly spaced phases (the last repeats the first):
    {side: [(upper, forearm, palm), ...]} with side 'p' (right) / 'l' (left), the rig's names."""
    a, b, _ = cyc
    ser = arm_series(mc)
    out = {}
    for s, rig in (('r', 'p'), ('l', 'l')):
        rows = []
        for i in range(phases):
            t = a + (b - a) * i / (phases - 1)
            i0 = int(math.floor(t))
            w = t - i0
            i1 = min(i0 + 1, mc.n - 1)
            vs = []
            for j in range(3):
                v = ser[s][j][i0] * (1 - w) + ser[s][j][i1] * w
                vs.append(v / np.linalg.norm(v))
            rows.append(tuple(vs))
        out[rig] = rows
    return out


def write_table(asf, amc, out_json, phases=41):
    """The cleanest stroke of a take as a small JSON table of arm targets (what surface_moves.py reads), so the moves
    can be rebuilt without the capture files."""
    import json
    mc = load(asf, amc)
    cyc = cycles(mc)[0]
    tg = arm_targets(mc, cyc, phases)
    take = os.path.splitext(os.path.basename(amc))[0]
    doc = {
        'source': 'CMU Graphics Lab Motion Capture Database, mocap.cs.cmu.edu (subject %s, trial %s). "The motion '
                  'capture data may be copied, modified, or redistributed without permission."' % tuple(take.split('_')),
        'take': take, 'cycle_frames': [int(cyc[0]), int(cyc[1])], 'cycle_seconds': round((cyc[1] - cyc[0]) / mc.fps, 3),
        'closure': round(float(cyc[2]), 3), 'phases': phases,
        'frame': 'chest: x to the right shoulder, y up the spine, z = x cross y (out of the back)',
        'arms': {s: [[[round(float(c), 5) for c in v] for v in row] for row in rows] for s, rows in tg.items()},
    }
    with open(out_json, 'w') as f:
        json.dump(doc, f, indent=1)
    return doc


if __name__ == '__main__':
    import sys
    # python cmu_mocap.py <asf> <amc> <out.json>
    d = write_table(sys.argv[1], sys.argv[2], sys.argv[3])
    print(d['take'], d['cycle_frames'], d['cycle_seconds'], 's, closure', d['closure'])
