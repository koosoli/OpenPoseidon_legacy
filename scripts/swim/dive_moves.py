# Sinkhole W4 (diving): the underwater moves, keyed as Blender actions on the MC rig (blender_swim_rig.py) and
# exported as RTMs. Made by Claude in Blender, 2026-10-17 (owner: "make with blender ... also a diving animation").
# Run inside Blender after rig['build'](...) and rig['import_rtm'](<tread.rtm>) (the dive and the surfacing join
# treading exactly, so the tread action must be loaded):
#
#   dm = runpy.run_path(r'<repo>\scripts\swim\dive_moves.py')
#   dm['make_all']()                              # (re)keys the four actions
#   dm['export_all'](r'<repo>\content\default-packs\swim\rtm')
#
# The actions stay ordinary keyframes: they can be reworked by hand in Blender and exported again with export_all.
#
# The technique: a loaded soldier without fins swims under water with the underwater breaststroke (the long pull to
# the thighs and a glide, the frog kick on the recovery), body flat, head in line, the rifle slung on the chest.
#   swimdive       duck-dive from treading water: reach forward, pike head-down at the hips, the arms pull back and
#                  the legs lift over, ending in the glide (one-shot, 1.5 s)
#   swimunder      the underwater stroke, 2.2 s a cycle, 1.6 m (looped)
#   swimunderidle  holding still under water: flat, the hands sculling under the chest, a slow easy kick (looped)
#   swimsurface    up from the glide: the head and chest lift, a big pull down and a kick, into treading (one-shot)
#
# Angles are degrees on the rig's own axes (see blender_swim_rig.py): flex is about local X (+ forward for the
# spine, arms, thighs, forearms; the knee bends with -), abd swings a limb out to its side, twist turns it about
# its length; the left side's abd and twist are mirrored here so both sides read the same.
import math

import bpy

FPS = 24
LOOPED = {'swimunder': (2.2, 41, (0.0, 0.0, -1.6)), 'swimunderidle': (2.4, 41, (0.0, 0.0, 0.0))}
ONESHOT = {'swimdive': (1.5, 31, (0.0, 0.0, -0.4)), 'swimsurface': (1.3, 27, (0.0, 0.0, -0.2))}
SPINE = ('bricho', 'zebra', 'hrudnik')
LIMB_BONES = [s + b for s in 'pl' for b in ('rameno', 'biceps', 'loket', 'ruka', 'stehno', 'holen', 'chodidlo', 'prsty')]
ALL = ['pzadek', 'krk', 'hlava', *SPINE, *LIMB_BONES]


def R(x=0.0, y=0.0, z=0.0):
    return (math.radians(x), math.radians(y), math.radians(z))


def body(pitch=90.0, height=1.15, fwd=-0.3, roll=0.0, spine=0.0, neck=0.0, head=0.0):
    """The root and the trunk. pitch: 0 upright, 90 lying flat face down, more is head-down. height and fwd: where
    the pelvis is (world Z, and how far behind the treading pelvis it sits, + backwards)."""
    p = {'pzadek': ((0.0, height - 0.0, -fwd), R(pitch, 0, roll))}
    for b, share in zip(SPINE, (0.4, 0.3, 0.3)):
        p[b] = ((0, 0, 0), R(spine * share))
    p['krk'] = ((0, 0, 0), R(neck))
    p['hlava'] = ((0, 0, 0), R(head))
    return p


def arm(p, s, flex, abd=0.0, twist=0.0, elbow=0.0, wrist=0.0):
    """Shoulder flex (0 hanging, 90 straight forward, 180 overhead along the body's line), abd out to the side."""
    m = 1 if s == 'l' else -1
    lift = max(0.0, flex - 90) * 0.3 + max(0.0, abd - 40) * 0.25  # the collarbone rises with an overhead reach
    p[s + 'rameno'] = ((0, 0, 0), R(min(flex, 120) * 0.12, 0, -m * lift))
    p[s + 'biceps'] = ((0, 0, 0), R(flex - min(flex, 120) * 0.12, m * twist, m * abd))
    p[s + 'loket'] = ((0, 0, 0), R(elbow))
    p[s + 'ruka'] = ((0, 0, 0), R(wrist))


