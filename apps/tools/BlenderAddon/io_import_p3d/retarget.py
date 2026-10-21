"""Retarget a humanoid clip (glTF/GLB) onto the MC soldier rig (rig.py), as an ordinary action that rig.export_rtm
writes as an RTM.

Sources: UE-mannequin bone names (pelvis, spine_01..03, neck_01, head, clavicle/upperarm/lowerarm/hand,
thigh/calf/foot/ball, _l/_r): Quaternius' Universal Animation Library, ActionForge (actionforge.app), most UE-style
rigs. Check a clip's licence before it goes into the repo: CC0 (Quaternius, ActionForge) and the CMU motion capture
database are fine for this GPL project; Mixamo's terms forbid redistributing the motion, so not that; a generative
model's licence is per model (Kimodo's SMPL-X variant is research-only). Never take motion from another game.

    src, clip = retarget.import_glb(path_to_glb, "Jump_Loop")
    act = retarget.retarget(src, clip, "fall_ual", keep_from="fall", match_height_of=None,
                            keep_bones=retarget.ARMS_AND_RIFLE)

How. Every mapped bone has an aim: the line from its joint to the joint of the next mapped bone down the chain (the
leaf bones: their own head-to-tail line). The rigs rest differently (a T-pose against the soldier's arms down), so each
bone gets a constant offset K = Rs^-1 . Q^-1 . Rt, where Q is the smallest turn taking the clip's rest aim onto the
soldier's rest aim; on every frame the soldier's bone is the clip's bone times K. That puts every soldier bone along
the clip's bone, with the twist the two rest poses imply. Only the pelvis moves: the clip's pelvis relative to its root
bone (root motion, the travel, is dropped -- the game moves the man by the move's step), scaled by the two leg lengths,
and stood on the game's ground: the clip's root is its floor, and the soldier's floor is z = 0 of the rig (a stock
move's lowest foot point stands at ~0.02 there), so the soldier's feet are where the clip's are above its floor. The
rig's rest pose is not that: its feet hang ~1.06 m below the floor, and a pelvis left at its rest height sinks the man
to the waist in the game (the first jump moves, 2026-10-20). Optionally the result is instead shifted so its mean
pelvis height matches a reference move (a swimmer's waterline). Bones the clip has no
counterpart for (zbran, roura, lzadek) keep the reference move's first pose; keep_bones follow the reference move
through its cycle instead (an unarmed clip's legs and trunk under a rifle-holding move's arms and rifle). Every joint
is held to the rig's limits except the upper arms (clamp=False turns that off); the bones below a clamped one still aim along the
clip's, so a clamped collarbone or neck costs the arm or the head nothing.
"""

import math

import bpy
from mathutils import Matrix, Vector

from . import rig as _r

SIDE = {"l": "l", "r": "p"}  # MC: l = levy (left), p = pravy (right)
MAP = {"pelvis": "pzadek", "spine_01": "bricho", "spine_02": "zebra", "spine_03": "hrudnik", "neck_01": "krk",
       "head": "hlava"}
for _s, _m in SIDE.items():
    MAP.update({f"clavicle_{_s}": _m + "rameno", f"upperarm_{_s}": _m + "biceps", f"lowerarm_{_s}": _m + "loket",
                f"hand_{_s}": _m + "ruka", f"thigh_{_s}": _m + "stehno", f"calf_{_s}": _m + "holen",
                f"foot_{_s}": _m + "chodidlo", f"ball_{_s}": _m + "prsty"})
# the joint each MC bone aims at (None: its own tail)
AIM_T = {"pzadek": "bricho", "bricho": "zebra", "zebra": "hrudnik", "hrudnik": "krk", "krk": "hlava", "hlava": None}
for _m in "pl":
    AIM_T.update({_m + "rameno": _m + "biceps", _m + "biceps": _m + "loket", _m + "loket": _m + "ruka",
                  _m + "ruka": None, _m + "stehno": _m + "holen", _m + "holen": _m + "chodidlo",
                  _m + "chodidlo": _m + "prsty", _m + "prsty": None})
FPS = 24
# keep_bones for a move that holds the rifle: the collarbones (and so the whole arms) and the slung weapons
ARMS_AND_RIFLE = ("prameno", "lrameno", "zbran", "roura")


