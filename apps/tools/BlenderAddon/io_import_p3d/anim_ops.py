"""Operators and the sidebar panel for new soldier moves from outside clips: import a humanoid clip library (GLB),
retarget a clip onto the soldier rig (retarget.py), and line moves up side by side for choosing (compare.py)."""

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty
from bpy_extras.io_utils import ImportHelper

from . import compare, retarget, rig
from .rig_ops import _ensure_rig, _fmt, _rig


class POSEIDON_OT_import_clip(bpy.types.Operator, ImportHelper):
    """Import a humanoid animation library or clip (glTF/GLB, UE-mannequin bone names: Quaternius, ActionForge) to
    retarget onto the soldier rig. Check the clip's licence first: CC0 and CMU mocap are fine, Mixamo is not"""

    bl_idname = "import_anim.poseidon_clip"
    bl_label = "Import Humanoid Clips"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".glb"
    filter_glob: StringProperty(default="*.glb;*.gltf", options={"HIDDEN"})

    def execute(self, context):
        _ensure_rig(context)
        src, act = retarget.import_glb(self.filepath)
        mapped = retarget.source_bones(src)
        context.scene["clip_source"] = src.name
        self.report({"INFO"}, f"{src.name}: {len(mapped)} of {len(retarget.MAP)} soldier bones mapped; "
                    f"first clip {act.name if act else '-'}")
        return {"FINISHED"}


class POSEIDON_OT_retarget_clip(bpy.types.Operator):
    """Retarget a clip onto the soldier rig as a new action (then Check Motion, Export RTM, or Add to Comparison)"""

    bl_idname = "poseidon.retarget_clip"
    bl_label = "Retarget Clip onto Soldier"
    bl_options = {"REGISTER", "UNDO"}
    source: StringProperty(name="Clip Rig", description="The imported clip's armature")
    clip: StringProperty(name="Clip", description="The clip (an action of the clip rig)")
    name: StringProperty(name="New Action", description="Name of the soldier action to make")
    reference: StringProperty(
        name="Reference Move",
        description="A soldier move: the rifle's sling pose comes from it, and the kept arms follow it",
    )
    keep_arms: BoolProperty(
        name="Keep the Reference's Arms",
        default=False,
        description="The arms and the rifle follow the reference move (for an unarmed clip under a rifle move)",
    )
    match_height: BoolProperty(
        name="Match Its Pelvis Height",
        default=False,
        description="Shift the pelvis to the reference move's mean height (a swimmer's waterline)",
    )
    loop: BoolProperty(name="Loop", default=True, description="Make the last key the first")
    clamp: BoolProperty(
        name="Joint Limits", default=True, description="Hold every joint but the upper arms in its limits"
    )

    def invoke(self, context, event):
        self.source = self.source or context.scene.get("clip_source", "")
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, context):
        lay = self.layout
        lay.prop_search(self, "source", bpy.data, "objects")
        lay.prop_search(self, "clip", bpy.data, "actions")
        lay.prop(self, "name")
        lay.prop_search(self, "reference", bpy.data, "actions")
        col = lay.column()
        col.enabled = bool(self.reference)
        col.prop(self, "keep_arms")
        col.prop(self, "match_height")
        lay.prop(self, "loop")
        lay.prop(self, "clamp")

    def execute(self, context):
        ob = _ensure_rig(context)
        src = bpy.data.objects.get(self.source)
        clip = bpy.data.actions.get(self.clip)
        if not src or src.type != "ARMATURE" or not clip:
            self.report({"ERROR"}, "Pick the clip's armature and one of its clips")
            return {"CANCELLED"}
        ref = self.reference or None
        try:
            act = retarget.retarget(src, clip, self.name or clip.name + "_mc", keep_from=ref,
                                    match_height_of=ref if self.match_height else None, loop=self.loop,
                                    clamp=self.clamp, keep_bones=retarget.ARMS_AND_RIFLE if self.keep_arms else ())
        except ValueError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        ob.animation_data.action = act
        context.scene.frame_start, context.scene.frame_end = 0, int(act.frame_range[1])
        hits, cross = rig.collision_hits(ob, act)
        limits = rig.joint_limit_hits(ob, act)
        msg = f"{act.name}: {act['seconds']:.2f} s from {clip.name}"
        if hits or cross or limits:
            self.report({"WARNING"}, msg + f"; {len(hits)} collision frames, {len(cross)} crossed-hand frames, "
                        f"{len(limits)} limit breaks: " + _fmt([f"f{f} {a}/{b}" for f, a, b, _ in hits]))
        else:
            self.report({"INFO"}, msg + "; clean")
        return {"FINISHED"}


