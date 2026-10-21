"""The CWA "MC" soldier as an editable Blender rig: build it, import RTM animations as ordinary actions, export actions
as RTMs, hold the joints to human limits, and check a move for limbs passing through each other.

Spaces. The MC rest skeleton (and the soldier model's own vertices, once the ODOL bounding centre is added back): x
forward, y up, z left, pelvis at the origin, feet at y -1.06. The game's model space, where an RTM puts the posed
bones: x left, y up, forward -z. In Blender the soldier faces -Y with his left at +X, Z up, in both; so an RTM bone
matrix D is Pg^-1 . Dbl . Pr, where Dbl is the bone's deformation in Blender (pose matrix times the inverse rest
matrix, the armature object at identity). Import and export round-trip the swim pack's tread.rtm to 1e-6.

Bone axes, for posing by hand: every bone's local Z points to the soldier's front (the feet: up), so a positive
rotation about local X flexes -- the spine bends forward, an arm or a thigh swings forward, a forearm folds up, the
toes come up -- and a knee bends with a negative X. Upper arms and thighs rotate Y (twist), then Z (out to the side),
then X (forward): with X first, an arm raised past the horizontal swung inward on a positive Z and the arms crossed.

The joint pivots (mc_joints.json) were fitted from the stock animations (scripts/swim/fit_mc_joints.py).
"""

import json
import os
import tempfile

import bpy
import numpy as np
from mathutils import Matrix, Vector

from . import p3d_lib
from .rtm_io import mat4, read_rtm, read_rtm_bytes, unmat, write_rtm

HERE = os.path.dirname(os.path.abspath(__file__))
ARM = "MC_Rig"
MESH = "MC_Soldier"
# the bones the swim RTMs carry, in their order
RTM_BONES = [
    "pzadek",
    "lzadek",
    "bricho",
    "zebra",
    "hrudnik",
    "krk",
    "hlava",
    "prameno",
    "lrameno",
    "pbiceps",
    "lbiceps",
    "ploket",
    "lloket",
    "pruka",
    "lruka",
    "pstehno",
    "lstehno",
    "pholen",
    "lholen",
    "pchodidlo",
    "lchodidlo",
    "pprsty",
    "lprsty",
    "zbran",
    "roura",
]
STOCK_SOLDIER = "data3d\\mc vojakw2.p3d"
Pr = Matrix(((0, 0, 1, 0), (-1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1)))  # rest skeleton -> Blender
Pg = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))  # game model space -> Blender
PrI, PgI = Pr.inverted(), Pg.inverted()


def rotation_mode(bone):
    return "YZX" if bone.endswith(("biceps", "stehno")) else "XYZ"


def _joints():
    with open(os.path.join(HERE, "mc_joints.json")) as f:
        d = json.load(f)
    return d["parent"], {c: Vector(p) for c, p in d["joints"].items()}


def _bl(p):
    return (Pr @ Vector((p[0], p[1], p[2], 1.0))).xyz


