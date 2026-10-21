"""Headless Blender check of the add-on's soldier rig and model export -- no game data needed.

    blender --background --factory-startup --python tests/BlenderAddon/blender_rig_check.py

POSEIDON_FORMATS_DLL may point at a PoseidonFormats library with the writers; without one the model export part is
skipped. Exits 1 on the first failure.
"""

import math
import os
import sys
import tempfile

import bpy
import numpy as np

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "apps", "tools", "BlenderAddon"))
import io_import_p3d
from io_import_p3d import p3d_lib, rig
from io_import_p3d.rtm_io import read_rtm

failures = []


def check(cond, what):
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


io_import_p3d.register()
tmp = tempfile.mkdtemp(prefix="poseidon_rig_check_")
ctx = bpy.context

# the rig, without the game's mesh
ob = rig.build(ctx)
check(len(ob.data.bones) == len(rig.RTM_BONES), "rig has the RTM bones")
check(all(pb.rotation_mode == rig.rotation_mode(pb.name) for pb in ob.pose.bones), "rotation modes (YZX limbs)")
check(any(c.name == "Joint limit" for pb in ob.pose.bones for c in pb.constraints), "joint limits on the rig")

# RTM round trip through an action
src = os.path.join(REPO, "content", "default-packs", "swim", "rtm", "tread.rtm")
act = rig.import_rtm_file(ob, src)
out = os.path.join(tmp, "tread.rtm")
rig.export_rtm(ob, act, out, phases=act["rtm_phases"], step=tuple(act["rtm_step"]))
a, b = read_rtm(src), read_rtm(out)
ia = {x: i for i, x in enumerate(a["names"])}
err = max(
    np.abs(np.array(ma[ia[n]]) - np.array(mb[j])).max()
    for (_, ma), (_, mb) in zip(a["phases"], b["phases"])
    for j, n in enumerate(b["names"])
)
check(err < 1e-5, f"tread.rtm round trip ({err:.1e})")
hits, cross = rig.collision_hits(ob, act)
check(not hits and not cross, "tread: no collisions, no crossed arms")
check(not rig.joint_limit_hits(ob, act), "tread: within the joint limits")

# a bad pose is caught: arms crossed in front, a knee bent backwards
bad = bpy.data.actions.new("bad_pose")
ob.animation_data.action = bad
rig.set_joint_limits(ob, False)
for f in (0, 1):
    for side, sign in (("l", -1), ("p", 1)):
        pb = ob.pose.bones[side + "biceps"]
        pb.rotation_euler = (math.radians(90), 0, math.radians(sign * 60))
        pb.keyframe_insert("rotation_euler", frame=f)
    kn = ob.pose.bones["lholen"]
    kn.rotation_euler = (math.radians(40), 0, 0)
    kn.keyframe_insert("rotation_euler", frame=f)
bad.frame_range = (0, 1)
hits, cross = rig.collision_hits(ob, bad)
check(bool(cross), "crossed arms are reported")
lim = rig.joint_limit_hits(ob, bad)
check(any(b == "lholen" and ax == "x" for _, b, ax, _ in lim), "a backward knee breaks its limit")

# retargeting a UE-mannequin clip (retarget.py): a T-posed armature with the Quaternius joint layout, posed on one
# frame; every soldier bone must then point where the clip's bone points, and kept arms must follow the reference move
from io_import_p3d import compare, retarget
from mathutils import Vector

UE = {  # joint: (parent, head), metres, Z up, facing -Y (glTF import convention); left is +X
    "root": (None, (0, 0, 0)), "pelvis": ("root", (0, 0.05, 0.917)), "spine_01": ("pelvis", (0, 0.016, 1.051)),
    "spine_02": ("spine_01", (0, 0, 1.174)), "spine_03": ("spine_02", (0, 0.005, 1.315)),
    "neck_01": ("spine_03", (0, 0.011, 1.488)), "Head": ("neck_01", (0, -0.005, 1.569)),
}
for s, x in (("l", 1), ("r", -1)):
    UE.update({
        f"clavicle_{s}": ("spine_03", (0.019 * x, -0.071, 1.458)),
        f"upperarm_{s}": (f"clavicle_{s}", (0.192 * x, 0.065, 1.441)),
        f"lowerarm_{s}": (f"upperarm_{s}", (0.466 * x, 0.07, 1.441)),
        f"hand_{s}": (f"lowerarm_{s}", (0.739 * x, 0.065, 1.441)),
        f"thigh_{s}": ("pelvis", (0.089 * x, -0.001, 0.932)), f"calf_{s}": (f"thigh_{s}", (0.089 * x, 0.001, 0.532)),
        f"foot_{s}": (f"calf_{s}", (0.089 * x, 0.036, 0.104)), f"ball_{s}": (f"foot_{s}", (0.089 * x, -0.113, 0.015)),
    })
arm_data = bpy.data.armatures.new("ue_clip")
ue = bpy.data.objects.new("ue_clip", arm_data)
ctx.scene.collection.objects.link(ue)
ctx.view_layer.objects.active = ue
bpy.ops.object.mode_set(mode="EDIT")
kids = {}
for n, (p, h) in UE.items():
    kids.setdefault(p, []).append(n)
for n, (p, h) in UE.items():
    e = arm_data.edit_bones.new(n)
    e.head = h
    c = [k for k in kids.get(n, []) if not k.startswith(("clavicle", "thigh"))]
    e.tail = Vector(UE[c[0]][1]) if c else Vector(h) + (Vector(h) - Vector(UE[p][1])).normalized() * 0.08
    if p:
        e.parent = arm_data.edit_bones[p]
