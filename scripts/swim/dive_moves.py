# Sinkhole W4 (diving): the underwater moves, keyed as Blender actions on the MC rig (blender_swim_rig.py) and
# exported as RTMs. Made by Claude in Blender, 2026-10-17 (owner: "make with blender ... also a diving animation").
# Run inside Blender after rig['build'](...) and rig['import_rtm'](<tread.rtm>) (the dive and the surfacing join
# treading exactly, so the tread action must be loaded):
#
#   dm = runpy.run_path(r'<repo>\scripts\swim\dive_moves.py')
#   dm['make_all']()                              # (re)keys the four actions
#   dm['export_all'](r'<repo>\content\default-packs\swim\rtm')
#
# The actions are baked to a key on every frame, so they can be reworked by hand in Blender (or the key poses below
# changed and make_all run again) and exported again with export_all.
#
# The technique: a loaded soldier without fins swims under water with the underwater breaststroke (the long pull to
# the thighs and a glide, the frog kick on the recovery), body flat, head in line, the rifle slung on the chest.
#   swimdive       surface dive (pike): lean onto the water, bend at the hips so the trunk points down while the hips
#                  stay up, pull, lift the legs straight overhead so their weight drives him under, level out into
#                  the glide (one-shot, 1.6 s)
#   swimunder      the underwater stroke, 2.2 s a cycle, 1.6 m: the body surges on the pull and drags on the
#                  recovery, the hips and head rise and dip through the cycle (looped)
#   swimunderidle  holding still under water: flat, the hands sculling ahead of the face, an easy kick (looped)
#   swimsurface    up from the glide: head and chest lift, a big pull down and a kick, into treading (one-shot)
#
# A pose is a set of channels (degrees, metres) on the rig's own axes (see blender_swim_rig.py). Between key poses
# every channel runs on a Catmull-Rom curve (periodic for the loops), and the hands, feet and head trail the body
# by a few hundredths of a cycle, so the limbs follow through instead of every joint stopping on the same frame.
import math

import bpy

FPS = 24
LOOPED = {'swimunder': (2.2, 41, (0.0, 0.0, -1.6)), 'swimunderidle': (2.4, 41, (0.0, 0.0, 0.0))}
ONESHOT = {'swimdive': (1.6, 33, (0.0, 0.0, -0.4)), 'swimsurface': (1.3, 27, (0.0, 0.0, -0.2))}
SPINE = ('bricho', 'zebra', 'hrudnik')
LIMB_BONES = [s + b for s in 'pl' for b in ('rameno', 'biceps', 'loket', 'ruka', 'stehno', 'holen', 'chodidlo', 'prsty')]
ALL = ['pzadek', 'krk', 'hlava', *SPINE, *LIMB_BONES]
SLING = ('zbran', 'roura', 'lzadek')

# channel defaults; fwd is how far forward of the rest pelvis the pelvis is, height is the pelvis bone's own height
# (treading has it at 0.96), both in metres
DEFAULT = {'pitch': 90.0, 'roll': 0.0, 'yaw': 0.0, 'fwd': 0.3, 'height': 1.15, 'spine': 0.0, 'neck': 0.0, 'head': 0.0}
for _s in 'pl':
    DEFAULT.update({_s + k: 0.0 for k in ('flex', 'abd', 'twist', 'elbow', 'wrist', 'hip', 'hipabd', 'hiptwist',
                                           'knee', 'ankle')})
# how far each channel trails the body, as a fraction of the move
LAG = {'neck': 0.02, 'head': 0.04}
for _s in 'pl':
    LAG.update({_s + 'elbow': 0.025, _s + 'wrist': 0.05, _s + 'knee': 0.02, _s + 'ankle': 0.05, _s + 'twist': 0.015})


def P(**kw):
    """A key pose: arm=/leg= set both sides (dicts), the rest are channels."""
    out = {}
    for side_key, prefix in (('arm', ('flex', 'abd', 'twist', 'elbow', 'wrist')), ('leg', None)):
        d = kw.pop(side_key, None)
        if d:
            for s in 'pl':
                for k, v in d.items():
                    out[s + k] = v
    out.update(kw)
    return out


