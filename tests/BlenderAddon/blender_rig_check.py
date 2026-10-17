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