# ---- building ----------------------------------------------------------------------------------------------------
def build(context, model_path=None, texture_root=None):
    """The armature MC_Rig, and given the soldier's model (a .p3d file) the skinned preview mesh MC_Soldier. Textures
    come from the mounted game data (the add-on's Game Data panel), else from texture_root."""
    parent, J = _joints()
    for name in (MESH, ARM):
        if name in bpy.data.objects:
            bpy.data.objects.remove(bpy.data.objects[name])
    arm = bpy.data.armatures.new(ARM)
    ob = bpy.data.objects.new(ARM, arm)
    context.scene.collection.objects.link(ob)
    ob.show_in_front = True
    context.view_layer.objects.active = ob
    if context.object and context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm.edit_bones
    children = {}
    for c, p in parent.items():
        children.setdefault(p, []).append(c)
    head = {c: _bl(J[c]) for c in parent}
    head["pzadek"] = _bl(J["bricho"]) + Vector((0, 0, -0.12))
    tails = {"hlava": head["hlava"] + Vector((0, 0, 0.22)), "pzadek": _bl(J["bricho"]), "hrudnik": head["krk"]}
    for s in "pl":
        tails[s + "ruka"] = head[s + "ruka"] + (head[s + "ruka"] - head[s + "loket"]).normalized() * 0.16
        tails[s + "prsty"] = head[s + "prsty"] + Vector((0, -0.06, 0))
        tails[s + "rameno"] = head[s + "biceps"]
        tails[s + "stehno"] = head[s + "holen"]
    for b in ["pzadek"] + [b for b in RTM_BONES if b in parent]:
        e = eb.new(b)
        e.head = head[b]
        t = tails.get(b)
        if t is None:
            kids = [k for k in children.get(b, []) if not k.endswith("rameno")]
            t = head[kids[0]] if kids else head[b] + Vector((0, 0, 0.1))
        e.tail = t
        if b != "pzadek":
            e.parent = eb[parent[b]]
            e.use_connect = False
        e.align_roll(Vector((0, 0, 1)) if b.endswith(("chodidlo", "prsty")) else Vector((0, -1, 0)))
    e = eb.new("lzadek")
    e.head = head["pzadek"]
    e.tail = head["pzadek"] + Vector((0.1, 0, 0))
    e.parent = eb["pzadek"]
    e.align_roll(Vector((0, -1, 0)))
    for b, at in (("zbran", Vector((0.05, -0.22, 0.25))), ("roura", Vector((0.05, 0.2, 0.25)))):
        e = eb.new(b)
        e.head = at
        e.tail = at + Vector((0, 0, 0.3))
        e.parent = eb["hrudnik"]
        e.align_roll(Vector((0, -1, 0)))
    bpy.ops.object.mode_set(mode="POSE")
    for pb in ob.pose.bones:
        pb.rotation_mode = rotation_mode(pb.name)
    bpy.ops.object.mode_set(mode="OBJECT")
    add_joint_limits(ob)
    if model_path:
        _build_mesh(context, ob, model_path, texture_root)
    return ob


def model_from_game_data(vfs_path=STOCK_SOLDIER):
    """The soldier's .p3d from the mounted game data, as a temporary file (the loader takes paths)."""
    from ctypes import c_uint8

    dll = p3d_lib.get()
    if not dll.pf_vfs_mounted():
        return None
    key = vfs_path.replace("/", "\\").encode("utf-8")
    size = dll.pf_vfs_read(key, None, 0)
    if size <= 0:
        return None
    buf = (c_uint8 * size)()
    dll.pf_vfs_read(key, buf, size)
    path = os.path.join(tempfile.mkdtemp(prefix="p3d_rig_"), os.path.basename(vfs_path))
    with open(path, "wb") as f:
        f.write(bytes(buf))
    return path


def _build_mesh(context, arm_ob, path, texture_root):
    m = p3d_lib.P3DModel(path)
    V = np.array(m.get_vertices(0)) + np.array(m.bounding_center())  # ODOL keeps them about the bounding centre
    F = np.array(m.get_faces(0))
    W = {}
    for i in range(m.selection_count(0)):
        n = m.selection_name(0, i)
        if n in RTM_BONES:
            W[n] = m.get_selection_vertices(0, i)
    total = np.zeros(len(V))
    for idx, wg in W.values():
        np.add.at(total, idx, wg)
    # drop proxy triangles and anything not skinned: faces with a long edge or an unweighted corner
    edge = np.max([np.linalg.norm(V[F[:, a]] - V[F[:, b]], axis=1) for a, b in ((0, 1), (1, 2), (2, 0))], axis=0)
    keep = (edge < 0.35) & (total[F] > 0).all(1)
    kept = F[keep][:, ::-1]  # the rest -> Blender mapping is a mirror, so the winding turns round with it
    me = bpy.data.meshes.new(MESH)
    me.from_pydata([_bl(v) for v in V], [], kept.tolist())
    me.update()
    ob = bpy.data.objects.new(MESH, me)
    context.scene.collection.objects.link(ob)
    _texture(m, ob, kept, np.array(m.get_face_materials(0))[keep], texture_root)
    for n, (idx, wg) in W.items():
        vg = ob.vertex_groups.new(name=n)
        for i, w in zip(idx, wg):
            # the engine weighs a vertex's bones by byte/128 and normalises them to 1; the DLL hands out byte/255,
            # so the shares are kept and the sum made 1 (blended shoulder vertices sat ~25 cm off otherwise)
            vg.add([int(i)], float(w / total[i]), "REPLACE")
    mod = ob.modifiers.new("Armature", "ARMATURE")
    mod.object = arm_ob
    ob.parent = arm_ob
    return ob


