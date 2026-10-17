# Fit the CWA MC soldier's joint pivots (rest model space) from the stock animations, for the Blender rig
# (blender_swim_rig.py). A joint between parent a and child b is the point both bones carry to the same place
# in every frame: D_a p = D_b p, solved by least squares over all of DTA\Anim.pbo.
# Usage: python fit_mc_joints.py <extracted DTA\Anim.pbo dir> [out json, default mc_joints.json beside this]
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtm_io import mat4, read_rtm  # noqa: E402

# child -> parent: the MC hierarchy (make_swim_rtms.py PARENT; the legs hang from the pelvis)
PARENT = {'bricho': 'pzadek', 'zebra': 'bricho', 'hrudnik': 'zebra', 'krk': 'hrudnik', 'hlava': 'krk'}
for s in 'pl':
    PARENT.update({s + 'rameno': 'hrudnik', s + 'biceps': s + 'rameno', s + 'loket': s + 'biceps',
                   s + 'ruka': s + 'loket', s + 'stehno': 'pzadek', s + 'holen': s + 'stehno',
                   s + 'chodidlo': s + 'holen', s + 'prsty': s + 'chodidlo'})


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mc_joints.json')
    rows = {c: ([], []) for c in PARENT}
    used = 0
    for name in sorted(os.listdir(src)):
        if not name.lower().endswith('.rtm'):
            continue
        try:
            r = read_rtm(os.path.join(src, name))
        except (ValueError, OSError):
            continue
        idx = {b: i for i, b in enumerate(r['names'])}
        if not all(b in idx for b in PARENT) or 'pzadek' not in idx:
            continue
        used += 1
        for _, mats in r['phases']:
            for c, p in PARENT.items():
                A, B = mat4(mats[idx[p]]), mat4(mats[idx[c]])
                rows[c][0].append(A[:3, :3] - B[:3, :3])
                rows[c][1].append(B[:3, 3] - A[:3, 3])
    joints, err = {}, {}
    for c, (As, bs) in rows.items():
        A = np.concatenate(As); b = np.concatenate(bs)
        p, *_ = np.linalg.lstsq(A, b, rcond=None)
        joints[c] = [round(float(v), 5) for v in p]
        err[c] = float(np.sqrt(np.mean((A @ p - b) ** 2)))
    with open(out, 'w') as f:
        json.dump({'source': 'fitted from the stock CWA animations (DTA\\Anim.pbo) by fit_mc_joints.py',
                   'space': 'rest model space of the MC soldier (x, y up, z), metres',
                   'parent': PARENT, 'joints': joints}, f, indent=1)
    print(f'{used} animations; rms residual per joint (m):')
    for c in PARENT:
        print(f'  {PARENT[c]:>9s} > {c:9s} {np.round(joints[c], 3)}  {err[c]:.4f}')


if __name__ == '__main__':
    main()
