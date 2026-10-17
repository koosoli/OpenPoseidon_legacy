import colorsys
import os

import bpy

from .helpers import _find_cwr_root_collection, _get_active_cwr_mesh
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


class POSEIDON_UL_p3d_list(bpy.types.UIList):
    bl_idname = "POSEIDON_UL_p3d_list"

    def draw_item(self, context, layout, data, item, icon, active_data, active_property, index):
        if self.layout_type in {"DEFAULT", "COMPACT"}:
            row = layout.row(align=True)
            row.label(text=item.vfs_path, icon="MESH_DATA")
            op = row.operator(POSEIDON_OT_import_vfs_p3d_by_index.bl_idname, text="", icon="IMPORT")
            op.index = index
        else:
            layout.alignment = "CENTER"
            layout.label(text=item.name, icon="MESH_DATA")

    def filter_items(self, context, data, propname):
        items = getattr(data, propname)
        n = len(items)
        flt_flags = [self.bitflag_filter_item] * n

        needle = context.scene.poseidon.vfs_search.lower() if hasattr(context.scene, "poseidon") else ""
        if needle:
            for i, item in enumerate(items):
                if needle not in item.vfs_path.lower():
                    flt_flags[i] = 0

        sorted_pairs = sorted(enumerate(items), key=lambda x: x[1].vfs_path.lower())
        flt_neworder = [p[0] for p in sorted_pairs]

        return flt_flags, flt_neworder


class POSEIDON_PT_game_data(bpy.types.Panel):
    bl_label = "Game Data"
    bl_idname = "POSEIDON_PT_game_data"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 0

    def draw(self, context):
        layout = self.layout
        props = context.scene.poseidon

        layout.prop(props, "game_data_dir", text="")
        if props.game_data_dir:
            d = bpy.path.abspath(props.game_data_dir)
            if os.path.isdir(d):
                row = layout.row(align=True)
                row.label(text="\u2713 Connected", icon="CHECKMARK")
                row.operator(POSEIDON_OT_scan_vfs.bl_idname, text="Scan", icon="FILE_REFRESH")
            else:
                layout.label(text="\u2717 Directory not found", icon="ERROR")

        layout.separator()
        layout.operator(ImportP3D.bl_idname, text="Import from File...", icon="FILEBROWSER")


class POSEIDON_PT_models(bpy.types.Panel):
    bl_label = "Models"
    bl_idname = "POSEIDON_PT_models"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 1

    @classmethod
    def poll(cls, context):
        props = context.scene.poseidon
        return len(props.vfs_p3d_list) > 0

    def draw(self, context):
        layout = self.layout
        props = context.scene.poseidon

        layout.label(text=f"{len(props.vfs_p3d_list)} models found", icon="MESH_DATA")
        layout.prop(props, "vfs_search", text="", icon="VIEWZOOM")
        layout.template_list(
            POSEIDON_UL_p3d_list.bl_idname,
            "",
            props,
            "vfs_p3d_list",
            props,
            "vfs_p3d_index",
            rows=12,
        )
        layout.operator(POSEIDON_OT_import_vfs_p3d.bl_idname, text="Import Selected", icon="IMPORT")