def _texture(m, ob, faces, face_mats, root):
    uvs = m.get_uvs(0)
    uv = ob.data.uv_layers.new(name="UVMap")
    for li, loop in enumerate(ob.data.loops):
        u, v = uvs[loop.vertex_index]
        uv.data[li].uv = (u, 1.0 - v)
    dll = p3d_lib.get()
    for mi in range(m.material_count(0)):
        tex = m.material_texture(0, mi)
        name = os.path.splitext(os.path.basename(tex.replace("\\", "/")))[0] if tex else f"mat{mi}"
        mat = bpy.data.materials.get("MC_" + name) or bpy.data.materials.new("MC_" + name)
        mat["cwr_texture"] = tex
        mat.use_nodes = True
        nt = mat.node_tree
        bsdf = nt.nodes.get("Principled BSDF")
        if tex and bsdf and not any(n.type == "TEX_IMAGE" for n in nt.nodes):
            img = None
            try:
                if dll.pf_vfs_mounted():
                    img = p3d_lib.VFSImage(tex)
                elif root and os.path.isfile(os.path.join(root, tex)):
                    img = p3d_lib.PAAImage(os.path.join(root, tex))
            except Exception:  # a texture the game does not have: the face stays untextured
                img = None
            if img:
                w, h, rgba = img.width, img.height, np.frombuffer(bytes(img.get_rgba()), np.uint8)
                img.close()
                bl = bpy.data.images.new("MC_" + name, width=w, height=h, alpha=True)
                bl.pixels[:] = (rgba.reshape(h, w, 4)[::-1].astype(np.float32) / 255.0).ravel()
                bl.pack()
                node = nt.nodes.new("ShaderNodeTexImage")
                node.image = bl
                nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
                nt.links.new(node.outputs["Alpha"], bsdf.inputs["Alpha"])
        ob.data.materials.append(mat)
    for poly, mi in zip(ob.data.polygons, face_mats):
        poly.material_index = int(mi)


