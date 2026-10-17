"""Export Blender meshes as an MLOD P3D (the editable source format) through the engine's own MLODWriter.

Each exported mesh object is one LOD. What the importer (import_p3d.py) stores is read back:
  * cwr_lod_resolution  -- the LOD's resolution (viewing distance, or a special-LOD value such as 1e13 for Geometry);
                           objects without it get 1.0, 2.0, ... in name order
  * cwr_center_offset, cwr_bounding_center -- added back to every point (the importer's centring, and the bounding
                           centre a binarized source keeps its points about), so a model is written where it came from
  * cwr_<name>          -- named properties (the importer's own bookkeeping keys are left out)
  * material cwr_texture -- the in-game texture path of each face
  * vertex groups        -- named selections (weights as O2 stores them: 1 fully selected)
  * child objects with cwr_proxy_path -- proxies, written back as the marker triangle and its "proxy:" selection
    (plus the selections its corners belonged to -- cwr_proxy_corner_selections, else cwr_proxy_selections -- so a
    rifle proxy still follows its bone)

The model's origin is each LOD object's origin: points are taken in object space with the object's rotation and
scale applied, not its location. Faces are written as triangles and quads (n-gons are split). The UVs are the
active UV map's. Optionally each material's image is written as a PAA next to the model.
"""

import os

import bmesh
import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix, Vector

from . import p3d_lib

# importer bookkeeping on LOD objects -- not named properties of the model
_INTERNAL_KEYS = {
    "cwr_lod_index",
    "cwr_lod_resolution",
    "cwr_format",
    "cwr_center_offset",
    "cwr_bounding_center",
    "cwr_proxy_corner_selections",
    "cwr_has_noshadow_faces",
    "cwr_nolight_face_count",
    "cwr_proxy_path",
    "cwr_proxy_selections",
}


def _model_point(v, offset):
    """Blender (x, y, z) -> model space (x, y up, z): the importer's Y<->Z swap undone, its centring added back."""
    return (v[0] + offset[0], v[2] + offset[1], v[1] + offset[2])


def _model_dir(v):
    return (v[0], v[2], v[1])


def _texture_of(mat, texture_prefix, export_dir, write_paa, written):
    """The in-game texture path of a material: its cwr_texture (from the importer), else its image written as a PAA
    (when asked) under texture_prefix, else the image's file name under texture_prefix, else nothing."""
    if mat is None:
        return ""
    tex = mat.get("cwr_texture", "")
    if tex:
        return tex
    img = None
    if mat.use_nodes and mat.node_tree:
        for node in mat.node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image:
                img = node.image
                break
    if img is None:
        return ""
    stem = os.path.splitext(bpy.path.basename(img.filepath or img.name))[0] or img.name
    rel = (texture_prefix + stem + ".paa") if texture_prefix else (stem + ".paa")
    if write_paa and img.name not in written:
        written[img.name] = _write_image_paa(img, os.path.join(export_dir, stem + ".paa"))
    return rel


def _write_image_paa(img, path):
    w, h = img.size
    if w <= 0 or h <= 0:
        return f"{img.name}: empty image"
    if w & (w - 1) or h & (h - 1):
        return f"{img.name}: {w}x{h} is not a power of two (PAA needs that); not written"
    px = list(img.pixels[:])  # bottom row first, floats
    rgba = bytearray(w * h * 4)
    for y in range(h):
        src = (h - 1 - y) * w * 4
        dst = y * w * 4
        for i in range(w * 4):
            rgba[dst + i] = max(0, min(255, int(px[src + i] * 255 + 0.5)))
    try:
        p3d_lib.write_paa(rgba, w, h, path)
    except RuntimeError as e:
        return f"{img.name}: {e}"
    return None


def _lod_mesh(obj, depsgraph):
    """The object's mesh as exported: modifiers applied except armatures (a posed model would be written posed),
    n-gons split, in object space with rotation and scale applied."""
    arm_mods = [m for m in obj.modifiers if m.type == "ARMATURE" and m.show_viewport]
    for m in arm_mods:
        m.show_viewport = False
    try:
        depsgraph.update()
        ev = obj.evaluated_get(depsgraph)
        mesh = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=depsgraph)
    finally:
        for m in arm_mods:
            m.show_viewport = True
    bm = bmesh.new()
    bm.from_mesh(mesh)
    ngons = [f for f in bm.faces if len(f.verts) > 4]
    if ngons:
        bmesh.ops.triangulate(bm, faces=ngons)
    rs = obj.matrix_world.to_3x3().to_4x4()
    bm.transform(rs)
    bm.to_mesh(mesh)
    bm.free()
    return mesh