class POSEIDON_PT_model_info(bpy.types.Panel):
    bl_label = "Model Info"
    bl_idname = "POSEIDON_PT_model_info"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 2
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return _get_active_cwr_mesh(context) is not None

    def draw(self, context):
        layout = self.layout
        obj = _get_active_cwr_mesh(context)
        if not obj:
            return

        mesh = obj.data

        col = layout.column(align=True)
        col.label(text=obj.name, icon="OBJECT_DATA")
        fmt = obj.get("cwr_format", "?")
        lod_idx = obj.get("cwr_lod_index", -1)
        lod_res = obj.get("cwr_lod_resolution", 0)
        col.label(text=f"Format: {fmt}  |  LOD: {lod_idx}")
        col.label(text=f"Resolution: {lod_res}")

        layout.separator()

        box = layout.box()
        box.label(text="Mesh", icon="MESH_DATA")
        row = box.row()
        row.label(text=f"Vertices: {len(mesh.vertices)}")
        row.label(text=f"Faces: {len(mesh.polygons)}")
        row = box.row()
        row.label(text=f"Edges: {len(mesh.edges)}")
        row.label(text=f"Tris: {sum(len(p.vertices) - 2 for p in mesh.polygons)}")

        box = layout.box()
        box.label(text="Materials", icon="MATERIAL")
        box.label(text=f"Count: {len(obj.material_slots)}")
        tex_count = 0
        for slot in obj.material_slots:
            if slot.material and slot.material.use_nodes:
                for node in slot.material.node_tree.nodes:
                    if node.type == "TEX_IMAGE" and node.image:
                        tex_count += 1
        box.label(text=f"With textures: {tex_count}")

        box = layout.box()
        box.label(text="Named Selections", icon="GROUP_VERTEX")
        box.label(text=f"Count: {len(obj.vertex_groups)}")

        proxy_count = sum(1 for c in obj.children if c.type == "EMPTY")
        if proxy_count:
            box = layout.box()
            box.label(text="Proxies", icon="EMPTY_ARROWS")
            box.label(text=f"Count: {proxy_count}")

        cwr_props = [
            k
            for k in obj.keys()
            if k.startswith("cwr_") and k not in ("cwr_lod_index", "cwr_lod_resolution", "cwr_format")
        ]
        if cwr_props:
            box = layout.box()
            box.label(text="Named Properties", icon="PROPERTIES")
            box.label(text=f"Count: {len(cwr_props)}")

        root_col = _find_cwr_root_collection(obj)
        if root_col and len(root_col.children) > 0:
            box = layout.box()
            box.label(text="All LODs", icon="MOD_DECIM")
            for group_col in root_col.children:
                row = box.row()
                row.label(text=f"\u25b6 {group_col.name}", icon="OUTLINER_COLLECTION")
                for lod_col in group_col.children:
                    mesh_objs = [o for o in lod_col.all_objects if o.type == "MESH"]
                    if mesh_objs:
                        o = mesh_objs[0]
                        verts = len(o.data.vertices)
                        faces = len(o.data.polygons)
                        row = box.row()
                        row.label(text=f"    {lod_col.name}: {verts}v / {faces}f")


class POSEIDON_PT_named_selections(bpy.types.Panel):
    bl_label = "Named Selections"
    bl_idname = "POSEIDON_PT_named_selections"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 3
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        obj = _get_active_cwr_mesh(context)
        return obj is not None and len(obj.vertex_groups) > 0

    def draw(self, context):
        layout = self.layout
        obj = _get_active_cwr_mesh(context)
        if not obj:
            return

        layout.label(text=f"{len(obj.vertex_groups)} named selections", icon="GROUP_VERTEX")
        layout.operator(POSEIDON_OT_clear_highlight.bl_idname, text="Clear Highlight", icon="X")
        layout.separator()

        for i, vg in enumerate(obj.vertex_groups):
            vert_count = 0
            for v in obj.data.vertices:
                for g in v.groups:
                    if g.group == vg.index and g.weight > 0.01:
                        vert_count += 1
                        break

            hue = (i * 0.618033988749895) % 1.0
            r, g, b = colorsys.hsv_to_rgb(hue, 0.8, 0.9)

            row = layout.row(align=True)
            sub = row.row(align=True)
            sub.scale_x = 0.3
            sub.label(text="\u2588")

            row.label(text=f"{vg.name} ({vert_count}v)")
            op = row.operator(POSEIDON_OT_select_group_vertices.bl_idname, text="", icon="RESTRICT_SELECT_OFF")
            op.group_name = vg.name
            op = row.operator(POSEIDON_OT_highlight_selection.bl_idname, text="", icon="LIGHT")
            op.group_name = vg.name
            op.color = (r, g, b)