# ---- joint limits ------------------------------------------------------------------------------------------------
# Degrees on each bone's own axes (see the module doc): (x min, x max), (y min, y max), (z min, z max). From the game's
# own moves: the full range of every stock animation in DTA\Anim.pbo with the soldier skeleton (509) and the swim
# pack's, plus 5 -- so no game animation is refused -- widened only where a person goes further than CWA's moves ever
# do: arms overhead (upper arm to 182, the collarbones turned up and out 30 the other way) and toes pointed. Knees and
# elbows bend one way only. The hands carry no limit: the rifle grips turn them past any useful bound.
LIMITS = {
    "bricho": ((-84, 82), (-69, 37), (-99, 57)),
    "zebra": ((-42, 47), (-34, 17), (-48, 50)),
    "hrudnik": ((-44, 78), (-28, 31), (-39, 38)),
    "krk": ((-69, 97), (-78, 68), (-82, 82)),
    "hlava": ((-29, 19), (-41, 33), (-22, 14)),
    "prameno": ((-31, 52), (-63, 73), (-93, 30)),
    "lrameno": ((-48, 54), (-73, 37), (-30, 84)),
    "pbiceps": ((-79, 182), (-106, 107), (-73, 38)),
    "lbiceps": ((-73, 182), (-82, 114), (-46, 82)),
    "ploket": ((-5, 159), (-54, 75), (-88, 69)),
    "lloket": ((-8, 162), (-45, 78), (-96, 99)),
    "pstehno": ((-77, 155), (-115, 143), (-78, 36)),
    "lstehno": ((-48, 162), (-105, 82), (-34, 82)),
    "pholen": ((-172, 5), (-20, 29), (-14, 30)),
    "lholen": ((-168, 5), (-27, 28), (-36, 16)),
    "pchodidlo": ((-69, 52), (-32, 45), (-96, 44)),
    "lchodidlo": ((-60, 63), (-23, 26), (-84, 51)),
    "pprsty": ((-89, 46), (-15, 10), (-21, 12)),
    "lprsty": ((-72, 82), (-30, 12), (-15, 26)),
}


def _limit_for(bone):
    return LIMITS.get(bone)


def add_joint_limits(ob):
    import math

    for pb in ob.pose.bones:
        lim = _limit_for(pb.name)
        if lim is None:
            continue
        c = pb.constraints.get("Joint limit") or pb.constraints.new("LIMIT_ROTATION")
        c.name = "Joint limit"
        c.owner_space = "LOCAL"
        c.use_transform_limit = True  # rotating by hand stops at the limit too
        (c.min_x, c.max_x), (c.min_y, c.max_y), (c.min_z, c.max_z) = [
            (math.radians(a), math.radians(b)) for a, b in lim
        ]
        c.use_limit_x = c.use_limit_y = c.use_limit_z = True


def set_joint_limits(ob, on):
    for pb in ob.pose.bones:
        c = pb.constraints.get("Joint limit")
        if c:
            c.mute = not on


def joint_limit_hits(ob, action, step=1):
    """[(frame, bone, axis, degrees)] where <action> rotates a joint past its limit (the limits themselves off)."""
    import math

    hits = []
    sc = bpy.context.scene
    ob.animation_data.action = action
    f0, f1 = int(action.frame_range[0]), int(action.frame_range[1])
    for f in range(f0, f1 + 1, step):
        sc.frame_set(f)
        for pb in ob.pose.bones:
            lim = _limit_for(pb.name)
            if lim is None:
                continue
            e = pb.matrix_basis.to_euler(pb.rotation_mode)
            for axis, val, (lo, hi) in zip("xyz", e, lim):
                d = math.degrees(val)
                if d < lo - 0.5 or d > hi + 0.5:
                    hits.append((f, pb.name, axis, round(d, 1)))
    return hits


# ---- collisions --------------------------------------------------------------------------------------------------
# limb capsules: bone -> radius (m), from the soldier model's flesh around each bone, a little under so touching is
# not a hit
RADIUS = {
    "bricho": 0.13,
    "zebra": 0.14,
    "hrudnik": 0.15,
    "krk": 0.05,
    "hlava": 0.10,
    "biceps": 0.055,
    "loket": 0.045,
    "ruka": 0.04,
    "stehno": 0.075,
    "holen": 0.055,
    "chodidlo": 0.045,
}
# pairs that touch by construction and are not checked: a bone and its parent, the trunk's own bones, and a limb
# against the trunk bone it hangs from
_TRUNK = ("bricho", "zebra", "hrudnik", "krk", "hlava")


def _capsules(ob):
    caps = {}
    M = ob.matrix_world
    for pb in ob.pose.bones:
        key = pb.name if pb.name in RADIUS else pb.name[1:] if pb.name[0] in "pl" else None
        if key not in RADIUS:
            continue
        caps[pb.name] = (M @ pb.head, M @ pb.tail, RADIUS[key])
    return caps


