"""Operators and the sidebar panel for the soldier rig (rig.py) and for model export (export_p3d.py)."""

import os

import bpy
from bpy.props import BoolProperty, FloatVectorProperty, IntProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import p3d_lib, rig


def _rig(context):
    ob = bpy.data.objects.get(rig.ARM)
    return ob if ob and ob.type == "ARMATURE" else None


def _ensure_rig(context):
    ob = _rig(context)
    if ob is None:
        p3d_lib.load()
        path = rig.model_from_game_data()
        ob = rig.build(context, model_path=path)
    return ob


def _fmt(items, n=6):
    return ", ".join(str(x) for x in items[:n]) + (" ..." if len(items) > n else "")


class POSEIDON_OT_build_soldier_rig(bpy.types.Operator):
    """Build the editable MC soldier rig (MC_Rig), skinned and textured from the mounted game data"""

    bl_idname = "poseidon.build_soldier_rig"
    bl_label = "Build Soldier Rig"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        p3d_lib.load()
        path = rig.model_from_game_data()
        if not path:
            self.report(
                {"WARNING"},
                "No game data mounted (Game Data panel): built the rig without the soldier mesh; "
                "File > Import > Poseidon Soldier Rig takes a .p3d file",
            )
        rig.build(context, model_path=path)
        self.report({"INFO"}, "MC_Rig built" + (" with the soldier mesh" if path else ""))
        return {"FINISHED"}


class POSEIDON_OT_import_soldier_rig(bpy.types.Operator, ImportHelper):
    """Build the editable MC soldier rig, skinned from an MC soldier model (.p3d, e.g. extracted 'mc vojakw2.p3d')"""

    bl_idname = "import_scene.poseidon_soldier_rig"
    bl_label = "Import Poseidon Soldier Rig"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".p3d"
    filter_glob: StringProperty(default="*.p3d", options={"HIDDEN"})
    texture_root: StringProperty(
        name="Texture Folder",
        subtype="DIR_PATH",
        default="",
        description="Folder holding the game's texture paths (merged\\..., data\\...) when no game data is mounted",
    )

    def execute(self, context):
        p3d_lib.load()
        rig.build(context, model_path=self.filepath, texture_root=bpy.path.abspath(self.texture_root) or None)
        return {"FINISHED"}


class POSEIDON_OT_import_rtm(bpy.types.Operator, ImportHelper):
    """Import an RTM animation as an editable action on the soldier rig (built first if there is none)"""

    bl_idname = "import_anim.poseidon_rtm"
    bl_label = "Import Poseidon Animation"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".rtm"
    filter_glob: StringProperty(default="*.rtm", options={"HIDDEN"})

    def execute(self, context):
        ob = _ensure_rig(context)
        act = rig.import_rtm_file(ob, self.filepath)
        context.scene.frame_start, context.scene.frame_end = 0, int(act.frame_range[1])
        self.report({"INFO"}, f"{act.name}: {act['rtm_phases']} phases on {ob.name} (joint limits off)")
        return {"FINISHED"}


class POSEIDON_OT_rtm_to_rig(bpy.types.Operator):
    """Load the selected game animation onto the soldier rig as an editable action"""

    bl_idname = "poseidon.rtm_to_rig"
    bl_label = "Load onto Soldier Rig"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        props = context.scene.poseidon
        if not (0 <= props.vfs_rtm_index < len(props.vfs_rtm_list)):
            self.report({"ERROR"}, "No animation selected")
            return {"CANCELLED"}
        ob = _ensure_rig(context)
        try:
            act = rig.import_rtm_vfs(ob, props.vfs_rtm_list[props.vfs_rtm_index].vfs_path)
        except RuntimeError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        context.scene.frame_start, context.scene.frame_end = 0, int(act.frame_range[1])
        self.report({"INFO"}, f"{act.name}: {act['rtm_phases']} phases (joint limits off)")
        return {"FINISHED"}


def _check(ob, action):
    hits, cross = rig.collision_hits(ob, action)
    limits = rig.joint_limit_hits(ob, action)
    return hits, cross, limits


def _report_check(op, hits, cross, limits):
    if hits:
        op.report(
            {"WARNING"},
            f"{len(hits)} frames with limbs inside each other: " + _fmt([f"f{f} {a}/{b}" for f, a, b, _ in hits]),
        )
    if cross:
        op.report(
            {"WARNING"},
            f"{len(cross)} frames with a hand past the body's middle: " + _fmt([f"f{f} {b}" for f, b in cross]),
        )
    if limits:
        op.report(
            {"WARNING"}, f"{len(limits)} joint-limit breaks: " + _fmt([f"f{f} {b} {a} {d}" for f, b, a, d in limits])
        )


class POSEIDON_OT_check_motion(bpy.types.Operator):
    """Check the rig's action for limbs inside each other, hands crossing the body, and joints past their limits"""

    bl_idname = "poseidon.check_motion"
    bl_label = "Check Motion"

    def execute(self, context):
        ob = _rig(context)
        if not ob or not ob.animation_data or not ob.animation_data.action:
            self.report({"ERROR"}, "The soldier rig has no action")
            return {"CANCELLED"}
        hits, cross, limits = _check(ob, ob.animation_data.action)
        _report_check(self, hits, cross, limits)
        if not (hits or cross or limits):
            self.report({"INFO"}, f"{ob.animation_data.action.name}: clean")
        return {"FINISHED"}


