import os

import bmesh
import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, StringProperty
from bpy_extras.io_utils import ImportHelper
from mathutils import Matrix

from . import p3d_lib


# CWR: X=right, Y=up, Z=forward → Blender: X=right, Y=forward, Z=up
def _convert_coord(x, y, z):
    return (x, z, y)


def _convert_normal(nx, ny, nz):
    return (nx, nz, ny)


def _convert_uv(u, v):
    return (u, 1.0 - v)


def _convert_matrix(col_major_16):
    """Convert 4x4 column-major floats to Blender Matrix with axis swap."""
    m = col_major_16
    # Column-major → row-major for Matrix constructor
    mat = Matrix(
        (
            (m[0], m[4], m[8], m[12]),
            (m[1], m[5], m[9], m[13]),
            (m[2], m[6], m[10], m[14]),
            (m[3], m[7], m[11], m[15]),
        )
    )
    # Apply Y↔Z swap via basis change
    swap = Matrix(
        (
            (1, 0, 0, 0),
            (0, 0, 1, 0),
            (0, 1, 0, 0),
            (0, 0, 0, 1),
        )
    )
    return swap @ mat @ swap.inverted()


def _find_texture_file(tex_path, search_dirs):
    """Resolve CWR texture path to a real file, checking PAA/PAC variants."""
    if not tex_path:
        return None
    tex_path = tex_path.replace("\\", "/").lstrip("/")
    base, ext = os.path.splitext(tex_path)
    candidates = [tex_path]
    if ext.lower() not in (".paa", ".pac"):
        candidates.append(base + ".paa")
        candidates.append(base + ".pac")
    for search_dir in search_dirs:
        for candidate in candidates:
            full = os.path.join(search_dir, candidate)
            if os.path.isfile(full):
                return full
            parts = candidate.split("/")
            cur = search_dir
            found = True
            for part in parts:
                entries = {}
                try:
                    entries = {e.lower(): e for e in os.listdir(cur)}
                except OSError:
                    found = False
                    break
                real = entries.get(part.lower())
                if real:
                    cur = os.path.join(cur, real)
                else:
                    found = False
                    break
            if found and os.path.isfile(cur):
                return cur
    return None


def _load_paa_as_blender_image(paa_path, name, vfs_path=None):
    """Load a PAA/PAC file and create a Blender image from RGBA pixels.
    If vfs_path is set, loads from VFS instead of disk."""
    try:
        if vfs_path:
            img = p3d_lib.VFSImage(vfs_path)
        else:
            img = p3d_lib.PAAImage(paa_path)
        w, h = img.width, img.height
        rgba = img.get_rgba()
        img.close()
    except Exception:
        return None

    bl_img = bpy.data.images.new(name, width=w, height=h, alpha=True)
    pixels = [b / 255.0 for b in rgba]
    # Blender images have origin bottom-left, PAA has top-left → flip vertically
    flipped = []
    stride = w * 4
    for row in range(h - 1, -1, -1):
        flipped.extend(pixels[row * stride : (row + 1) * stride])
    bl_img.pixels[:] = flipped
    bl_img.update()
    bl_img.pack()
    return bl_img


def _create_material(name, tex_path, search_dirs):
    """Create a Blender material with texture from PAA file."""
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (300, 0)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (0, 0)
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
    bl_img = None
    real_path = _find_texture_file(tex_path, search_dirs)
    if real_path:
        bl_img = _load_paa_as_blender_image(real_path, os.path.basename(real_path))
    elif p3d_lib.vfs_mounted() and tex_path:
        # Try VFS — texture paths are relative to game root
        vfs_key = tex_path.replace("/", "\\").lstrip("\\")
        # Try original path, then .paa/.pac variants
        base, ext = os.path.splitext(vfs_key)
        candidates = [vfs_key]
        if ext.lower() not in (".paa", ".pac"):
            candidates.append(base + ".paa")
            candidates.append(base + ".pac")
        for c in candidates:
            bl_img = _load_paa_as_blender_image(None, os.path.basename(tex_path), vfs_path=c)
            if bl_img:
                print(f"[P3D] Loaded texture from VFS: {c}")
                break

    if bl_img:
        tex_node = nodes.new("ShaderNodeTexImage")
        tex_node.location = (-300, 0)
        tex_node.image = bl_img
        tex_node.select = True
        mat.node_tree.nodes.active = tex_node
        links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])
        links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
        mat.blend_method = "CLIP" if "glass" not in name.lower() else "BLEND"
    mat["cwr_texture"] = tex_path
    return mat


