bl_info = {
    "name": "Poseidon P3D Tools",
    "author": "CWR Project",
    "version": (1, 1, 0),
    "blender": (3, 6, 0),
    "location": "File > Import / Export > Poseidon; sidebar > Poseidon",
    "description": "Import Poseidon engine P3D models (MLOD & ODOL), export MLOD; RTM animation on a soldier rig",
    "category": "Import-Export",
}

import bpy
from bpy.props import CollectionProperty, IntProperty, PointerProperty, StringProperty

from . import p3d_lib, rig_ops
from .export_p3d import ExportP3D
from .import_p3d import ImportP3D
from .operators import (
    POSEIDON_OT_activate_material,
    POSEIDON_OT_clear_highlight,
    POSEIDON_OT_highlight_selection,
    POSEIDON_OT_import_vfs_p3d,
    POSEIDON_OT_import_vfs_p3d_by_index,
    POSEIDON_OT_load_animation,
    POSEIDON_OT_refresh_animation_match,
    POSEIDON_OT_scan_animations,
    POSEIDON_OT_scan_vfs,
    POSEIDON_OT_select_group_vertices,
    POSEIDON_OT_select_material_faces,
    POSEIDON_OT_select_proxy,
    POSEIDON_OT_toggle_proxies,
)
from .panels import (
    POSEIDON_PT_animations,
    POSEIDON_PT_game_data,
    POSEIDON_PT_model_info,
    POSEIDON_PT_models,
    POSEIDON_PT_named_properties,
    POSEIDON_PT_named_selections,
    POSEIDON_PT_proxies,
    POSEIDON_PT_textures,
    POSEIDON_UL_p3d_list,
    POSEIDON_UL_rtm_list,
)


class POSEIDON_VfsP3DItem(bpy.types.PropertyGroup):
    name: StringProperty()
    vfs_path: StringProperty()


class POSEIDON_VfsRTMItem(bpy.types.PropertyGroup):
    name: StringProperty()
    vfs_path: StringProperty()
    bone_count: IntProperty()
    phase_count: IntProperty()
    match_count: IntProperty()
    bone_names: StringProperty()  # comma-separated


class PoseidonSceneProperties(bpy.types.PropertyGroup):
    game_data_dir: StringProperty(
        name="Game Data",
        description="CWR game data root folder",
        subtype="DIR_PATH",
        default="",
    )
    vfs_p3d_list: CollectionProperty(type=POSEIDON_VfsP3DItem)
    vfs_p3d_index: IntProperty(default=0)
    vfs_search: StringProperty(
        name="Search",
        description="Filter models by name",
        default="",
        options={"TEXTEDIT_UPDATE"},
    )
    vfs_rtm_list: CollectionProperty(type=POSEIDON_VfsRTMItem)
    vfs_rtm_index: IntProperty(default=0)
    vfs_rtm_search: StringProperty(
        name="Search RTM",
        description="Filter animations by name",
        default="",
        options={"TEXTEDIT_UPDATE"},
    )


def menu_func_import(self, context):
    self.layout.operator(ImportP3D.bl_idname, text="Poseidon P3D (.p3d)")


_classes = (
    POSEIDON_VfsP3DItem,
    POSEIDON_VfsRTMItem,
    POSEIDON_OT_scan_vfs,
    POSEIDON_OT_import_vfs_p3d,
    POSEIDON_OT_import_vfs_p3d_by_index,
    POSEIDON_OT_highlight_selection,
    POSEIDON_OT_clear_highlight,
    POSEIDON_OT_select_material_faces,
    POSEIDON_OT_activate_material,
    POSEIDON_OT_select_group_vertices,
    POSEIDON_OT_toggle_proxies,
    POSEIDON_OT_select_proxy,
    POSEIDON_OT_scan_animations,
    POSEIDON_OT_refresh_animation_match,
    POSEIDON_OT_load_animation,
    POSEIDON_UL_p3d_list,
    POSEIDON_UL_rtm_list,
    PoseidonSceneProperties,
    POSEIDON_PT_game_data,
    POSEIDON_PT_models,
    POSEIDON_PT_model_info,
    POSEIDON_PT_named_selections,
    POSEIDON_PT_proxies,
    POSEIDON_PT_named_properties,
    POSEIDON_PT_textures,
    POSEIDON_PT_animations,
    ImportP3D,
    ExportP3D,
    *rig_ops.CLASSES,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.poseidon = PointerProperty(type=PoseidonSceneProperties)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)
    bpy.types.TOPBAR_MT_file_import.append(rig_ops.menu_import)
    bpy.types.TOPBAR_MT_file_export.append(rig_ops.menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(rig_ops.menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(rig_ops.menu_import)
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
    del bpy.types.Scene.poseidon
    p3d_lib.unload()


if __name__ == "__main__":
    register()
