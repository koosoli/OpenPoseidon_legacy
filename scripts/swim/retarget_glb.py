# Retarget a humanoid clip from a glTF/GLB file (UE-mannequin bone names: pelvis, spine_01..03, neck_01, head,
# clavicle/upperarm/lowerarm/hand, thigh/calf/foot/ball, _l/_r -- Quaternius' Universal Animation Library, ActionForge,
# most UE-style rigs) onto the MC soldier rig (blender_swim_rig.py / the add-on's rig.py), as an ordinary action that
# rig.export_rtm writes as an RTM. Run inside Blender, after the rig is built:
#
#   rt = runpy.run_path(r'<repo>\scripts\swim\retarget_glb.py')
#   src, clip = rt['import_glb'](r'<dir>\swim_idle_loop.glb')
#   act = rt['retarget'](src, clip, 'tread_af', keep_from='tread', match_height_of='tread')
#
# How. Every mapped bone has an aim: the line from its joint to the joint of the next mapped bone down the chain
# (the leaf bones: their own head-to-tail line). The rigs rest differently (the clip's T-pose, the soldier's arms
# down), so each bone gets a constant offset K = Rs^-1 . Q^-1 . Rt, where Q is the smallest turn taking the clip's rest
# aim onto the soldier's rest aim; on every frame the soldier's bone is the clip's bone times K. That puts every
# soldier bone along the clip's bone exactly, with the twist the two rest poses imply. Only the pelvis moves: the
# clip's pelvis relative to its root bone (so root motion, the travel, is dropped -- the game moves the man by the
# move's step), scaled by the two leg lengths, and shifted so its mean height over the cycle matches a reference move
# (the waterline the engine's heave holds). Bones the clip has no counterpart for (the rifle and launcher, zbran and
# roura, and lzadek) keep the reference move's first pose: the sling stays where the swim moves put it.
import math

import bpy
from mathutils import Matrix, Vector

SIDE = {'l': 'l', 'r': 'p'}  # MC: l = levy (left), p = pravy (right)
MAP = {'pelvis': 'pzadek', 'spine_01': 'bricho', 'spine_02': 'zebra', 'spine_03': 'hrudnik', 'neck_01': 'krk',
       'head': 'hlava'}
for _s, _m in SIDE.items():
    MAP.update({f'clavicle_{_s}': _m + 'rameno', f'upperarm_{_s}': _m + 'biceps', f'lowerarm_{_s}': _m + 'loket',
                f'hand_{_s}': _m + 'ruka', f'thigh_{_s}': _m + 'stehno', f'calf_{_s}': _m + 'holen',
                f'foot_{_s}': _m + 'chodidlo', f'ball_{_s}': _m + 'prsty'})
# the joint each MC bone aims at (None: its own tail)
AIM_T = {'pzadek': 'bricho', 'bricho': 'zebra', 'zebra': 'hrudnik', 'hrudnik': 'krk', 'krk': 'hlava', 'hlava': None}
for _m in 'pl':
    AIM_T.update({_m + 'rameno': _m + 'biceps', _m + 'biceps': _m + 'loket', _m + 'loket': _m + 'ruka', _m + 'ruka': None,
                  _m + 'stehno': _m + 'holen', _m + 'holen': _m + 'chodidlo', _m + 'chodidlo': _m + 'prsty',
                  _m + 'prsty': None})
FPS = 24


def _rig():
    return bpy.data.objects['MC_Rig']


def import_glb(path):
    """Import a GLB; returns (armature object, its first action)."""
    before = set(bpy.data.objects)
    acts = set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == 'ARMATURE')
    for o in new:
        o.hide_render = True
        o.hide_set(True)
    act = next(a for a in bpy.data.actions if a not in acts)
    return arm, act


def _src_names(arm):
    lower = {b.name.lower(): b.name for b in arm.data.bones}
    return {lower[k]: v for k, v in MAP.items() if k in lower}


def _aim_src(src_map):
    """The clip's aims, by its own bone names: the joint of the clip bone mapped to the soldier bone's aim joint."""
    inv = {v: k for k, v in src_map.items()}
    return {s: (inv.get(AIM_T[t]) if AIM_T[t] else None) for s, t in src_map.items()}


def _world(ob, posed):
    """{bone: (head, tail, 3x3 rotation)} in world space, rest or posed."""
    out = {}
    for b in (ob.pose.bones if posed else ob.data.bones):
        M = ob.matrix_world @ (b.matrix if posed else b.matrix_local)
        bone = b.bone if posed else b
        tail = ob.matrix_world @ (b.tail if posed else bone.tail_local)
        out[b.name] = (M.translation.copy(), tail.copy(), M.to_3x3().normalized())
    return out


def _aims(world, aim_of):
    return {b: ((world[aim_of[b]][0] if aim_of[b] else world[b][1]) - world[b][0]).normalized() for b in aim_of}