class POSEIDON_OT_compare_add(bpy.types.Operator):
    """Add a row to the comparison: the move in the game and a candidate to replace it (or a new move alone), each on
    its own soldier, playing at its in-game cycle time"""

    bl_idname = "poseidon.compare_add"
    bl_label = "Add to Comparison"
    bl_options = {"REGISTER", "UNDO"}
    current: StringProperty(name="Current Move", description="The move in the game now (empty: a new move)")
    current_seconds: FloatProperty(name="Its Cycle (s)", default=1.0, min=0.05, description="config.cpp: -speed")
    candidate: StringProperty(name="Candidate", description="The move that would replace it")
    candidate_seconds: FloatProperty(name="Its Cycle (s)", default=1.0, min=0.05)
    note: StringProperty(name="Note", description="A line under the candidate's label (its source)")
    water: BoolProperty(name="Water", default=False, description="Draw the water at the chin line (swim moves)")

    def invoke(self, context, event):
        ob = _rig(context)
        act = ob.animation_data.action if ob and ob.animation_data else None
        if act and not self.candidate:
            self.candidate = act.name
            self.candidate_seconds = float(act.get("seconds", act.frame_range[1] / compare.FPS))
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, context):
        lay = self.layout
        lay.prop_search(self, "current", bpy.data, "actions")
        lay.prop(self, "current_seconds")
        lay.prop_search(self, "candidate", bpy.data, "actions")
        lay.prop(self, "candidate_seconds")
        lay.prop(self, "note")
        lay.prop(self, "water")

    def execute(self, context):
        if not self.candidate or self.candidate not in bpy.data.actions:
            self.report({"ERROR"}, "Pick the candidate move")
            return {"CANCELLED"}
        rows = [o.get("cmp_row") for o in bpy.data.objects if o.get("cmp_row") is not None]
        y = (max(rows) + 3.5) if rows else 0.0
        items = []
        if self.current:
            label = f"{self.current}, {self.current_seconds:.2f} s"
            items.append((self.current, self.current_seconds, "CURRENT", label))
        detail = f"{self.candidate}, {self.candidate_seconds:.2f} s" + (f"\n{self.note}" if self.note else "")
        items.append((self.candidate, self.candidate_seconds, "CANDIDATE" if self.current else "NEW", detail))
        compare.add_row(y, items, water=self.water)
        self.report({"INFO"}, f"Comparison row at y={y:g}: " + ", ".join(i[0] for i in items))
        return {"FINISHED"}


class POSEIDON_PT_clips(bpy.types.Panel):
    bl_label = "New Moves from Clips"
    bl_idname = "POSEIDON_PT_clips"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 9

    def draw(self, context):
        col = self.layout.column(align=True)
        col.operator(POSEIDON_OT_import_clip.bl_idname, text="Import Clips (.glb)...", icon="IMPORT")
        col.operator(POSEIDON_OT_retarget_clip.bl_idname, text="Retarget onto Soldier...", icon="ARMATURE_DATA")
        col.operator(POSEIDON_OT_compare_add.bl_idname, text="Add to Comparison...", icon="SPLIT_HORIZONTAL")
        rows = sorted({o.get("cmp_row") for o in bpy.data.objects if o.get("cmp_row") is not None})
        if rows:
            self.layout.label(text=f"{len(rows)} comparison rows (Space plays them)", icon="PLAY")


CLASSES = (POSEIDON_OT_import_clip, POSEIDON_OT_retarget_clip, POSEIDON_OT_compare_add, POSEIDON_PT_clips)


def menu_import(self, context):
    self.layout.operator(POSEIDON_OT_import_clip.bl_idname, text="Humanoid Clips for the Soldier (.glb)")