class POSEIDON_OT_joint_limits(bpy.types.Operator):
    """Switch the rig's joint limits on or off (on while posing; importing a game move switches them off)"""

    bl_idname = "poseidon.joint_limits"
    bl_label = "Joint Limits"
    on: BoolProperty(default=True)

    def execute(self, context):
        ob = _rig(context)
        if not ob:
            return {"CANCELLED"}
        rig.set_joint_limits(ob, self.on)
        ob["joint_limits"] = self.on
        return {"FINISHED"}


class POSEIDON_OT_export_rtm(bpy.types.Operator, ExportHelper):
    """Export the soldier rig's action as an RTM animation (checked for collisions and joint limits first)"""

    bl_idname = "export_anim.poseidon_rtm"
    bl_label = "Export Poseidon Animation"
    filename_ext = ".rtm"
    filter_glob: StringProperty(default="*.rtm", options={"HIDDEN"})
    phases: IntProperty(
        name="Phases",
        default=0,
        min=0,
        max=1000,
        description="Evenly spaced samples over the action (0: one per frame)",
    )
    step: FloatVectorProperty(
        name="Step",
        size=3,
        default=(0.0, 0.0, 0.0),
        description="Travel per cycle in model space (forward is -Z: a 1.6 m stroke is 0, 0, -1.6)",
    )
    check: BoolProperty(
        name="Check First",
        default=True,
        description="Refuse to write a move with limbs inside each other or joints past their limits",
    )

    def invoke(self, context, event):
        ob = _rig(context)
        if ob and ob.animation_data and ob.animation_data.action:
            act = ob.animation_data.action
            self.filepath = act.name + ".rtm"
            if "rtm_step" in act:
                self.step = act["rtm_step"]
            if "rtm_phases" in act:
                self.phases = int(act["rtm_phases"])
        return super().invoke(context, event)

    def execute(self, context):
        ob = _rig(context)
        if not ob or not ob.animation_data or not ob.animation_data.action:
            self.report({"ERROR"}, "The soldier rig has no action")
            return {"CANCELLED"}
        act = ob.animation_data.action
        if self.check:
            hits, cross, limits = _check(ob, act)
            if hits or cross or limits:
                _report_check(self, hits, cross, limits)
                self.report({"ERROR"}, "Not written: fix the frames above, or untick Check First")
                return {"CANCELLED"}
        n = rig.export_rtm(ob, act, self.filepath, phases=self.phases or None, step=tuple(self.step))
        self.report({"INFO"}, f"Wrote {os.path.basename(self.filepath)}: {n} phases")
        return {"FINISHED"}


class POSEIDON_PT_soldier_rig(bpy.types.Panel):
    bl_label = "Soldier Rig"
    bl_idname = "POSEIDON_PT_soldier_rig"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 8

    def draw(self, context):
        layout = self.layout
        ob = _rig(context)
        layout.operator(
            POSEIDON_OT_build_soldier_rig.bl_idname,
            icon="ARMATURE_DATA",
            text="Rebuild Soldier Rig" if ob else "Build Soldier Rig",
        )
        if not ob:
            layout.label(text="Mount the game data for the soldier's mesh", icon="INFO")
            return
        act = ob.animation_data.action if ob.animation_data else None
        layout.label(text=f"Action: {act.name}" if act else "No action", icon="ACTION")
        row = layout.row(align=True)
        on = ob.get("joint_limits", True)
        op = row.operator(
            POSEIDON_OT_joint_limits.bl_idname,
            text="Limits On" if not on else "Limits Off",
            icon="CON_ROTLIMIT",
            depress=on,
        )
        op.on = not on
        row.operator(POSEIDON_OT_check_motion.bl_idname, icon="CHECKMARK")
        col = layout.column(align=True)
        col.operator(POSEIDON_OT_import_rtm.bl_idname, text="Import RTM...", icon="IMPORT")
        col.operator(POSEIDON_OT_export_rtm.bl_idname, text="Export RTM...", icon="EXPORT")


CLASSES = (
    POSEIDON_OT_build_soldier_rig,
    POSEIDON_OT_import_soldier_rig,
    POSEIDON_OT_import_rtm,
    POSEIDON_OT_rtm_to_rig,
    POSEIDON_OT_check_motion,
    POSEIDON_OT_joint_limits,
    POSEIDON_OT_export_rtm,
    POSEIDON_PT_soldier_rig,
)


def menu_import(self, context):
    self.layout.operator(POSEIDON_OT_import_soldier_rig.bl_idname, text="Poseidon Soldier Rig (.p3d)")
    self.layout.operator(POSEIDON_OT_import_rtm.bl_idname, text="Poseidon Animation (.rtm)")


def menu_export(self, context):
    from .export_p3d import ExportP3D

    self.layout.operator(ExportP3D.bl_idname, text="Poseidon P3D (.p3d)")
    self.layout.operator(POSEIDON_OT_export_rtm.bl_idname, text="Poseidon Animation (.rtm)")