def leg(p, s, hip, abd=0.0, twist=0.0, knee=0.0, ankle=0.0):
    """Hip flex (+ thigh forward), knee bend (positive here, applied as -X), ankle (- points the toes)."""
    m = 1 if s == 'l' else -1
    p[s + 'stehno'] = ((0, 0, 0), R(hip, m * twist, m * abd))
    p[s + 'holen'] = ((0, 0, 0), R(-knee))
    p[s + 'chodidlo'] = ((0, 0, 0), R(ankle))
    p[s + 'prsty'] = ((0, 0, 0), R(0))


def both_arms(p, **kw):
    arm(p, 'p', **kw); arm(p, 'l', **kw)


def both_legs(p, **kw):
    leg(p, 'p', **kw); leg(p, 'l', **kw)


# ---- key poses ---------------------------------------------------------------------------------------------------
def glide():
    """Streamlined: flat, arms reaching ahead, legs long together, toes pointed."""
    p = body(pitch=90, neck=-8, head=-4)
    both_arms(p, flex=172, abd=6, elbow=6, wrist=-5)
    both_legs(p, hip=-4, abd=2, knee=4, ankle=-45)
    return p


def under_keys():
    """One cycle of the underwater breaststroke: (u, pose)."""
    k = [(0.0, glide())]
    p = body(pitch=92, neck=-10, head=-4)                      # outsweep: hands part, palms out
    both_arms(p, flex=160, abd=40, twist=-25, elbow=12, wrist=10)
    both_legs(p, hip=-4, abd=2, knee=5, ankle=-45)
    k.append((0.14, p))
    p = body(pitch=93, neck=-12, head=-6, spine=4)              # catch and insweep: elbows high, hands under the chest
    both_arms(p, flex=95, abd=55, twist=-35, elbow=95, wrist=15)
    both_legs(p, hip=-2, abd=3, knee=8, ankle=-45)
    k.append((0.30, p))
    p = body(pitch=91, neck=-6, head=-2)                        # push: hands past the hips to the thighs
    both_arms(p, flex=-15, abd=12, twist=10, elbow=8, wrist=-20)
    both_legs(p, hip=-3, abd=2, knee=6, ankle=-45)
    k.append((0.46, p))
    p = body(pitch=90, neck=-6, head=-2)                        # glide with the arms at the sides
    both_arms(p, flex=-12, abd=10, twist=10, elbow=10, wrist=-15)
    both_legs(p, hip=-4, abd=2, knee=4, ankle=-45)
    k.append((0.58, p))
    p = body(pitch=86, neck=-10, head=-4, spine=6)              # recovery: hands slide up under the body, knees draw up
    both_arms(p, flex=55, abd=10, twist=-10, elbow=135, wrist=10)
    both_legs(p, hip=48, abd=28, twist=25, knee=115, ankle=20)
    k.append((0.72, p))
    p = body(pitch=92, neck=-8, head=-4)                        # kick: the hands shoot forward, the feet push back
    both_arms(p, flex=150, abd=8, elbow=35, wrist=0)
    both_legs(p, hip=14, abd=32, twist=10, knee=35, ankle=-20)
    k.append((0.84, p))
    k.append((1.0, glide()))
    return k


def idle_keys():
    """Holding position under water: flat with the head up a little, sculling, a slow alternating kick."""
    k = []
    for i in range(5):
        u = i / 4
        p = body(pitch=80 + 3 * math.sin(2 * math.pi * u), neck=-22, head=-10, spine=-4)
        w = 1 if i % 2 == 0 else -1                             # the hands sweep out and in, twice a cycle
        # the hands ahead of the face, forearms level, sweeping out and in
        both_arms(p, flex=125, abd=32 + 16 * w, twist=-22 * w, elbow=65 - 12 * w, wrist=8 * w)
        for s, ph in (('p', 0.0), ('l', 0.5)):
            c = math.cos(2 * math.pi * (u + ph))
            leg(p, s, hip=8 + 8 * c, abd=8, twist=6, knee=28 + 18 * c, ankle=-38 + 8 * c)
        k.append((u, p))
    return k


