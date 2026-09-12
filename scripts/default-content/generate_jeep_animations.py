"""Author RTM_0101 Jeep pose drafts. No original animation is read or copied.

Model-space calibration describes the existing MC soldier/weapon interface, not
animation keyframes. Output is development-only until in-game pose acceptance.
"""

import argparse
import json
import math
from pathlib import Path
import struct

import numpy as np


def unit(v):
    v = np.asarray(v, dtype=float)
    length = np.linalg.norm(v)
    if not np.isfinite(length) or length < 1e-9:
        raise ValueError("Degenerate direction")
    return v / length


def rotation(axis, degrees):
    x, y, z = unit(axis)
    a = math.radians(degrees)
    c, s, t = math.cos(a), math.sin(a), 1 - math.cos(a)
    return np.array([[t*x*x+c, t*x*y-s*z, t*x*z+s*y],
                     [t*x*y+s*z, t*y*y+c, t*y*z-s*x],
                     [t*x*z-s*y, t*y*z+s*x, t*z*z+c]])


def frame(r=None, p=None):
    m = np.eye(4)
    if r is not None:
        m[:3, :3] = r
    if p is not None:
        m[:3, 3] = p
    return m


def point(m, p):
    return m[:3, :3] @ np.asarray(p) + m[:3, 3]


def pivot(axis, degrees, p):
    r = rotation(axis, degrees)
    p = np.asarray(p)
    return frame(r, p - r @ p)


def swing(source, destination):
    a, b = unit(source), unit(destination)
    cosine = np.clip(a @ b, -1, 1)
    if cosine > 1 - 1e-9:
        return np.eye(3)
    axis = np.cross(a, b)
    if np.linalg.norm(axis) < 1e-8:
        axis = np.cross(a, [1, 0, 0] if abs(a[0]) < .9 else [0, 1, 0])
    return rotation(axis, math.degrees(math.acos(cosine)))


def segment(rest_start, rest_end, target_start, target_end):
    r = swing(np.subtract(rest_end, rest_start), np.subtract(target_end, target_start))
    return frame(r, np.asarray(target_start) - r @ rest_start)


def elbow(shoulder, wrist, upper_length, fore_length, pole):
    """Two-bone IK, rejecting unreachable grips instead of stretching arms."""
    delta = np.subtract(wrist, shoulder)
    distance = np.linalg.norm(delta)
    if not abs(upper_length - fore_length) + 1e-6 < distance < upper_length + fore_length - 1e-6:
        raise ValueError(f"Unreachable hand grip: {distance:.3f} m")
    direction = delta / distance
    along = (upper_length**2 - fore_length**2 + distance**2) / (2 * distance)
    pole = np.asarray(pole)
    bend = unit(pole - direction * (pole @ direction))
    return np.asarray(shoulder) + direction * along + bend * math.sqrt(max(0, upper_length**2 - along**2))


def smooth(t):
    return t*t*(3-2*t)


def rest_point(spec, p):
    # The model API exposes centered ODOL vertices; RTMs act before centering.
    return np.asarray(p) + np.asarray(spec['model_center'])


def rest_proxy(spec):
    proxy = np.array(spec['weapon_proxy'], dtype=float)
    proxy[:3, 3] += spec['model_center']
    return proxy


