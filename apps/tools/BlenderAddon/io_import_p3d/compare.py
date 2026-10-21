"""Side-by-side comparison of soldier moves in one scene, for an owner to choose between a move in the game and a
candidate to replace it: each move plays on its own copy of the soldier (MC_Rig + MC_Soldier) at its in-game cycle
time, in a row, labelled CURRENT (in the game now), CANDIDATE (would replace it) or NEW (no move like it yet).

    compare.add_row(0.0, [("tread", 2.38, "CURRENT", "tread, 2.38 s"),
                          ("tread_af", 3.33, "CANDIDATE", "ActionForge swim_idle, 3.33 s")], water=True)

Rows are re-made in place: adding a row with the same actions replaces the old copies. The copies play through NLA
strips (scaled to the cycle time and repeated to the scene's end), so the scene plays live with Space; water=True
draws the water 2 cm under each soldier's chin, where the engine floats a swimmer (SoldierOldSwim.cpp).
"""

import math

import bpy

from . import rig as _r

FPS = 24
KINDS = {
    "CURRENT": ("CURRENT - in game now", (0.08, 0.08, 0.08, 1)),
    "CANDIDATE": ("CANDIDATE - would replace it", (0.95, 0.45, 0.05, 1)),
    "NEW": ("NEW - no move like it yet", (0.1, 0.45, 0.85, 1)),
}
SPACING = 2.2


def _chin_mean(rig_ob, action):
    sc = bpy.context.scene
    rig_ob.animation_data_create()
    keep = rig_ob.animation_data.action
    rig_ob.animation_data.action = action
    r0, r1 = action.frame_range
    zs = []
    for i in range(24):
        f = r0 + (r1 - r0) * i / 24
        sc.frame_set(int(f), subframe=f - int(f))
        zs.append(rig_ob.pose.bones["hlava"].head.z)
    rig_ob.animation_data.action = keep
    return sum(zs) / len(zs)


def clear_row(y):
    for o in [o for o in bpy.data.objects if o.get("cmp_row") == y]:
        bpy.data.objects.remove(o)


def add_row(y, items, water=False, length=None):
    """items: [(action name, cycle seconds, kind, detail)], kind one of KINDS. Returns the rig copies' names."""
    sc = bpy.context.scene
    mc = bpy.data.objects[_r.ARM]
    mesh = bpy.data.objects.get(_r.MESH)
    if length:
        sc.frame_start, sc.frame_end = 0, int(length * FPS)
    frames = sc.frame_end - sc.frame_start + 1
    clear_row(y)
    made = []
    for i, (a, cycle, kind, detail) in enumerate(items):
        act = bpy.data.actions[a]
        x = (i - (len(items) - 1) / 2) * SPACING
        r = mc.copy()
        r.name = a + "_cmp"
        r["cmp_row"] = y
        sc.collection.objects.link(r)
        r.animation_data_clear()
        r.hide_set(False)
        r.hide_render = False
        r.location = (x, y, 0)
        for pb in r.pose.bones:
            for c in pb.constraints:
                c.mute = True
        track = r.animation_data_create().nla_tracks.new()
        f0, f1 = act.frame_range
        strip = track.strips.new(a, 0, act)
        if hasattr(strip, "action_slot") and len(getattr(act, "slots", [])):
            strip.action_slot = act.slots[0]
        strip.scale = (cycle * FPS) / max(1.0, f1 - f0)
        strip.repeat = math.ceil(frames / (cycle * FPS)) + 1
        if mesh:
            m = mesh.copy()
            m.name = a + "_cmp_soldier"
            m["cmp_row"] = y
            sc.collection.objects.link(m)
            m.parent = r
            m.matrix_parent_inverse.identity()
            m.hide_set(False)
            m.hide_render = False
            m.modifiers["Armature"].object = r
        top = 1.0
        if water:
            chin = _chin_mean(mc, act)
            bpy.ops.mesh.primitive_plane_add(size=1.7, location=(x, y, chin - 0.02))
            w = bpy.context.object
            w.name = a + "_cmp_water"
            w["cmp_row"] = y
            wm = bpy.data.materials.get("cmp_water") or bpy.data.materials.new("cmp_water")
            wm.diffuse_color = (0.15, 0.45, 0.65, 0.45)
            w.data.materials.append(wm)
            top = chin + 0.85
        bpy.ops.object.text_add(location=(x, y, top), rotation=(math.radians(80), 0, 0))
        t = bpy.context.object
        t.name = a + "_cmp_label"
        t["cmp_row"] = y
        title, colour = KINDS[kind]
        t.data.body = title + "\n" + detail
        t.data.size = 0.16
        t.data.align_x = "CENTER"
        tm = bpy.data.materials.get("cmp_" + kind) or bpy.data.materials.new("cmp_" + kind)
        tm.diffuse_color = colour
        t.data.materials.append(tm)
        t.color = colour
        made.append(r.name)
    mc.hide_set(True)
    if mesh:
        mesh.hide_set(True)
    return made