def _proxy_triangle(obj, lod_obj, offset):
    """A proxy object's marker triangle in model space, its corners in the order the engine reads them (position,
    then the shortest edge is the direction, the next the up). A mesh proxy keeps its own triangle."""
    local = lod_obj.matrix_world.inverted() @ obj.matrix_world
    rs = lod_obj.matrix_world.to_3x3()
    if obj.type == "MESH" and len(obj.data.vertices) >= 3:
        pts = [rs @ (local @ v.co) for v in obj.data.vertices[:3]]
    else:
        # an empty: its axes are aside (x), direction (y in Blender = model z), up (z in Blender = model y)
        m = Matrix(rs).to_4x4() @ local
        pos = m.to_translation()
        direction = (m.to_3x3() @ Vector((0, 1, 0))).normalized()
        up = (m.to_3x3() @ Vector((0, 0, 1))).normalized()
        pts = [pos, pos + direction * 1.0, pos + up * 2.0]
    return [_model_point(p, offset) for p in pts]


def export_lod(writer, obj, resolution, depsgraph, options, report):
    # back to the model's own space: the importer's centring, and a binarized source's bounding centre
    co = obj.get("cwr_center_offset", (0.0, 0.0, 0.0))
    bc = obj.get("cwr_bounding_center", (0.0, 0.0, 0.0))
    offset = tuple(co[i] + bc[i] for i in range(3))
    mesh = _lod_mesh(obj, depsgraph)
    try:
        points = [_model_point(v.co, offset) for v in mesh.vertices]
        mesh.calc_loop_triangles()
        normals = []
        try:
            normals = [_model_dir(n.vector) for n in mesh.corner_normals]
        except AttributeError:  # Blender < 4.1
            mesh.calc_normals_split()
            normals = [_model_dir(lp.normal) for lp in mesh.loops]
        uv = mesh.uv_layers.active
        # the game's face flags (shading, both-sides lighting, shadows), kept by the importer
        flag_attr = mesh.attributes.get("cwr_face_flags")
        face_flags = [d.value for d in flag_attr.data] if flag_attr and flag_attr.domain == "FACE" else None
        export_dir = options["export_dir"]
        written = options.setdefault("_paa_written", {})
        faces = []
        for poly in mesh.polygons:
            loops = list(poly.loop_indices)
            # Blender keeps the loader's corner order (which turned the file's round); turn it back
            order = [1, 0, 2] if len(loops) == 3 else [1, 0, 3, 2]
            loops = [loops[k] for k in order]
            mat = (
                obj.material_slots[poly.material_index].material
                if poly.material_index < len(obj.material_slots)
                else None
            )
            faces.append(
                {
                    "points": [mesh.loops[li].vertex_index for li in loops],
                    "normals": loops,
                    "uvs": [((uv.data[li].uv[0], 1.0 - uv.data[li].uv[1]) if uv else (0.0, 0.0)) for li in loops],
                    "texture": _texture_of(mat, options["texture_prefix"], export_dir, options["write_paa"], written),
                    "flags": (face_flags[poly.index] & 0xFFFFFFFF)
                    if face_flags and poly.index < len(face_flags)
                    else 0,
                }
            )
        # proxies: their marker triangles go after the mesh's own points and faces
        proxy_sel = []
        counters = {}
        for child in obj.children:
            path = child.get("cwr_proxy_path")
            if not path:
                continue
            tri = _proxy_triangle(child, obj, offset)
            base = len(points)
            points.extend(tri)
            puv = [(0.0, 0.0)] * 3
            if child.type == "MESH" and child.data.uv_layers.active and child.data.polygons:
                lay = child.data.uv_layers.active.data
                by_vertex = {
                    child.data.loops[li].vertex_index: lay[li].uv for li in child.data.polygons[0].loop_indices
                }
                puv = [((by_vertex[k][0], 1.0 - by_vertex[k][1]) if k in by_vertex else (0.0, 0.0)) for k in range(3)]
            faces.append({"points": [base, base + 1, base + 2], "uvs": puv, "texture": "", "flags": 0})
            counters[path] = counters.get(path, 0) + 1
            name = "proxy:%s.%02d" % (path.lstrip("\\"), counters[path])
            corners = list(child.get("cwr_proxy_corner_selections", []))
            if child.type == "MESH" and len(corners) == 3:  # the proxy's own corners: per-corner membership
                extra = [(base + k, n) for k in range(3) for n in corners[k].split("|") if n]
            else:
                extra = [(base + k, n) for n in child.get("cwr_proxy_selections", []) for k in range(3)]
            proxy_sel.append((name, [base, base + 1, base + 2], len(faces) - 1, extra))

        lod = writer.add_lod(resolution, points, normals)
        writer.set_faces(lod, faces)

        # named selections from vertex groups; a face is in a selection when all its corners are
        groups = {vg.index: vg.name for vg in obj.vertex_groups}
        members = {name: {} for name in groups.values()}
        for v in mesh.vertices:
            for g in v.groups:
                name = groups.get(g.group)
                if name is not None:  # weight 0 is still membership (hit zones have such points): O2's smallest byte
                    members[name][v.index] = g.weight
        for _, _, _, extra in proxy_sel:
            for i, name in extra:
                members.setdefault(name, {})[i] = 1.0
        has_proxy_children = bool(proxy_sel)
        for name, pts in members.items():
            # the importer also keeps each proxy's own "proxy:" group; with the proxy objects present those are
            # written from the objects instead (else they would be written twice)
            if not pts or name.startswith("#") or (has_proxy_children and name.startswith("proxy:")):
                continue
            face_ids = [fi for fi, f in enumerate(faces) if all(p in pts for p in f["points"])]
            idx = sorted(pts)
            writer.add_selection(lod, name, idx, [p3d_lib.o2_weight(pts[i]) for i in idx], face_ids)
        for name, tri, fi, _ in proxy_sel:
            writer.add_selection(lod, name, tri, None, [fi])

        for key in obj.keys():
            if key.startswith("cwr_") and key not in _INTERNAL_KEYS and not key.startswith("cwr_centroid_"):
                writer.add_property(lod, key[4:], str(obj[key]))
        report["lods"] += 1
        report["faces"] += len(faces)
        report["proxies"] += len(proxy_sel)
    finally:
        bpy.data.meshes.remove(mesh)