def pose(spec, raised, recoil=0.0, reload=0.0):
    # RTMs contain absolute deformation matrices, not parent-relative joints.
    root = frame(rotation([0, 1, 0], spec['root_yaw']), spec['root_offset'])
    torso = root @ pivot([0, 1, 0], spec['torso_yaw'] * raised, rest_point(spec, spec['waist']))
    result = {name: root.copy() for name in spec['bones']}
    for name in ('bricho', 'zebra', 'hrudnik', 'krk', 'hlava', 'prameno', 'lrameno', 'roura'):
        result[name] = torso.copy()
    # The neck lies on the torso's vertical pivot axis. Turn the head the
    # remaining angle toward the sights without twisting the shoulders further.
    head = torso @ pivot([0, 1, 0], spec['head_yaw'] * raised, rest_point(spec, spec['waist']))
    result['krk'] = head.copy()
    result['hlava'] = head
    for side, leg in spec['legs'].items():
        thigh = root @ pivot([0, 0, 1], spec['thigh_bend'], rest_point(spec, leg['hip']))
        shin = thigh @ pivot([0, 0, 1], spec['knee_bend'], rest_point(spec, leg['knee']))
        foot = shin @ pivot([0, 0, 1], -spec['thigh_bend']-spec['knee_bend'], rest_point(spec, leg['ankle']))
        result[side+'stehno'] = thigh
        result[side+'holen'] = shin
        result[side+'chodidlo'] = foot
        result[side+'prsty'] = foot.copy()

    gun_pos = (1-raised)*np.array(spec['gun_lowered']) + raised*np.array(spec['gun_raised'])
    gun_r = rotation([0, 1, 0], spec['gun_yaw']) @ rotation([1, 0, 0], 25*(1-raised))
    gun_pos -= gun_r[:, 2] * recoil
    gun = frame(gun_r, gun_pos)
    proxy = rest_proxy(spec)
    # Original M16 points down model -X, not +Z. Author grip/recoil coordinates
    # in a barrel-forward frame, then map the actual model into that frame.
    result['zbran'] = gun @ frame(rotation([0, 1, 0], 90)) @ np.linalg.inv(proxy)
    for side, arm in spec['arms'].items():
        shoulder_rest, elbow_rest, palm_rest = (rest_point(spec, arm[j]) for j in ('shoulder', 'elbow', 'palm'))
        shoulder = point(torso, shoulder_rest)
        palm = point(gun, spec['grips'][side])
        # Left support hand travels to the magazine, then returns to its grip.
        if side == 'l':
            palm += reload * (point(gun, spec['magazine_grip']) - palm)
        # The contact is the palm, not the wrist. Solve the straight wrist/hand
        # extension with the forearm so both keep a rigid, continuous transform.
        mid = elbow(shoulder, palm,
                    np.linalg.norm(np.subtract(arm['elbow'], arm['shoulder'])),
                    np.linalg.norm(np.subtract(arm['palm'], arm['elbow'])),
                    arm['pole'])
        result[side+'biceps'] = segment(shoulder_rest, elbow_rest, shoulder, mid)
        result[side+'loket'] = segment(elbow_rest, palm_rest, mid, palm)
        hand_r = rotation(palm-mid, arm['hand_degrees']) @ result[side+'loket'][:3, :3]
        result[side+'ruka'] = frame(hand_r, palm - hand_r @ palm_rest)
    return result


def clip(spec, kind, count=31):
    for i in range(count):
        t = i / (count-1)
        raised, recoil, reload = 1.0, 0.0, 0.0
        if kind == 'idle':
            raised = 0.0
        elif kind == 'raise':
            raised = smooth(t)
        elif kind == 'lower':
            raised = 1-smooth(t)
        elif kind == 'recoil':
            recoil = .025 * math.sin(math.pi*t)**2 * math.exp(-4*t)
        elif kind == 'reload':
            reload = math.sin(math.pi*t)**2
        elif kind != 'aim':
            raise ValueError(kind)
        yield t, pose(spec, raised, recoil, reload)


def encode(spec, phases):
    phases = list(phases)
    names = spec['bones']
    if len(names) != len(set(names)) or not phases:
        raise ValueError('Duplicate bones or empty animation')
    def name_bytes(name):
        data = name.encode('ascii')
        if not data or len(data) >= 32 or b'\0' in data:
            raise ValueError('Invalid RTM bone name')
        return data.ljust(32, b'\0')
    data = bytearray(b'RTM_0101' + struct.pack('<3f2i', 0, 0, 0, len(phases), len(names)))
    for name in names:
        data += name_bytes(name)
    last_time = -1.0
    for time, matrices in phases:
        if not last_time < time <= 1:
            raise ValueError('Non-monotonic phase')
        last_time = time
        data += struct.pack('<f', time)
        for name in names:
            m = matrices[name]
            if (not np.isfinite(m).all()
                    or not np.allclose(m[:3, :3].T @ m[:3, :3], np.eye(3), atol=1e-4)
                    or not np.isclose(np.linalg.det(m[:3, :3]), 1, atol=1e-4)):
                raise ValueError(f'Non-rigid matrix: {name}')
            data += name_bytes(name) + struct.pack('<12f', *m[:3, :].T.flatten())
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spec', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--override', type=Path)
    parser.add_argument('--prefix', choices=('jeep', 'uh60'), default='jeep')
    args = parser.parse_args()
    spec = json.loads(args.spec.read_text())
    if args.override:
        spec.update(json.loads(args.override.read_text()))
    outputs = {kind: encode(spec, clip(spec, kind)) for kind in ('idle', 'raise', 'aim', 'recoil', 'reload', 'lower')}
    args.out.mkdir(parents=True, exist_ok=True)
    for kind, data in outputs.items():
        (args.out / f'{args.prefix}_{kind}.rtm').write_bytes(data)
    print('Generated six own draft RTMs; in-game pose/transition acceptance is still required.')


if __name__ == '__main__':
    main()