def dive_keys(tread0):
    k = [(0.0, tread0)]
    p = body(pitch=35, height=1.05, fwd=0.0, neck=12, head=8, spine=10)        # reach forward, take a breath
    both_arms(p, flex=125, abd=18, elbow=20)
    both_legs(p, hip=30, abd=14, knee=55, ankle=-10)
    k.append((0.2, p))
    p = body(pitch=118, height=1.25, fwd=0.05, neck=28, head=14, spine=22)    # pike: head down, the hips over
    both_arms(p, flex=150, abd=25, twist=-20, elbow=25)
    both_legs(p, hip=95, abd=6, knee=20, ankle=-35)
    k.append((0.42, p))
    p = body(pitch=128, height=1.3, fwd=0.2, neck=18, head=8, spine=14)       # the arms pull back, the legs rise
    both_arms(p, flex=40, abd=45, twist=-30, elbow=60, wrist=15)
    both_legs(p, hip=45, abd=4, knee=12, ankle=-45)
    k.append((0.62, p))
    p = body(pitch=112, height=1.22, fwd=0.28, neck=4, head=0)                # straight, head-down, arms forward again
    both_arms(p, flex=155, abd=12, elbow=20)
    both_legs(p, hip=4, abd=2, knee=6, ankle=-45)
    k.append((0.82, p))
    k.append((1.0, glide()))
    return k


def surface_keys(tread0):
    k = [(0.0, glide())]
    p = body(pitch=62, height=1.15, fwd=0.15, neck=-30, head=-15, spine=-8)   # head and chest lift, arms wide
    both_arms(p, flex=150, abd=55, twist=-25, elbow=20, wrist=10)
    both_legs(p, hip=40, abd=25, twist=20, knee=100, ankle=15)
    k.append((0.3, p))
    p = body(pitch=30, height=1.0, fwd=0.05, neck=-12, head=-6)               # the pull down and the kick
    both_arms(p, flex=35, abd=50, twist=-10, elbow=70, wrist=10)
    both_legs(p, hip=15, abd=30, knee=40, ankle=-20)
    k.append((0.6, p))
    k.append((1.0, tread0))
    return k


# ---- keying ------------------------------------------------------------------------------------------------------
def _rig():
    return bpy.data.objects['MC_Rig']


def tread_pose(frame=0):
    """The tread action's pose at <frame> as {bone: (location, rotation)} (the dive starts there, surfacing ends)."""
    ob = _rig()
    ob.animation_data.action = bpy.data.actions['tread']
    bpy.context.scene.frame_set(frame)
    return {b: (tuple(ob.pose.bones[b].location), tuple(ob.pose.bones[b].rotation_euler)) for b in ALL}


def _sling():
    ob = _rig()
    ob.animation_data.action = bpy.data.actions['tread']
    bpy.context.scene.frame_set(0)
    return {b: (tuple(ob.pose.bones[b].location), tuple(ob.pose.bones[b].rotation_euler)) for b in ('zbran', 'roura', 'lzadek')}


def key_action(name, keys, seconds, cyclic):
    ob = _rig()
    act = bpy.data.actions.get(name)
    if act:
        bpy.data.actions.remove(act)
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data_create()
    ob.animation_data.action = act
    sling = _sling()
    ob.animation_data.action = act
    last = seconds * FPS
    for u, pose in keys:
        f = round(u * last)
        full = dict(sling)
        full.update(pose)
        for b in ALL + ['zbran', 'roura', 'lzadek']:
            pb = ob.pose.bones[b]
            loc, rot = full.get(b, ((0, 0, 0), (0, 0, 0)))
            pb.location = loc
            pb.rotation_euler = rot
            pb.keyframe_insert('location', frame=f)
            pb.keyframe_insert('rotation_euler', frame=f)
    for fc in _fcurves(act):
        for kp in fc.keyframe_points:
            kp.interpolation = 'BEZIER'
            kp.handle_left_type = kp.handle_right_type = 'AUTO_CLAMPED'
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
    tread0 = tread_pose(0)
    acts = {}
    acts['swimunder'] = key_action('swimunder', under_keys(), LOOPED['swimunder'][0], True)
    acts['swimunderidle'] = key_action('swimunderidle', idle_keys(), LOOPED['swimunderidle'][0], True)
    acts['swimdive'] = key_action('swimdive', dive_keys(tread0), ONESHOT['swimdive'][0], False)
    acts['swimsurface'] = key_action('swimsurface', surface_keys(tread0), ONESHOT['swimsurface'][0], False)
    return acts


def export_all(out_dir):
    import os
    import runpy
    rig = runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blender_swim_rig.py'))
    done = {}
    for name, (_, phases, step) in {**LOOPED, **ONESHOT}.items():
        done[name] = rig['export_rtm'](name, os.path.join(out_dir, name + '.rtm'), phases=phases, step=step)
    return done