class POSEIDON_PT_proxies(bpy.types.Panel):
    bl_label = "Proxies"
    bl_idname = "POSEIDON_PT_proxies"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 4
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        obj = _get_active_cwr_mesh(context)
        return obj is not None and any(c.type == "EMPTY" and "cwr_proxy_path" in c for c in (obj.children or []))

    def draw(self, context):
        layout = self.layout
        obj = _get_active_cwr_mesh(context)
        if not obj:
            return

        proxies = [c for c in obj.children if c.type == "EMPTY" and "cwr_proxy_path" in c]
        layout.label(text=f"{len(proxies)} proxies", icon="EMPTY_ARROWS")

        layout.operator(POSEIDON_OT_toggle_proxies.bl_idname, text="Toggle Proxy Visibility", icon="HIDE_OFF")
        layout.separator()

        for prx in proxies:
            box = layout.box()
            row = box.row(align=True)
            row.label(text=prx.name, icon="EMPTY_ARROWS")
            op = row.operator(POSEIDON_OT_select_proxy.bl_idname, text="", icon="RESTRICT_SELECT_OFF")
            op.proxy_name = prx.name
            cwr_path = prx.get("cwr_proxy_path", "")
            if cwr_path:
                box.label(text=cwr_path, icon="FILE_3D")
            pos = prx.matrix_world.translation
            box.label(text=f"Pos: ({pos.x:.2f}, {pos.y:.2f}, {pos.z:.2f})")


_METADATA_KEYS = {"cwr_lod_index", "cwr_lod_resolution", "cwr_format"}


class POSEIDON_PT_named_properties(bpy.types.Panel):
    bl_label = "Named Properties"
    bl_idname = "POSEIDON_PT_named_properties"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 5
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        obj = _get_active_cwr_mesh(context)
        if not obj:
            return False
        return any(k.startswith("cwr_") and k not in _METADATA_KEYS for k in obj.keys())

    def draw(self, context):
        layout = self.layout
        obj = _get_active_cwr_mesh(context)
        if not obj:
            return

        props = [(k, obj[k]) for k in obj.keys() if k.startswith("cwr_") and k not in _METADATA_KEYS]
        layout.label(text=f"{len(props)} properties", icon="PROPERTIES")
        layout.separator()

        for key, val in sorted(props, key=lambda x: x[0]):
            name = key[4:]  # strip cwr_ prefix
            row = layout.row(align=True)
            row.label(text=name, icon="DOT")
            row.label(text=str(val))


class POSEIDON_PT_textures(bpy.types.Panel):
    bl_label = "Textures"
    bl_idname = "POSEIDON_PT_textures"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 6
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        obj = _get_active_cwr_mesh(context)
        return obj is not None and len(obj.material_slots) > 0

    def draw(self, context):
        layout = self.layout
        obj = _get_active_cwr_mesh(context)
        if not obj:
            return

        mesh = obj.data

        mat_face_counts = {}
        for poly in mesh.polygons:
            mi = poly.material_index
            mat_face_counts[mi] = mat_face_counts.get(mi, 0) + 1

        layout.label(text=f"{len(obj.material_slots)} materials", icon="MATERIAL")
        layout.separator()

        for i, slot in enumerate(obj.material_slots):
            mat = slot.material
            if not mat:
                continue

            box = layout.box()
            face_count = mat_face_counts.get(i, 0)
            row = box.row(align=True)

            tex_img = None
            tex_dims = ""
            if mat.use_nodes:
                for node in mat.node_tree.nodes:
                    if node.type == "TEX_IMAGE" and node.image:
                        tex_img = node.image
                        tex_dims = f"{int(tex_img.size[0])}x{int(tex_img.size[1])}"
                        break

            if tex_img:
                preview = tex_img.preview_ensure()
                icon_id = preview.icon_id if preview else 0
                if icon_id:
                    row.template_icon(icon_value=icon_id, scale=2.0)

            col = row.column(align=True)
            col.label(text=mat.name, icon="MATERIAL" if not tex_img else "NONE")
            info = f"{face_count} faces"
            if tex_dims:
                info += f" | {tex_dims}"

            cwr_path = mat.get("cwr_texture", "")
            if cwr_path:
                col.label(text=cwr_path)
            col.label(text=info)

            row2 = box.row(align=True)
            op = row2.operator(POSEIDON_OT_activate_material.bl_idname, text="Activate", icon="MATERIAL")
            op.mat_index = i
            op = row2.operator(
                POSEIDON_OT_select_material_faces.bl_idname, text="Select Faces", icon="RESTRICT_SELECT_OFF"
            )
            op.mat_index = i