def _rig():
    return bpy.data.objects[_r.ARM]


def import_glb(path, clip=None):
    """Import a GLB/glTF (every clip becomes an action), hidden. Returns (armature, the action named <clip>, else the
    first new one)."""
    before = set(bpy.data.objects)
    acts = set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == "ARMATURE")
    for o in new:
        o.hide_render = True
        o.hide_set(True)
    new_acts = [a for a in bpy.data.actions if a not in acts]
    act = next((a for a in new_acts if a.name == clip), None) if clip else None
    return arm, act or (new_acts[0] if new_acts else None)


def source_bones(arm):
    """{clip bone: MC bone} for the bones of <arm> the map knows (case-insensitive)."""
    lower = {b.name.lower(): b.name for b in arm.data.bones}
    return {lower[k]: v for k, v in MAP.items() if k in lower}


def _aim_src(src_map):
    """The clip's aims, by its own bone names: the joint of the clip bone mapped to the soldier bone's aim joint."""
    inv = {v: k for k, v in src_map.items()}
    return {s: (inv.get(AIM_T[t]) if AIM_T[t] else None) for s, t in src_map.items()}


def _world(ob, posed):
    """{bone: (head, tail, 3x3 rotation)} in world space, rest or posed."""
    out = {}
    for b in ob.pose.bones if posed else ob.data.bones:
        M = ob.matrix_world @ (b.matrix if posed else b.matrix_local)
        bone = b.bone if posed else b
        tail = ob.matrix_world @ (b.tail if posed else bone.tail_local)
        out[b.name] = (M.translation.copy(), tail.copy(), M.to_3x3().normalized())
    return out


def _aims(world, aim_of):
    return {b: ((world[aim_of[b]][0] if aim_of[b] else world[b][1]) - world[b][0]).normalized() for b in aim_of}


FOOT_BONES = ("pchodidlo", "lchodidlo", "pprsty", "lprsty")


def _lowest(world, bones):
    """The lowest joint or tail of <bones> in <world> (_world's dict)."""
    return min(min(world[b][0].z, world[b][1].z) for b in bones)


def _sample(ob, action, u):
    """Set <ob> to <action> at the fraction u of its frame range."""
    sc = bpy.context.scene
    ob.animation_data_create()
    ob.animation_data.action = action
    r0, r1 = action.frame_range
    f = r0 + (r1 - r0) * u
    sc.frame_set(int(math.floor(f)), subframe=f - math.floor(f))