def _seg_dist(p1, q1, p2, q2):
    d1, d2, r = q1 - p1, q2 - p2, p1 - p2
    a, e, f = d1.dot(d1), d2.dot(d2), d2.dot(r)
    if a < 1e-9 and e < 1e-9:
        return r.length
    if a < 1e-9:
        s, t = 0.0, max(0.0, min(1.0, f / e))
    else:
        c = d1.dot(r)
        if e < 1e-9:
            t, s = 0.0, max(0.0, min(1.0, -c / a))
        else:
            b = d1.dot(d2)
            den = a * e - b * b
            s = max(0.0, min(1.0, (b * f - c * e) / den)) if den > 1e-9 else 0.0
            t = (b * s + f) / e
            if t < 0:
                t, s = 0.0, max(0.0, min(1.0, -c / a))
            elif t > 1:
                t, s = 1.0, max(0.0, min(1.0, (b - c) / a))
    return ((p1 + d1 * s) - (p2 + d2 * t)).length


def _skip(ob, a, b):
    ba, bb = ob.data.bones[a], ob.data.bones[b]
    if ba.parent == bb or bb.parent == ba:
        return True
    if a in _TRUNK and b in _TRUNK:
        return True
    for x, y in ((a, b), (b, a)):  # a limb's root against the trunk it hangs from
        if x[1:] in ("biceps", "stehno") and y in ("hrudnik", "zebra", "bricho"):
            return True
        if x[1:] == "stehno" and y[1:] == "stehno":  # the thighs meet at the crotch
            return True
        if x[1:] == "biceps" and y == "hlava":  # arms overhead lie against the head (a streamline squeezes the ears)
            return True
    return False


def collision_hits(ob, action, step=1, depth=1.0):
    """[(frame, bone a, bone b, overlap in m)] where two limb capsules sink into each other by more than <depth> of the
    thinner one's radius -- at 1, a limb's centre line reaches inside the other: passing through, not resting on it (a
    hand on a knee, an arm on a thigh in a crouch); and [(frame, "ruka" | "loket")] where the hands (or forearms) swap sides."""
    hits, cross = [], []
    sc = bpy.context.scene
    ob.animation_data.action = action
    f0, f1 = int(action.frame_range[0]), int(action.frame_range[1])
    names = None
    for f in range(f0, f1 + 1, step):
        sc.frame_set(f)
        caps = _capsules(ob)
        if names is None:
            names = sorted(caps)
            pairs = [(a, b) for i, a in enumerate(names) for b in names[i + 1 :] if not _skip(ob, a, b)]
        for a, b in pairs:
            pa, qa, ra = caps[a]
            pb_, qb, rb = caps[b]
            overlap = ra + rb - _seg_dist(pa, qa, pb_, qb)
            if overlap > depth * min(ra, rb):
                hits.append((f, a, b, round(overlap, 3)))
        M = ob.matrix_world
        pb = ob.pose.bones
        ls, rs = M @ pb["lbiceps"].head, M @ pb["pbiceps"].head
        lat = (ls - rs).normalized()
        mid = (ls + rs) / 2
        # the hands (or the forearms) swapped sides: the arms cross. One hand past the middle is not -- a rifle is
        # held across the body
        for bone in ("ruka", "loket"):
            if (M @ pb["l" + bone].tail - M @ pb["p" + bone].tail).dot(lat) < 0.0:
                cross.append((f, bone))
    return hits, cross


# ---- RTM <-> action ----------------------------------------------------------------------------------------------
def _ortho(M):
    u, _, vt = np.linalg.svd(np.array(M)[:3, :3])
    R = u @ vt
    out = Matrix(np.array(M).tolist())
    for i in range(3):
        for j in range(3):
            out[i][j] = R[i, j]
    return out