def arms(flex, abd=0.0, twist=0.0, elbow=0.0, wrist=0.0):
    return dict(flex=flex, abd=abd, twist=twist, elbow=elbow, wrist=wrist)


def legs(hip, abd=0.0, twist=0.0, knee=0.0, ankle=0.0):
    return dict(hip=hip, hipabd=abd, hiptwist=twist, knee=knee, ankle=ankle)


# ---- the moves' key poses -----------------------------------------------------------------------------------------
GLIDE = P(pitch=90, fwd=0.3, height=1.15, neck=-8, head=-4, arm=arms(172, -6, 0, 6, -5), leg=legs(-4, 2, 0, 4, -45))


def under_keys():
    """One cycle of the underwater breaststroke (fwd: the surge about the steady swim speed the engine adds)."""
    return [
        (0.00, GLIDE),
        (0.12, P(pitch=91, fwd=0.31, height=1.15, neck=-9, head=-4, arm=arms(160, 38, -25, 12, 10), leg=legs(-4, 2, 0, 4, -45))),
        (0.27, P(pitch=95, fwd=0.34, height=1.13, neck=-12, head=-6, spine=5, arm=arms(96, 55, -35, 95, 15), leg=legs(-2, 3, 0, 8, -45))),
        (0.40, P(pitch=91, fwd=0.40, height=1.14, neck=-6, head=-2, arm=arms(24, 14, 10, 8, -20), leg=legs(-4, 2, 0, 5, -45))),
        (0.50, P(pitch=90, fwd=0.40, height=1.15, neck=-6, head=-2, arm=arms(22, 12, 10, 10, -15), leg=legs(-4, 2, 0, 4, -45))),
        (0.64, P(pitch=84, fwd=0.32, height=1.12, neck=-16, head=-6, spine=6, arm=arms(50, 12, -10, 135, 10), leg=legs(46, 28, 25, 112, 20))),
        (0.76, P(pitch=93, fwd=0.27, height=1.17, neck=-10, head=-4, arm=arms(140, 10, 0, 40, 0), leg=legs(16, 30, 10, 40, -15))),
        (0.88, P(pitch=91, fwd=0.29, height=1.16, neck=-8, head=-4, arm=arms(168, -2, 0, 10, -5), leg=legs(-2, 6, 0, 8, -40))),
    ]


def idle_keys():
    """Holding position under water: flat with the head up a little, hands sculling ahead of the face out and in twice a
    cycle, an alternating easy kick."""
    keys = []
    for i in range(8):
        u = i / 8
        w = math.cos(4 * math.pi * u)
        p = P(pitch=80 + 3 * math.sin(2 * math.pi * u), fwd=0.3, height=1.15 + 0.02 * math.sin(2 * math.pi * u + 1),
              neck=-22, head=-10, spine=-4, arm=arms(125, 44 + 14 * w, -22 * w, 65 - 12 * w, 8 * w))
        for s, ph in (('p', 0.0), ('l', 0.5)):
            c = math.cos(2 * math.pi * (u + ph))
            p.update({s + 'hip': 8 + 8 * c, s + 'hipabd': 8, s + 'hiptwist': 6, s + 'knee': 28 + 18 * c, s + 'ankle': -38 + 8 * c})
        keys.append((u, p))
    return keys


def dive_keys():
    """The surface dive. The first key is where the blend out of treading lands (see TREAD_IN)."""
    return [
        (0.00, P(pitch=20, fwd=0.0, height=0.98, neck=8, head=4, spine=6, arm=arms(70, 30, 0, 50, 0), leg=legs(35, 18, 10, 70, -10))),
        (0.16, P(pitch=70, fwd=0.05, height=1.10, neck=-6, head=-2, arm=arms(150, 18, 0, 15, 0), leg=legs(-2, 8, 0, 30, -30))),
        # the pike: the trunk swings down past vertical while the hips stay at the surface, legs still trailing
        (0.34, P(pitch=150, fwd=-0.12, height=1.22, neck=12, head=6, spine=14, arm=arms(170, 15, 0, 10, 0), leg=legs(95, 6, 0, 15, -35))),
        # the pull back to the hips; the legs come up
        (0.50, P(pitch=168, fwd=-0.10, height=1.18, neck=10, head=4, spine=8, arm=arms(25, 40, -25, 55, 15), leg=legs(45, 4, 0, 8, -45))),
        # straight, head-down, legs overhead; their weight takes him under
        (0.66, P(pitch=178, fwd=-0.05, height=1.05, neck=4, head=0, arm=arms(165, 0, 0, 12, 0), leg=legs(0, 2, 0, 4, -45))),
        (0.84, P(pitch=128, fwd=0.15, height=1.07, neck=-4, head=-2, arm=arms(170, -3, 0, 8, -5), leg=legs(-4, 2, 0, 4, -45))),
        (1.00, GLIDE),
    ]