def retarget(src, clip, name, seconds=None, keep_from=None, match_height_of=None, loop=True, clamp=True,
             keep_bones=(), clip_range=(0.0, 1.0)):
    """Key the action <name> on the MC rig from <clip> playing on the armature <src>, one key per 1/24 s (seconds:
    the clip's own length unless given). keep_from: the move whose pose the unmapped bones take (and keep_bones follow);
    match_height_of: the move whose mean pelvis height the result takes; loop: make the last key the first; clip_range:
    the part of the clip to use, as fractions of its length (the result spans <seconds> either way). Returns the
    action; its custom properties "source" and "seconds" record the clip and the cycle time."""
    ob = _rig()
    sc = bpy.context.scene
    src_map = source_bones(src)
    missing = sorted(set(MAP.values()) - set(src_map.values()))
    if "pzadek" in missing or "pstehno" in missing or "pchodidlo" in missing:
        raise ValueError(f"{src.name} has no pelvis/leg bones the map knows (missing {missing})")
    aim_s_of = _aim_src(src_map)
    aim_t_of = {t: AIM_T[t] for t in src_map.values()}
    # rest frames and the constant offsets
    src.animation_data_create()
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
    leg_t = (wt["pstehno"][0] - wt["pchodidlo"][0]).length
    scale = leg_t / (ws[inv_map["pstehno"]][0] - ws[inv_map["pchodidlo"]][0]).length
    root_s = next((b.name for b in src.data.bones if b.parent is None), None)
    pelvis_s = inv_map["pzadek"]
    rest_rel = ws[pelvis_s][0] - ws[root_s][0]
    # stand on the floor: the soldier's rest feet go to where the clip's rest feet are above its root, scaled
    floor = Vector((0, 0, (_lowest(ws, [inv_map[b] for b in FOOT_BONES if b in inv_map]) - ws[root_s][0].z) * scale
                          - _lowest(wt, FOOT_BONES)))
    # the reference move's first pose for the unmapped bones
    keep = {}
    if keep_from:
        _sample(ob, bpy.data.actions[keep_from], 0.0)
        keep = {pb.name: pb.matrix_basis.copy() for pb in ob.pose.bones if pb.name not in src_map.values()}
    # sample the clip
    f0, f1 = clip.frame_range
    u0, u1 = clip_range
    seconds = seconds or (f1 - f0) * (u1 - u0) / FPS
    n = max(1, int(round(seconds * FPS)))
    frames = []
    for i in range(n + 1):
        _sample(src, clip, u0 + (u1 - u0) * i / n)
        w = _world(src, True)
        rel = (w[pelvis_s][0] - w[root_s][0]) - rest_rel
        frames.append(({t: w[s][2] for s, t in src_map.items()}, wt["pzadek"][0] + floor + rel * scale))
    if loop:
        frames[-1] = frames[0]
    # bones that follow the reference move, sampled over its cycle at the same fraction
    held = set()
    for b in keep_bones:
        held.add(b)
        held.update(c.name for c in ob.data.bones[b].children_recursive)
    kept = [{} for _ in frames]
    if held:
        if not keep_from:
            raise ValueError("keep_bones needs keep_from (the move they follow)")
        for i in range(len(frames)):
            _sample(ob, bpy.data.actions[keep_from], i / n)
            kept[i] = {b: ob.pose.bones[b].matrix_basis.copy() for b in held}
        frames = [({t: r for t, r in rots.items() if t not in held}, p) for rots, p in frames]
    # the pelvis' mean height over the cycle matches the reference move's (a swimmer's waterline)
    if match_height_of:
        hs = []
        for i in range(21):
            _sample(ob, bpy.data.actions[match_height_of], i / 20)
            hs.append((ob.matrix_world @ ob.pose.bones["pzadek"].head).z)
        dz = sum(hs) / len(hs) - sum(p.z for _, p in frames) / len(frames)
        frames = [(r, p + Vector((0, 0, dz))) for r, p in frames]
    # key the rig
    act = bpy.data.actions.get(name)
    if act:
        bpy.data.actions.remove(act)
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data.action = act
    _r.set_joint_limits(ob, False)
    inv_world = ob.matrix_world.inverted()
    prev = {}
    for k, (rots, pelvis) in enumerate(frames):
        posed = {}
        for bone in ob.data.bones:  # parents come before children in Blender's bone list
            b = bone.name
            pb = ob.pose.bones[b]
            if bone.parent:
                chain = posed[bone.parent.name] @ (bone.parent.matrix_local.inverted() @ bone.matrix_local)
            else:
                chain = bone.matrix_local.copy()
            if b in rots:
                M = (inv_world.to_3x3() @ rots[b] @ K[b]).to_4x4()
                M.translation = (inv_world @ pelvis) if b == "pzadek" else chain.translation
                basis = chain.inverted() @ M
                lim = _r.LIMITS.get(b) if clamp and not b.endswith("biceps") else None
                if lim:
                    loc0, rot0, _ = basis.decompose()
                    e = rot0.to_euler(pb.rotation_mode, prev[b]) if b in prev else rot0.to_euler(pb.rotation_mode)
                    for i, (lo, hi) in enumerate(lim):
                        e[i] = min(max(e[i], math.radians(lo)), math.radians(hi))
                    basis = Matrix.Translation(loc0) @ e.to_matrix().to_4x4()
                    M = chain @ basis
            else:
                basis = kept[k].get(b) or keep.get(b, Matrix())
                M = chain @ basis
            posed[b] = M
            loc, rot, _ = basis.decompose()
            eul = rot.to_euler(pb.rotation_mode, prev[b]) if b in prev else rot.to_euler(pb.rotation_mode)
            prev[b] = eul
            pb.location = (0.0, 0.0, 0.0) if b in rots and b != "pzadek" else loc
            pb.rotation_euler = eul
            pb.keyframe_insert("location", frame=k)
            pb.keyframe_insert("rotation_euler", frame=k)
    act.frame_range = (0, n)
    act["source"] = clip.name
    act["seconds"] = seconds
    return act