def _clean_faces(faces_raw, vertex_count=0):
    """Remove degenerate and duplicate faces. Returns (clean_faces, index_map, extra) where index_map[i] is the
    original face index of clean_faces[i]. A face on the same points as an earlier one but wound the other way is the
    back of a double-sided face: a Blender mesh cannot hold both on the same vertices, so the back gets copies of its
    vertices, numbered from vertex_count on; extra[k] is the original vertex copy k stands for."""
    seen = {}
    clean = []
    index_map = []
    extra = []

    def turn(f):
        i = f.index(min(f))
        return tuple(f[i:]) + tuple(f[:i])

    for orig_idx, f in enumerate(faces_raw):
        if len(f) != len(set(f)):
            continue  # degenerate: repeated vertex in same face
        key = tuple(sorted(f))
        if key in seen:
            if turn(list(f)) in seen[key]:
                continue  # an exact duplicate
            seen[key].add(turn(list(f)))
            copied = []
            for vi in f:
                copied.append(vertex_count + len(extra))
                extra.append(vi)
            clean.append(tuple(copied))
            index_map.append(orig_idx)
            continue
        seen[key] = {turn(list(f))}
        clean.append(f)
        index_map.append(orig_idx)
    return clean, index_map, extra


def import_lod(context, model, lod_idx, collection, options):
    """Import a single LOD as a Blender mesh object."""
    res = model.lod_resolution(lod_idx)
    nv = model.vertex_count(lod_idx)
    nf = model.face_count(lod_idx)

    if nv == 0:
        return None

    base_name = options["base_name"]
    lod_name = f"{base_name}_LOD{lod_idx}_{res:.0f}m"
    verts_raw = model.get_vertices(lod_idx)
    faces_raw = model.get_faces(lod_idx) if nf > 0 else []
    uvs_raw = model.get_uvs(lod_idx) if nf > 0 else []
    normals_raw = model.get_normals(lod_idx) if nf > 0 else []

    # RTM bone matrices expect vertices centered at the non-proxy mesh
    # bounding box midpoint, matching the engine's CalculateBoundingSphere path.
    face_flags_raw = model.get_face_flags(lod_idx) if nf > 0 else None
    center_offset_cwr = (0.0, 0.0, 0.0)
    if face_flags_raw and verts_raw:
        FF_PROXY_MASK = 0x30000000
        nonproxy_verts = set()
        for fi, face in enumerate(faces_raw):
            if fi < len(face_flags_raw) and not (face_flags_raw[fi] & FF_PROXY_MASK):
                nonproxy_verts.update(face)
        if nonproxy_verts:
            valid = [i for i in nonproxy_verts if 0 <= i < len(verts_raw)]
            if valid:
                xs = [verts_raw[i][0] for i in valid]
                ys = [verts_raw[i][1] for i in valid]
                zs = [verts_raw[i][2] for i in valid]
                center_offset_cwr = (
                    (min(xs) + max(xs)) / 2.0,
                    (min(ys) + max(ys)) / 2.0,
                    (min(zs) + max(zs)) / 2.0,
                )

    if center_offset_cwr != (0.0, 0.0, 0.0):
        verts_raw = [
            (x - center_offset_cwr[0], y - center_offset_cwr[1], z - center_offset_cwr[2]) for x, y, z in verts_raw
        ]
    faces, face_map, extra = _clean_faces(faces_raw, len(verts_raw))
    if extra:  # the backs of double-sided faces, on their own copies of the vertices
        verts_raw = list(verts_raw) + [verts_raw[i] for i in extra]
        uvs_raw = list(uvs_raw) + [uvs_raw[i] for i in extra] if uvs_raw else uvs_raw
        normals_raw = list(normals_raw) + [normals_raw[i] for i in extra] if normals_raw else normals_raw
    copies_of = {}
    for k, vi in enumerate(extra):
        copies_of.setdefault(vi, []).append(nv + k)
    verts = [_convert_coord(*v) for v in verts_raw]
    mesh = bpy.data.meshes.new(lod_name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    if faces and uvs_raw and any(uv != (0, 0) for uv in uvs_raw):
        uv_layer = mesh.uv_layers.new(name="UVMap")
        for poly in mesh.polygons:
            for li, vi in zip(poly.loop_indices, poly.vertices):
                if vi < len(uvs_raw):
                    u, v = _convert_uv(*uvs_raw[vi])
                    uv_layer.data[li].uv = (u, v)

    # Custom split normals — requires initialized loop data from update()
    if faces and normals_raw and options.get("import_normals", True):
        nv_mesh = len(mesh.vertices)
        if len(normals_raw) >= nv_mesh and nv_mesh > 0 and len(mesh.loops) > 0:
            normals_converted = [
                _convert_normal(*normals_raw[i])
                if abs(normals_raw[i][0]) + abs(normals_raw[i][1]) + abs(normals_raw[i][2]) > 1e-10
                else (0.0, 0.0, 1.0)
                for i in range(nv_mesh)
            ]
            try:
                mesh.normals_split_custom_set_from_vertices(normals_converted)
            except Exception as e:
                print(f"[P3D] Warning: custom normals failed on {lod_name}: {e}")
    mesh.update()
    obj = bpy.data.objects.new(lod_name, mesh)
    collection.objects.link(obj)

    # Store centering offset for reference (animation pipeline may need it)
    if center_offset_cwr != (0.0, 0.0, 0.0):
        obj["cwr_center_offset"] = center_offset_cwr
    if faces and options.get("import_materials", True):
        mat_count = model.material_count(lod_idx)
        face_mats_raw = model.get_face_materials(lod_idx)
        search_dirs = options.get("texture_dirs", [])
        mat_cache = options.get("_mat_cache", {})

        for mi in range(mat_count):
            tex = model.material_texture(lod_idx, mi)
            surf = model.material_surface(lod_idx, mi)
            mat_key = f"{tex}|{surf}"
            if mat_key in mat_cache:
                mat = mat_cache[mat_key]
            else:
                mat_name = os.path.splitext(os.path.basename(tex))[0] if tex else f"Material_{mi}"
                mat = _create_material(mat_name, tex, search_dirs)
                mat["cwr_surface"] = surf
                mat_cache[mat_key] = mat
            obj.data.materials.append(mat)

        for clean_fi, orig_fi in enumerate(face_map):
            if clean_fi < len(mesh.polygons) and orig_fi < len(face_mats_raw):
                mi = face_mats_raw[orig_fi]
                if mi < len(obj.data.materials):
                    mesh.polygons[clean_fi].material_index = mi

    # Face flags → smooth/flat shading, backface culling, custom attribute
    # (face_flags_raw was loaded earlier for auto-centering)
    if faces and face_flags_raw:
        FF_FLATLIGHT = 0x200000
        FF_BOTHSIDES = 0x0020
        FF_NOSHADOW = 0x0010
        FF_NOLIGHT = 0x0001
        clean_flags = [face_flags_raw[orig_fi] for orig_fi in face_map if orig_fi < len(face_flags_raw)]
        for fi, ff in enumerate(clean_flags):
            if fi < len(mesh.polygons):
                mesh.polygons[fi].use_smooth = not (ff & FF_FLATLIGHT)
        # BothSidesLight → disable backface culling on affected materials
        if options.get("import_materials", True):
            bothsides_mats = set()
            face_mats_raw = model.get_face_materials(lod_idx) if faces else []
            for clean_fi, orig_fi in enumerate(face_map):
                if orig_fi < len(face_flags_raw) and orig_fi < len(face_mats_raw):
                    if face_flags_raw[orig_fi] & FF_BOTHSIDES:
                        mi = face_mats_raw[orig_fi]
                        if mi < len(obj.data.materials):
                            bothsides_mats.add(mi)
            for mi in bothsides_mats:
                obj.data.materials[mi].use_backface_culling = False
        if any(ff & FF_NOSHADOW for ff in clean_flags):
            obj["cwr_has_noshadow_faces"] = True
        nolight_count = sum(1 for ff in clean_flags if ff & FF_NOLIGHT)
        if nolight_count > 0:
            obj["cwr_nolight_face_count"] = nolight_count
        attr = mesh.attributes.new(name="cwr_face_flags", type="INT", domain="FACE")
        for fi, ff in enumerate(clean_flags):
            if fi < len(attr.data):
                attr.data[fi].value = ff

    # Named selections → vertex groups (also collect proxy selection geometry)
    proxy_sel_data = []  # (sel_name, vertex_indices) for proxy:* selections
    sel_sets = {}  # every other selection's points, to record which ones a proxy belongs to (export puts it back)
    if options.get("import_selections", True):
        sel_count = model.selection_count(lod_idx)
        for si in range(sel_count):
            sel_name = model.selection_name(lod_idx, si)
            if not sel_name:
                sel_name = f"Selection_{si}"
            indices, weights = model.get_selection_vertices(lod_idx, si)
            if indices and not sel_name.startswith("proxy:"):
                sel_sets[sel_name] = set(indices)
            if indices:
                vg = obj.vertex_groups.new(name=sel_name)
                for idx, wgt in zip(indices, weights):
                    if 0 <= idx < nv:
                        vg.add([idx, *copies_of.get(idx, [])], wgt, "REPLACE")
                # Store centroid before proxy stripping removes vertices
                if not sel_name.startswith("proxy:"):
                    valid = [i for i in indices if 0 <= i < nv]
                    if valid:
                        cx = sum(verts[i][0] for i in valid) / len(valid)
                        cy = sum(verts[i][1] for i in valid) / len(valid)
                        cz = sum(verts[i][2] for i in valid) / len(valid)
                        obj[f"cwr_centroid_{sel_name}"] = (cx, cy, cz)
            if sel_name.startswith("proxy:") and indices:
                proxy_sel_data.append((sel_name, list(indices)))
    else:
        # Always scan for proxy geometry to strip proxy faces from the main mesh
        sel_count = model.selection_count(lod_idx)
        for si in range(sel_count):
            sel_name = model.selection_name(lod_idx, si)
            if sel_name and sel_name.startswith("proxy:"):
                indices, _ = model.get_selection_vertices(lod_idx, si)
                if indices:
                    proxy_sel_data.append((sel_name, list(indices)))
    if options.get("import_proxies", True):
        prx_count = model.proxy_count(lod_idx)
        if prx_count > 0:
            prx_col = bpy.data.collections.new(f"{lod_name}_Proxies")
            collection.children.link(prx_col)
            for pi in range(prx_count):
                prx_path = model.proxy_path(lod_idx, pi)
                prx_name = os.path.basename(prx_path.replace("\\", "/")) if prx_path else f"proxy_{pi}"

                # Build proxy mesh from selection geometry (proxy:* selections match proxy order)
                prx_obj = None
                if pi < len(proxy_sel_data):
                    _, v_indices = proxy_sel_data[pi]
                    prx_verts = [_convert_coord(*verts_raw[i]) for i in v_indices]
                    prx_faces = [(0, 1, 2)] if len(v_indices) >= 3 else []
                    prx_mesh = bpy.data.meshes.new(prx_name)
                    prx_mesh.from_pydata(prx_verts, [], prx_faces)
                    if uvs_raw and prx_faces:
                        prx_uv = prx_mesh.uv_layers.new(name="UVMap")
                        for li, lvi in zip(prx_mesh.polygons[0].loop_indices, range(len(v_indices))):
                            u, v = _convert_uv(*uvs_raw[v_indices[lvi]])
                            prx_uv.data[li].uv = (u, v)
                    prx_mesh.update()
                    prx_obj = bpy.data.objects.new(prx_name, prx_mesh)
                else:
                    # Fallback: empty object if no matching selection
                    prx_obj = bpy.data.objects.new(prx_name, None)
                    prx_obj.empty_display_type = "ARROWS"
                    prx_obj.empty_display_size = 0.25
                    mat16 = model.proxy_transform(lod_idx, pi)
                    prx_obj.matrix_world = _convert_matrix(mat16)

                prx_obj["cwr_proxy_path"] = prx_path
                if pi < len(proxy_sel_data):
                    # the selections the proxy's triangle is in (a rifle proxy is in its bone's, so it follows the
                    # hands): export_p3d.py writes them back
                    tri = set(proxy_sel_data[pi][1])
                    prx_obj["cwr_proxy_selections"] = [n for n, pts in sel_sets.items() if tri and tri <= pts]
                    # and per corner, in the mesh's vertex order (a hit zone can hold one corner of a proxy)
                    prx_obj["cwr_proxy_corner_selections"] = [
                        "|".join(n for n, pts in sel_sets.items() if vi in pts) for vi in proxy_sel_data[pi][1]
                    ]
                prx_col.objects.link(prx_obj)
                prx_obj.parent = obj

    # Always strip proxy faces from the main mesh — proxy attachment triangles
    # have vertices at extreme positions that cause spike artifacts during
    # skeletal animation, regardless of whether proxy objects are imported.
    if proxy_sel_data:
        proxy_vert_sets = [set(idx) for _, idx in proxy_sel_data]
        bm = bmesh.new()
        bm.from_mesh(mesh)
        bm.faces.ensure_lookup_table()
        to_del = []
        for face in bm.faces:
            fv = set(v.index for v in face.verts)
            if any(fv.issubset(pvs) for pvs in proxy_vert_sets):
                to_del.append(face)
        if to_del:
            bmesh.ops.delete(bm, geom=to_del, context="FACES")
        bm.to_mesh(mesh)
        bm.free()
        mesh.update()
    if options.get("import_properties", True):
        prop_count = model.property_count(lod_idx)
        for pi in range(prop_count):
            pname = model.property_name(lod_idx, pi)
            pvalue = model.property_value(lod_idx, pi)
            if pname:
                obj[f"cwr_{pname}"] = pvalue
    obj["cwr_lod_index"] = lod_idx
    obj["cwr_lod_resolution"] = res
    # a binarized (ODOL) model keeps its points about its bounding centre: export_p3d.py adds it back
    obj["cwr_bounding_center"] = tuple(model.bounding_center())
    obj["cwr_format"] = model.format

    return obj


SPEC_LOD = 1e15


def _lod_category(res):
    """Return (group_name, lod_label) based on resolution."""
    if res < 1000:
        return ("Visual", f"{res:.0f}m")
    if abs(res - 1000) < 1:
        return ("Cockpit", "ViewGunner")
    if abs(res - 1100) < 1:
        return ("Cockpit", "ViewPilot")
    if abs(res - 1200) < 1:
        return ("Cockpit", "ViewCargo")
    if abs(res - 1e13) < 1e9:
        return ("Special", "Geometry")
    if abs(res - SPEC_LOD * 1) < 1e11:
        return ("Special", "Memory")
    if abs(res - SPEC_LOD * 2) < 1e11:
        return ("Special", "LandContact")
    if abs(res - SPEC_LOD * 3) < 1e11:
        return ("Special", "Roadway")
    if abs(res - SPEC_LOD * 4) < 1e11:
        return ("Special", "Paths")
    if abs(res - SPEC_LOD * 5) < 1e11:
        return ("Special", "HitPoints")
    if abs(res - SPEC_LOD * 6) < 1e11:
        return ("Special", "ViewGeometry")
    if abs(res - SPEC_LOD * 7) < 1e11:
        return ("Special", "FireGeometry")
    if abs(res - SPEC_LOD * 8) < 1e11:
        return ("Special", "ViewCargoGeometry")
    if abs(res - SPEC_LOD * 10) < 1e11:
        return ("Cockpit", "ViewCommander")
    if abs(res - SPEC_LOD * 11) < 1e11:
        return ("Special", "ViewCommanderGeometry")
    if abs(res - SPEC_LOD * 13) < 1e11:
        return ("Special", "ViewPilotGeometry")
    if abs(res - SPEC_LOD * 15) < 1e11:
        return ("Special", "ViewGunnerGeometry")
    if abs(res - SPEC_LOD * 16) < 1e11:
        return ("Special", "FireGunnerGeometry")
    return ("Special", f"{res:.0e}")


def do_import(context, filepath, options):
    """Main import entry point."""
    p3d_lib.load()

    model = p3d_lib.P3DModel(filepath)
    base_name = os.path.splitext(os.path.basename(filepath))[0]
    options["base_name"] = base_name
    options["_mat_cache"] = {}
    tex_dirs = []
    model_dir = os.path.dirname(filepath)
    if model_dir:
        tex_dirs.append(model_dir)
    # Walk up to find game root (look for "dta" or "Addons" folder)
    parent = model_dir
    for _ in range(5):
        parent = os.path.dirname(parent)
        if not parent or parent == os.path.dirname(parent):
            break
        if os.path.isdir(os.path.join(parent, "dta")) or os.path.isdir(os.path.join(parent, "Addons")):
            tex_dirs.append(parent)
            break
    # Game data dir from options (operator override or scene property)
    game_dir = options.get("game_data_dir", "")
    if game_dir and os.path.isdir(game_dir):
        tex_dirs.append(game_dir)
        # Mount VFS if not already mounted
        if not p3d_lib.vfs_mounted():
            pbo_count = p3d_lib.vfs_mount(game_dir)
            if pbo_count > 0:
                print(f"[P3D] VFS: mounted {pbo_count} PBOs from {game_dir}")
    options["texture_dirs"] = tex_dirs
    if tex_dirs:
        print(f"[P3D] Texture search dirs: {tex_dirs}")
    root_col = bpy.data.collections.new(base_name)
    context.scene.collection.children.link(root_col)

    lod_count = model.lod_count
    lod_filter = options.get("lod_filter", "ALL")
    max_res = options.get("max_lod_resolution", 1e30)
    print(f"[P3D] Importing {base_name}: {lod_count} LODs, filter={lod_filter}")

    group_cols = {}  # category name → collection
    imported = 0
    for li in range(lod_count):
        res = model.lod_resolution(li)
        nv = model.vertex_count(li)
        nf = model.face_count(li)
        if lod_filter == "FIRST" and li > 0:
            break
        if lod_filter == "VISUAL" and res > 10000:
            continue
        if res > max_res:
            continue

        if lod_count > 1 and lod_filter == "ALL":
            group, label = _lod_category(res)
            if group not in group_cols:
                group_cols[group] = bpy.data.collections.new(group)
                root_col.children.link(group_cols[group])
            lod_col = bpy.data.collections.new(label)
            group_cols[group].children.link(lod_col)
        else:
            lod_col = root_col

        obj = import_lod(context, model, li, lod_col, options)
        if obj:
            imported += 1
            print(
                f"[P3D]   LOD{li}: {group if lod_count > 1 and lod_filter == 'ALL' else '-'}/{label if lod_count > 1 and lod_filter == 'ALL' else '-'} V={nv} F={nf} ✓"
            )
        else:
            print(f"[P3D]   LOD{li}: V={nv} F={nf} (skipped, empty)")
    view_layer = context.view_layer
    root_lc = None
    for lc in view_layer.layer_collection.children:
        if lc.collection == root_col:
            root_lc = lc
            break

    if root_lc:
        # Multi-LOD ALL: show only Visual group, first LOD
        if lod_filter == "ALL" and lod_count > 1:
            for child_lc in root_lc.children:
                if child_lc.collection.name != "Visual":
                    child_lc.exclude = True
                else:
                    for vi, vlod in enumerate(child_lc.children):
                        if vi > 0:
                            vlod.exclude = True

        # Always hide proxy sub-collections in entire tree
        def _exclude_proxies(lc):
            for child in lc.children:
                if child.collection.name.endswith("_Proxies"):
                    child.exclude = True
                _exclude_proxies(child)

        _exclude_proxies(root_lc)

    model.close()

    return {"FINISHED"} if imported > 0 else {"CANCELLED"}


class ImportP3D(bpy.types.Operator, ImportHelper):
    """Import Poseidon P3D Model"""

    bl_idname = "import_scene.p3d"
    bl_label = "Import P3D"
    bl_options = {"REGISTER", "UNDO", "PRESET"}

    filename_ext = ".p3d"
    filter_glob: StringProperty(default="*.p3d", options={"HIDDEN"})

    lod_filter: EnumProperty(
        name="LODs",
        items=[
            ("ALL", "All LODs", "Import all level-of-detail meshes"),
            ("FIRST", "First LOD Only", "Import only the highest detail LOD"),
            ("VISUAL", "Visual Only", "Skip shadow/geometry LODs (res > 10000)"),
        ],
        default="ALL",
    )
    max_lod_resolution: FloatProperty(
        name="Max LOD Resolution",
        description="Skip LODs with resolution above this value",
        default=1e30,
        min=0.0,
        max=1e30,
    )
    import_materials: BoolProperty(name="Materials", default=True)
    import_normals: BoolProperty(name="Normals", default=True)
    import_selections: BoolProperty(name="Named Selections", default=True, description="Import as vertex groups")
    import_proxies: BoolProperty(name="Proxies", default=True, description="Import as empty objects")
    import_properties: BoolProperty(name="Properties", default=True, description="Import as custom properties")
    game_data_dir: StringProperty(
        name="Game Data Directory",
        description="CWR game data root (overrides scene setting for this import)",
        subtype="DIR_PATH",
        default="",
    )

    def execute(self, context):
        game_dir = self.game_data_dir
        if not game_dir and hasattr(context.scene, "poseidon"):
            game_dir = bpy.path.abspath(context.scene.poseidon.game_data_dir)
        options = {
            "lod_filter": self.lod_filter,
            "max_lod_resolution": self.max_lod_resolution,
            "import_materials": self.import_materials,
            "import_normals": self.import_normals,
            "import_selections": self.import_selections,
            "import_proxies": self.import_proxies,
            "import_properties": self.import_properties,
            "game_data_dir": game_dir,
        }
        return do_import(context, self.filepath, options)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "lod_filter")
        if self.lod_filter != "FIRST":
            layout.prop(self, "max_lod_resolution")
        layout.separator()
        layout.label(text="Import:")
        col = layout.column(align=True)
        col.prop(self, "import_materials")
        col.prop(self, "import_normals")
        col.prop(self, "import_selections")
        col.prop(self, "import_proxies")
        col.prop(self, "import_properties")
        layout.separator()
        layout.prop(self, "game_data_dir")