def surface_keys():
    return [
        (0.00, GLIDE),
        (0.25, P(pitch=62, fwd=0.2, height=1.12, neck=-32, head=-14, spine=-8, arm=arms(140, 58, -25, 25, 10), leg=legs(46, 26, 20, 100, 15))),
        (0.50, P(pitch=32, fwd=0.08, height=1.00, neck=-14, head=-6, arm=arms(35, 50, -10, 70, 10), leg=legs(18, 28, 0, 40, -20))),
        (0.72, P(pitch=16, fwd=0.02, height=0.97, neck=-2, head=0, arm=arms(45, 40, 10, 70, 0), leg=legs(40, 22, 10, 80, -5))),
    ]


# ---- curves -------------------------------------------------------------------------------------------------------
def _fill(keys):
    """Every key gets every channel: a channel a key leaves out keeps the previous key's value."""
    cur = dict(DEFAULT)
    out = []
    for u, p in keys:
        cur = dict(cur); cur.update(p)
        out.append((u, cur))
    return out


def _catmull(p0, p1, p2, p3, t):
    t2, t3 = t * t, t * t * t
    return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3)


def channel(keys, ch, u, cyclic):
    us = [k[0] for k in keys]
    vs = [k[1][ch] for k in keys]
    n = len(keys)
    if cyclic:
        u %= 1.0
        i = max(j for j in range(n) if us[j] <= u)
        u0, u1 = us[i], (us[i + 1] if i + 1 < n else 1.0)
        get = lambda j: vs[j % n]
    else:
        u = min(max(u, 0.0), 1.0)
        i = min(max(j for j in range(n) if us[j] <= u), n - 2)
        u0, u1 = us[i], us[i + 1]
        get = lambda j: vs[min(max(j, 0), n - 1)]
    t = 0.0 if u1 <= u0 else (u - u0) / (u1 - u0)
    return _catmull(get(i - 1), get(i), get(i + 1), get(i + 2), t)


def sample(keys, u, cyclic):
    return {ch: channel(keys, ch, u - LAG.get(ch, 0.0), cyclic) for ch in DEFAULT}


# ---- channels -> bones --------------------------------------------------------------------------------------------
def R(x=0.0, y=0.0, z=0.0):
    return (math.radians(x), math.radians(y), math.radians(z))


def bones(c):
    """{bone: (location, euler)} for a set of channels."""
    b = {'pzadek': ((0.0, c['height'], c['fwd']), R(c['pitch'], c['yaw'], c['roll']))}
    for name, share in zip(SPINE, (0.4, 0.3, 0.3)):
        b[name] = ((0, 0, 0), R(c['spine'] * share))
    b['krk'] = ((0, 0, 0), R(c['neck']))
    b['hlava'] = ((0, 0, 0), R(c['head']))
    for s in 'pl':
        m = 1 if s == 'l' else -1
        flex, abd = c[s + 'flex'], c[s + 'abd']
        lift = max(0.0, flex - 90) * 0.3 + max(0.0, abd - 40) * 0.25  # the collarbone rises with an overhead reach
        b[s + 'rameno'] = ((0, 0, 0), R(min(flex, 120) * 0.12, 0, -m * lift))
        b[s + 'biceps'] = ((0, 0, 0), R(flex - min(flex, 120) * 0.12, m * c[s + 'twist'], m * abd))
        b[s + 'loket'] = ((0, 0, 0), R(max(c[s + 'elbow'], 2.0)))  # the curves must not bend an elbow backwards
        b[s + 'ruka'] = ((0, 0, 0), R(c[s + 'wrist']))
        b[s + 'stehno'] = ((0, 0, 0), R(c[s + 'hip'], m * c[s + 'hiptwist'], m * c[s + 'hipabd']))
        b[s + 'holen'] = ((0, 0, 0), R(-max(c[s + 'knee'], 2.0)))
        b[s + 'chodidlo'] = ((0, 0, 0), R(c[s + 'ankle']))
        b[s + 'prsty'] = ((0, 0, 0), R())
    return b