def retarget(src, clip, name, seconds=None, keep_from='tread', match_height_of='tread', loop=True, clamp=True):
    """Key <name> on the MC rig from <clip> playing on <src>, one key per 1/24 s. Returns the action. clamp: hold
    every joint to the rig's limits except the upper arms (swimming takes them overhead, past the stock range)."""
    ob = _rig()
    sc = bpy.context.scene
    src_map = _src_names(src)
    aim_s_of = _aim_src(src_map)
    aim_t_of = {t: AIM_T[t] for t in src_map.values()}
    # rest frames and the constant offsets
    src.animation_data.action = None
    for pb in src.pose.bones:
        pb.matrix_basis = Matrix()
    bpy.context.view_layer.update()
    ws, wt = _world(src, False), _world(ob, False)
    aim_s, aim_t = _aims(ws, aim_s_of), _aims(wt, aim_t_of)
    K = {}
    for s, t in src_map.items():
        Q = aim_s[s].rotation_difference(aim_t[t]).to_matrix()
        K[t] = ws[s][2].inverted() @ Q.inverted() @ wt[t][2]
    inv_map = {v: k for k, v in src_map.items()}
    leg_s = (ws[inv_map['pstehno']][0] - ws[inv_map['pchodidlo']][0]).length
    leg_t = (wt['pstehno'][0] - wt['pchodidlo'][0]).length
    scale = leg_t / leg_s
    root_s = next((b.name for b in src.data.bones if b.parent is None), None)
    pelvis_s = next(k for k, v in src_map.items() if v == 'pzadek')
    rest_rel = ws[pelvis_s][0] - ws[root_s][0]
    # the reference move's first pose for the unmapped bones
    keep = {}
    if keep_from:
        ob.animation_data.action = bpy.data.actions[keep_from]
        sc.frame_set(0)
        keep = {pb.name: pb.matrix_basis.copy() for pb in ob.pose.bones if pb.name not in src_map.values()}
    # sample the clip
    src.animation_data.action = clip
    f0, f1 = clip.frame_range
    seconds = seconds or (f1 - f0) / FPS
    n = int(round(seconds * FPS))
    frames = []
    for i in range(n + 1):
        f = f0 + (f1 - f0) * i / n
        sc.frame_set(int(f), subframe=f - int(f))
        w = _world(src, True)
        rel = (w[pelvis_s][0] - w[root_s][0]) - rest_rel
        frames.append(({t: w[s][2] for s, t in src_map.items()}, wt['pzadek'][0] + rel * scale))
    if loop:
        frames[-1] = frames[0]
    # waterline: the pelvis' mean height over the cycle matches the reference move's
    if match_height_of:
        ob.animation_data.action = bpy.data.actions[match_height_of]
        ref = bpy.data.actions[match_height_of]
        r0, r1 = ref.frame_range
        hs = []
        for i in range(21):
            sc.frame_set(int(r0 + (r1 - r0) * i / 20))
            hs.append((ob.matrix_world @ ob.pose.bones['pzadek'].head).z)
        dz = sum(hs) / len(hs) - sum(p.z for _, p in frames) / len(frames)
        frames = [(r, p + Vector((0, 0, dz))) for r, p in frames]
    # key the rig
    act = bpy.data.actions.get(name)
    if act:
        bpy.data.actions.remove(act)
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data.action = act
    from io_import_p3d import rig as _r
    _r.set_joint_limits(ob, False)
    inv_world = ob.matrix_world.inverted()
    order = [b for b in ob.data.bones]  # parents come before children in Blender's bone list
    prev = {}
    for k, (rots, pelvis) in enumerate(frames):
        posed = {}
        for bone in order:
            b = bone.name
            pb = ob.pose.bones[b]
            if bone.parent:
                rest_rel_b = bone.parent.matrix_local.inverted() @ bone.matrix_local
                chain = posed[bone.parent.name] @ rest_rel_b
            else:
                chain = bone.matrix_local.copy()
            if b in rots:
                R = (inv_world.to_3x3() @ rots[b] @ K[b])
                M = R.to_4x4()
                M.translation = (inv_world @ pelvis) if b == 'pzadek' else chain.translation
                basis = chain.inverted() @ M
                lim = _r.LIMITS.get(b) if clamp and not b.endswith('biceps') else None
                if lim:
                    # held to the joint's limit; the bones below still aim along the clip's (they are set in world
                    # space), so a clamped collarbone or neck costs the arm or head nothing
                    loc0, rot0, _ = basis.decompose()
                    e = rot0.to_euler(pb.rotation_mode, prev[b]) if b in prev else rot0.to_euler(pb.rotation_mode)
                    for i, (lo, hi) in enumerate(lim):
                        e[i] = min(max(e[i], math.radians(lo)), math.radians(hi))
                    basis = Matrix.Translation(loc0) @ e.to_matrix().to_4x4()
                    M = chain @ basis
            else:
                basis = keep.get(b, Matrix())
                M = chain @ basis
            posed[b] = M
            loc, rot, _ = basis.decompose()
            eul = rot.to_euler(pb.rotation_mode, prev[b]) if b in prev else rot.to_euler(pb.rotation_mode)
            prev[b] = eul
            pb.location = (0.0, 0.0, 0.0) if b in rots and b != 'pzadek' else loc
            pb.rotation_euler = eul
            pb.keyframe_insert('location', frame=k)
            pb.keyframe_insert('rotation_euler', frame=k)
    act.frame_range = (0, n)
    act['source'] = clip.name
    return act