def export_p3d(context, filepath, objects, texture_prefix="", write_paa=False):
    """Write <objects> (mesh objects, one per LOD) to <filepath> as an MLOD P3D. Returns a report dict."""
    if not p3d_lib.can_write():
        raise RuntimeError("This PoseidonFormats.dll cannot write models; build a newer one (PoseidonFormatsDLL)")
    lods = [o for o in objects if o.type == "MESH" and not o.get("cwr_proxy_path")]
    if not lods:
        raise RuntimeError("Nothing to export: select the model's LOD mesh objects")
    with_res = sorted([o for o in lods if "cwr_lod_resolution" in o], key=lambda o: float(o["cwr_lod_resolution"]))
    without = sorted([o for o in lods if "cwr_lod_resolution" not in o], key=lambda o: o.name)
    used = {float(o["cwr_lod_resolution"]) for o in with_res}
    plan = [(o, float(o["cwr_lod_resolution"])) for o in with_res]
    nxt = 1.0
    for o in without:
        while nxt in used:
            nxt += 1.0
        plan.append((o, nxt))
        used.add(nxt)
    plan.sort(key=lambda p: p[1])
    options = {"texture_prefix": texture_prefix, "write_paa": write_paa, "export_dir": os.path.dirname(filepath)}
    report = {"lods": 0, "faces": 0, "proxies": 0, "paa_problems": []}
    depsgraph = context.evaluated_depsgraph_get()
    writer = p3d_lib.MLODWriter()
    try:
        for obj, res in plan:
            export_lod(writer, obj, res, depsgraph, options, report)
        writer.write(filepath)
    finally:
        writer.close()
    report["paa_problems"] = [p for p in options.get("_paa_written", {}).values() if p]
    report["paa_written"] = len(options.get("_paa_written", {})) - len(report["paa_problems"])
    return report


class ExportP3D(bpy.types.Operator, ExportHelper):
    """Export the selected mesh objects (one per LOD) as an MLOD P3D model"""

    bl_idname = "export_scene.poseidon_p3d"
    bl_label = "Export Poseidon P3D"
    bl_options = {"REGISTER", "PRESET"}

    filename_ext = ".p3d"
    filter_glob: StringProperty(default="*.p3d", options={"HIDDEN"})
    selected_only: BoolProperty(
        name="Selected Only",
        description="Export the selected mesh objects (else every mesh in the scene)",
        default=True,
    )
    texture_prefix: StringProperty(
        name="Texture Prefix",
        description="In-game folder for textures that came from images rather than the game, e.g. mymod\\data\\",
        default="",
    )
    write_paa: BoolProperty(
        name="Write Textures as PAA",
        description="Write each material image (power-of-two sizes) as a PAA next to the model",
        default=False,
    )

    def execute(self, context):
        objs = context.selected_objects if self.selected_only else context.scene.objects
        try:
            rep = export_p3d(context, self.filepath, list(objs), self.texture_prefix, self.write_paa)
        except RuntimeError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for p in rep["paa_problems"]:
            self.report({"WARNING"}, p)
        self.report(
            {"INFO"},
            f"Wrote {os.path.basename(self.filepath)}: {rep['lods']} LODs, {rep['faces']} faces, "
            f"{rep['proxies']} proxies, {rep['paa_written']} PAA textures",
        )
        return {"FINISHED"}