def _ease(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def _mix(a, b, f):
    return tuple(x * (1 - f) + y * f for x, y in zip(a, b))


# ---- keying -------------------------------------------------------------------------------------------------------
def _rig():
    return bpy.data.objects['MC_Rig']


def tread_pose(frame=0):
    """The tread action's pose at <frame> as {bone: (location, rotation)}."""
    ob = _rig()
    ob.animation_data.action = bpy.data.actions['tread']
    bpy.context.scene.frame_set(frame)
    return {b: (tuple(ob.pose.bones[b].location), tuple(ob.pose.bones[b].rotation_euler)) for b in ALL + list(SLING)}


def bake(name, keys, seconds, cyclic, join_in=None, join_out=None):
    """Key <name> on every frame. join_in=(pose, u0, u1): blend from <pose> ({bone: (location, rotation)}) over u0..u1;
    join_out the same into it -- so a transition starts and ends exactly on the moves it joins."""
    ob = _rig()
    tread = tread_pose(0)
    keys = _fill(keys)
    act = bpy.data.actions.get(name)
    if act:
        bpy.data.actions.remove(act)
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data.action = act
    last = int(round(seconds * FPS))
    for f in range(last + 1):
        u = f / last
        pose = bones(sample(keys, u, cyclic))
        blends = []
        if join_in:
            blends.append((join_in[0], 1.0 - _ease((u - join_in[1]) / (join_in[2] - join_in[1]))))
        if join_out:
            blends.append((join_out[0], _ease((u - join_out[1]) / (join_out[2] - join_out[1]))))
        for b in ALL + list(SLING):
            pb = ob.pose.bones[b]
            loc, rot = pose.get(b, tread[b])
            for target, w in blends:
                if w > 0 and b in target:
                    loc, rot = _mix(loc, target[b][0], w), _mix(rot, target[b][1], w)
            pb.location, pb.rotation_euler = loc, rot
            pb.keyframe_insert('location', frame=f)
            pb.keyframe_insert('rotation_euler', frame=f)
    for fc in _fcurves(act):
        if cyclic:
            fc.modifiers.new('CYCLES')
        fc.update()
    act.frame_range = (0, last)
    return act


def _fcurves(act):
    if hasattr(act, 'fcurves') and len(act.fcurves):
        return list(act.fcurves)
    out = []  # Blender 4.4+ layered actions
    for layer in getattr(act, 'layers', []):
        for strip in layer.strips:
            for bag in strip.channelbags:
                out.extend(bag.fcurves)
    return out


def make_all():
    acts = {}
    acts['swimunder'] = bake('swimunder', under_keys(), LOOPED['swimunder'][0], True)
    acts['swimunderidle'] = bake('swimunderidle', idle_keys(), LOOPED['swimunderidle'][0], True)
    tread = tread_pose(0)
    stroke = bones(sample(_fill(under_keys()), 0.0, True))  # the stroke's first pose, its trailing limbs included
    acts['swimdive'] = bake('swimdive', dive_keys(), ONESHOT['swimdive'][0], False,
                            join_in=(tread, 0.0, 0.16), join_out=(stroke, 0.85, 1.0))
    acts['swimsurface'] = bake('swimsurface', surface_keys(), ONESHOT['swimsurface'][0], False,
                               join_in=(stroke, 0.0, 0.1), join_out=(tread, 0.6, 1.0))
    return acts


def export_all(out_dir):
    import os
    import runpy
    rig = runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blender_swim_rig.py'))
    done = {}
    for name, (_, phases, step) in {**LOOPED, **ONESHOT}.items():
        done[name] = rig['export_rtm'](name, os.path.join(out_dir, name + '.rtm'), phases=phases, step=step)
    return done