def phase_bases(ob, rtm):
    """Per phase of an RTM, {bone: (location, Matrix rotation basis)} as the rig's pose bones would carry it."""
    bones = ob.data.bones
    for _, mats in rtm["phases"]:
        D = {b: Pg @ Matrix(mat4(m).tolist()) @ PrI for b, m in zip(rtm["names"], mats)}
        pose = {}
        for b in RTM_BONES:
            bone = bones[b]
            src = D.get(b) if b in D else (D.get(bone.parent.name) if bone.parent else None)
            pose[b] = _ortho(src) @ bone.matrix_local if src is not None else bone.matrix_local.copy()
        out = {}
        for b in RTM_BONES:
            bone = bones[b]
            if bone.parent:
                rest = bone.parent.matrix_local.inverted() @ bone.matrix_local
                basis = rest.inverted() @ pose[bone.parent.name].inverted() @ pose[b]
            else:
                basis = bone.matrix_local.inverted() @ pose[b]
            out[b] = basis
        yield out


def import_rtm(ob, rtm, name):
    """An RTM (read_rtm's dict) as the action <name> on the rig, one key per phase; the joint limits are switched off
    (a stock move may go past them, and the limits would change it)."""
    set_joint_limits(ob, False)
    act = bpy.data.actions.get(name) or bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data_create()
    ob.animation_data.action = act
    prev = {}
    for k, bases in enumerate(phase_bases(ob, rtm)):
        for b in RTM_BONES:
            pb = ob.pose.bones[b]
            loc, rot, _ = bases[b].decompose()
            eul = rot.to_euler(pb.rotation_mode, prev[b]) if b in prev else rot.to_euler(pb.rotation_mode)
            prev[b] = eul
            pb.location = loc
            pb.rotation_euler = eul
            pb.keyframe_insert("location", frame=k)
            pb.keyframe_insert("rotation_euler", frame=k)
    act["rtm_step"] = list(rtm["step"])
    act["rtm_phases"] = len(rtm["phases"])
    act.frame_range = (0, max(1, len(rtm["phases"]) - 1))
    return act


def import_rtm_file(ob, path):
    return import_rtm(ob, read_rtm(path), os.path.splitext(os.path.basename(path))[0])


def import_rtm_vfs(ob, vfs_path):
    from ctypes import c_uint8

    dll = p3d_lib.get()
    key = vfs_path.replace("/", "\\").encode("utf-8")
    size = dll.pf_vfs_read(key, None, 0)
    if size <= 0:
        raise RuntimeError(f"cannot read {vfs_path} from the game data")
    buf = (c_uint8 * size)()
    dll.pf_vfs_read(key, buf, size)
    name = os.path.splitext(os.path.basename(vfs_path.replace("\\", "/")))[0]
    return import_rtm(ob, read_rtm_bytes(bytes(buf), vfs_path), name)


def pose_matrices(ob):
    out = {}
    for b in RTM_BONES:
        pb = ob.pose.bones[b]
        Dbl = ob.matrix_world @ pb.matrix @ pb.bone.matrix_local.inverted()
        out[b] = unmat(np.array(PgI @ Dbl @ Pr))
    return out


def export_rtm(ob, action, path, phases=None, step=(0.0, 0.0, 0.0)):
    """Sample <action> over its frame range into an RTM_0101 with <phases> evenly spaced phases (a looped move's last
    phase repeats its first, as the stock moves do). Returns the phase count."""
    sc = bpy.context.scene
    ob.animation_data_create()
    ob.animation_data.action = action
    f0, f1 = action.frame_range[0], action.frame_range[1]
    n = phases or round(f1 - f0) + 1
    out = []
    for i in range(n):
        u = i / (n - 1)
        f = f0 + (f1 - f0) * u
        sc.frame_set(int(np.floor(f)), subframe=float(f - np.floor(f)))
        m = pose_matrices(ob)
        out.append((u, [m[b] for b in RTM_BONES]))
    write_rtm(path, tuple(step), RTM_BONES, out)
    return n