class POSEIDON_UL_rtm_list(bpy.types.UIList):
    bl_idname = "POSEIDON_UL_rtm_list"

    def draw_item(self, context, layout, data, item, icon, active_data, active_property, index):
        if self.layout_type in {"DEFAULT", "COMPACT"}:
            row = layout.row(align=True)
            if item.bone_count == 0:
                ic = "FILE"
            elif item.match_count == item.bone_count:
                ic = "CHECKMARK"
            elif item.match_count > 0:
                ic = "ERROR"
            else:
                ic = "CANCEL"
            row.label(text=item.name, icon=ic)
            sub = row.row(align=True)
            sub.scale_x = 0.5
            sub.label(text=f"{item.match_count}/{item.bone_count}")
            sub = row.row(align=True)
            sub.scale_x = 0.35
            sub.label(text=f"{item.phase_count}p")
        else:
            layout.alignment = "CENTER"
            layout.label(text=item.name, icon="ANIM_DATA")

    def filter_items(self, context, data, propname):
        items = getattr(data, propname)
        n = len(items)
        flt_flags = [self.bitflag_filter_item] * n

        needle = context.scene.poseidon.vfs_rtm_search.lower() if hasattr(context.scene, "poseidon") else ""
        if needle:
            for i, item in enumerate(items):
                if needle not in item.vfs_path.lower():
                    flt_flags[i] = 0

        sorted_pairs = sorted(enumerate(items), key=lambda x: x[1].vfs_path.lower())
        flt_neworder = [p[0] for p in sorted_pairs]

        return flt_flags, flt_neworder


class POSEIDON_PT_animations(bpy.types.Panel):
    bl_label = "Animations"
    bl_idname = "POSEIDON_PT_animations"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Poseidon"
    bl_order = 7
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        props = context.scene.poseidon
        return hasattr(props, "game_data_dir") and props.game_data_dir

    def draw(self, context):
        layout = self.layout
        props = context.scene.poseidon

        row = layout.row(align=True)
        row.operator(POSEIDON_OT_scan_animations.bl_idname, text="Scan RTM", icon="ANIM_DATA")
        row.operator(POSEIDON_OT_refresh_animation_match.bl_idname, text="", icon="FILE_REFRESH")

        if len(props.vfs_rtm_list) == 0:
            layout.label(text="No animations scanned yet", icon="INFO")
            return

        obj = _get_active_cwr_mesh(context)
        if obj:
            layout.label(text=f"Matching: {obj.name}", icon="MESH_DATA")
        else:
            layout.label(text="No CWR mesh selected", icon="ERROR")

        layout.label(text=f"{len(props.vfs_rtm_list)} animations found", icon="ANIM_DATA")
        layout.prop(props, "vfs_rtm_search", text="", icon="VIEWZOOM")
        layout.template_list(
            POSEIDON_UL_rtm_list.bl_idname,
            "",
            props,
            "vfs_rtm_list",
            props,
            "vfs_rtm_index",
            rows=10,
        )
        idx = props.vfs_rtm_index
        if 0 <= idx < len(props.vfs_rtm_list):
            item = props.vfs_rtm_list[idx]
            box = layout.box()
            box.label(text=item.vfs_path, icon="FILE")
            row = box.row()
            row.label(text=f"Bones: {item.bone_count}")
            row.label(text=f"Phases: {item.phase_count}")
            row = box.row()
            row.label(text=f"Matched: {item.match_count}/{item.bone_count}")
            if item.bone_names:
                bones = item.bone_names.split(",")
                vgroups = set()
                if obj:
                    vgroups = {vg.name.lower() for vg in obj.vertex_groups}

                col = box.column(align=True)
                for bname in bones:
                    r = col.row(align=True)
                    matched = bname.lower() in vgroups
                    r.label(text=bname, icon="CHECKMARK" if matched else "CANCEL")

            layout.operator(POSEIDON_OT_load_animation.bl_idname, text="Load Animation", icon="ARMATURE_DATA")
            layout.operator("poseidon.rtm_to_rig", text="Load onto Soldier Rig (editable)", icon="POSE_HLT")