bpy.ops.object.mode_set(mode="OBJECT")
clip = bpy.data.actions.new("ue_wave")
ue.animation_data_create().action = clip
for pb in ue.pose.bones:
    pb.rotation_mode = "XYZ"
    pb.keyframe_insert("rotation_euler", frame=0)
for n, r in (("upperarm_r", (0.4, 0.2, -1.0)), ("lowerarm_r", (0.9, 0, 0)), ("thigh_l", (-0.6, 0.1, 0.2)),
             ("calf_l", (0.8, 0, 0)), ("spine_02", (0.3, 0.2, 0.1)), ("neck_01", (-0.2, 0.3, 0))):
    pb = ue.pose.bones[n]
    pb.rotation_euler = r
    pb.keyframe_insert("rotation_euler", frame=24)
clip.frame_range = (0, 24)
mc_act = retarget.retarget(ue, clip, "ue_wave_mc", loop=False, clamp=False, keep_from="tread")
check(len(retarget.source_bones(ue)) == len(retarget.MAP), "every soldier bone mapped from UE names")


def aims(o, names, joint_of, posed=True):
    ctx.view_layer.update()
    pos = {b.name: o.matrix_world @ (b.head if posed else b.bone.head_local) for b in o.pose.bones}
    tail = {b.name: o.matrix_world @ (b.tail if posed else b.bone.tail_local) for b in o.pose.bones}
    return {n: ((pos[joint_of[n]] if joint_of[n] else tail[n]) - pos[n]).normalized() for n in names}


ue_aim_of = retarget._aim_src(retarget.source_bones(ue))
worst = 1.0
for f in (0, 12, 24):
    ue.animation_data.action, ob.animation_data.action = clip, mc_act
    ctx.scene.frame_set(f)
    a_src = aims(ue, ue_aim_of, ue_aim_of)
    a_mc = aims(ob, retarget.AIM_T, retarget.AIM_T)
    for s, t in retarget.source_bones(ue).items():
        if retarget.AIM_T[t] and ue_aim_of[s]:  # leaf bones aim along their own tails, which differ by rig
            worst = min(worst, a_src[s].dot(a_mc[t]))
check(worst > 0.9999, f"retargeted bones point along the clip's (worst cos {worst:.6f})")
# the feet stand on the game's floor (z = 0; a stock move's lowest foot point is ~0.02), not at the rig's rest height
# (~1.06 m below it: the first jump moves sank the man to the waist). Loose: this rig's toe tails are extrapolated
# past its floor, so the lowest point here is ~-0.08; real Quaternius clips land within 0.02.
ob.animation_data.action = mc_act
ctx.scene.frame_set(0)
ctx.view_layer.update()
feet = min(min((ob.matrix_world @ ob.pose.bones[b].head).z, (ob.matrix_world @ ob.pose.bones[b].tail).z)
           for b in retarget.FOOT_BONES)
check(abs(feet) < 0.15, f"retargeted feet stand on the floor (lowest {feet:.3f} m)")
kept = retarget.retarget(ue, clip, "ue_wave_kept", loop=False, keep_from="tread", keep_bones=retarget.ARMS_AND_RIFLE)
ob.animation_data.action = act
ctx.scene.frame_set(0)
tread0 = ob.pose.bones["pbiceps"].matrix_basis.copy()
ob.animation_data.action = kept
ctx.scene.frame_set(0)
check(np.allclose(np.array(ob.pose.bones["pbiceps"].matrix_basis), np.array(tread0), atol=1e-5),
      "kept arms follow the reference move")
rows = compare.add_row(0.0, [("tread", 2.38, "CURRENT", "tread"), ("ue_wave_mc", 1.0, "CANDIDATE", "test")])
strip = bpy.data.objects[rows[0]].animation_data.nla_tracks[0].strips[0]
scaled = abs(strip.scale - 2.38 * 24 / act.frame_range[1]) < 1e-4
check(len(rows) == 2 and scaled, "comparison row plays at the cycle time")

# model export (needs the DLL's writers)
try:
    p3d_lib.load()
    can = p3d_lib.can_write()
except Exception:
    can = False
if not can:
    print("skip  model export: no PoseidonFormats library with writers (POSEIDON_FORMATS_DLL)")
else:
    from io_import_p3d import export_p3d

    bpy.ops.mesh.primitive_cube_add(size=1.0)
    cube = ctx.active_object
    vg = cube.vertex_groups.new(name="lid")
    vg.add([v.index for v in cube.data.vertices if v.co.z > 0], 1.0, "REPLACE")
    cube["cwr_class"] = "house"
    path = os.path.join(tmp, "cube.p3d")
    rep = export_p3d.export_p3d(ctx, path, [cube])
    m = p3d_lib.P3DModel(path)
    check(m.format == "MLOD" and m.lod_count == 1 and m.face_count(0) == 12, "cube exported as MLOD (12 triangles)")
    check("lid" in [m.selection_name(0, i) for i in range(m.selection_count(0))], "vertex group -> named selection")
    props = {m.property_name(0, i): m.property_value(0, i) for i in range(m.property_count(0))}
    check(props.get("class") == "house", "custom property -> named property")
    V = np.array(m.get_vertices(0)) + np.array(m.bounding_center())
    check(
        np.allclose(sorted(V[:, 1].round(3)), sorted(V[:, 1].round(3))) and abs(V[:, 1].max() - 0.5) < 1e-4,
        "Blender Z up -> model Y up",
    )

io_import_p3d.unregister()
print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failure(s)")
sys.exit(1 if failures else 0)
