# Blender rig for the CWA MC soldier: author swim/dive moves as ordinary Blender actions and export them as RTMs.
# Run inside Blender (Blender 4.2+; made on 5.3), e.g. from the Scripting tab or the Blender MCP:
#
#   import runpy; rig = runpy.run_path(r'<repo>\scripts\swim\blender_swim_rig.py')
#   rig['build'](mesh=r'<extracted Data3D.pbo>\mc vojakw2.p3d', dll=r'<repo>\dist\x64-win-rwdi\PoseidonFormats.dll')
#   rig['import_rtm'](r'<repo>\content\default-packs\swim\rtm\tread.rtm')      # an existing move as an action
#   rig['export_rtm']('swimunder', r'<out>\swimunder.rtm', phases=41, step=(0, 0, 0))
#
# The mesh is optional (the game's model, not in this repo; it is only a preview skinned by its named selections).
# The armature is built from mc_joints.json (fit_mc_joints.py).
#
# Spaces. The MC rest skeleton: x forward, y up, z left (pelvis at the origin, feet at y -1.06). The game's model
# space, where an RTM puts the posed bones: x left, y up, forward -z. In Blender the soldier faces -Y with his left
# at +X, Z up, in both, so an RTM bone matrix D (rest -> game) is Pg^-1 . Dbl . Pr, where Dbl is the bone's
# deformation in Blender (pose matrix times the inverse rest matrix, the armature object left at identity).
#
# Bone axes, for posing by hand: every bone's local Z points to the soldier's front (the feet: up), so a positive
# rotation about local X flexes -- the spine bends forward, an arm or a thigh swings forward, a forearm folds up,
# the toes come up -- and a knee bends with a negative X. Local Y runs along the bone.
import json
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rtm_io import mat4, read_rtm, unmat, write_rtm  # noqa: E402

ARM = 'MC_Rig'
MESH = 'MC_Soldier'
# the order and set of bones the swim RTMs carry (tread.rtm)
RTM_BONES = ['pzadek', 'lzadek', 'bricho', 'zebra', 'hrudnik', 'krk', 'hlava', 'prameno', 'lrameno', 'pbiceps',
             'lbiceps', 'ploket', 'lloket', 'pruka', 'lruka', 'pstehno', 'lstehno', 'pholen', 'lholen', 'pchodidlo',
             'lchodidlo', 'pprsty', 'lprsty', 'zbran', 'roura']
Pr = Matrix(((0, 0, 1, 0), (-1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1)))  # rest skeleton -> Blender
Pg = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))  # game model space -> Blender
PrI, PgI = Pr.inverted(), Pg.inverted()


def _joints():
    with open(os.path.join(HERE, 'mc_joints.json')) as f:
        d = json.load(f)
    J = {c: Vector(p) for c, p in d['joints'].items()}
    return d['parent'], J


def _bl(p):
    return (Pr @ Vector((p[0], p[1], p[2], 1.0))).xyz


def build(mesh=None, dll=None, rest_pose=True):
    """Make the armature MC_Rig (and, given the model, the skinned preview mesh MC_Soldier)."""
    parent, J = _joints()
    if ARM in bpy.data.objects:
        bpy.data.objects.remove(bpy.data.objects[ARM])
    arm = bpy.data.armatures.new(ARM)
    ob = bpy.data.objects.new(ARM, arm)
    bpy.context.scene.collection.objects.link(ob)
    ob.show_in_front = True
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.edit_bones
    children = {}
    for c, p in parent.items():
        children.setdefault(p, []).append(c)
    # heads: the joint with the parent (the pelvis's own pivot is where the spine starts)
    head = {c: _bl(J[c]) for c in parent}
    head['pzadek'] = _bl(J['bricho']) + Vector((0, 0, -0.12))
    tails = {'hlava': head['hlava'] + Vector((0, 0, 0.22)), 'pruka': None, 'lruka': None,
             'pprsty': None, 'lprsty': None, 'pzadek': _bl(J['bricho'])}
    for s in 'pl':
        tails[s + 'ruka'] = head[s + 'ruka'] + (head[s + 'ruka'] - head[s + 'loket']).normalized() * 0.16
        tails[s + 'prsty'] = head[s + 'prsty'] + Vector((0, -0.06, 0))
        tails[s + 'rameno'] = head[s + 'biceps']
        tails[s + 'stehno'] = head[s + 'holen']
    tails['hrudnik'] = head['krk']
    order = ['pzadek'] + [b for b in RTM_BONES if b in parent]
    for b in order:
        e = eb.new(b)
        e.head = head[b]
        t = tails.get(b)
        if t is None:
            kids = [k for k in children.get(b, []) if not k.endswith('rameno')]
            t = head[kids[0]] if kids else head[b] + Vector((0, 0, 0.1))
        e.tail = t
        if b != 'pzadek':
            e.parent = eb[parent[b]]
            e.use_connect = False
        e.align_roll(Vector((0, 0, 1)) if b.endswith(('chodidlo', 'prsty')) else Vector((0, -1, 0)))
    e = eb.new('lzadek'); e.head = head['pzadek']; e.tail = head['pzadek'] + Vector((0.1, 0, 0))
    e.parent = eb['pzadek']; e.align_roll(Vector((0, -1, 0)))
    # weapons: slung (on the chest) in the swim moves
    for b, at in (('zbran', Vector((0.05, -0.22, 0.25))), ('roura', Vector((0.05, 0.2, 0.25)))):
        e = eb.new(b); e.head = at; e.tail = at + Vector((0, 0, 0.3)); e.parent = eb['hrudnik']
        e.align_roll(Vector((0, -1, 0)))
    bpy.ops.object.mode_set(mode='POSE')
    for pb in ob.pose.bones:
        pb.rotation_mode = 'XYZ'
    bpy.ops.object.mode_set(mode='OBJECT')
    if mesh:
        _build_mesh(ob, mesh, dll)
    return ob


def _build_mesh(arm_ob, path, dll):
    if dll:
        os.environ['POSEIDON_FORMATS_DLL'] = dll
    addon = os.path.join(HERE, '..', '..', 'apps', 'tools', 'BlenderAddon', 'io_import_p3d')
    sys.path.insert(0, os.path.abspath(addon))
    import p3d_lib
    m = p3d_lib.P3DModel(path)
    V = np.array(m.get_vertices(0)) + np.array(m.bounding_center())  # ODOL keeps them about the bounding centre
    F = np.array(m.get_faces(0))
    W = {}
    for i in range(m.selection_count(0)):
        n = m.selection_name(0, i)
        if n in RTM_BONES:
            W[n] = m.get_selection_vertices(0, i)
    weighted = np.zeros(len(V), bool)
    for idx, _ in W.values():
        weighted[idx] = True
    # drop proxy triangles and anything not skinned: faces with a long edge or an unweighted corner
    edge = np.max([np.linalg.norm(V[F[:, a]] - V[F[:, b]], axis=1) for a, b in ((0, 1), (1, 2), (2, 0))], axis=0)
    keep = (edge < 0.35) & weighted[F].all(1)
    P = [_bl(v) for v in V]
    if MESH in bpy.data.objects:
        bpy.data.objects.remove(bpy.data.objects[MESH])
    me = bpy.data.meshes.new(MESH)
    me.from_pydata(P, [], F[keep].tolist())
    me.update()
    ob = bpy.data.objects.new(MESH, me)
    bpy.context.scene.collection.objects.link(ob)
    for n, (idx, wg) in W.items():
        vg = ob.vertex_groups.new(name=n)
        for i, w in zip(idx, wg):
            vg.add([int(i)], float(w), 'REPLACE')
    mod = ob.modifiers.new('Armature', 'ARMATURE')
    mod.object = arm_ob
    ob.parent = arm_ob
    return ob


# ---- RTM <-> action ---------------------------------------------------------------------------------------------
def _ortho(M):
    u, _, vt = np.linalg.svd(np.array(M)[:3, :3])
    R = u @ vt
    out = Matrix(np.array(M).tolist())
    for i in range(3):
        for j in range(3):
            out[i][j] = R[i, j]
    return out


def import_rtm(path, name=None, frame_step=1):
    """An RTM as the action <name> on MC_Rig, one key per phase (frame = phase index * frame_step)."""
    ob = bpy.data.objects[ARM]
    r = read_rtm(path)
    name = name or os.path.splitext(os.path.basename(path))[0]
    act = bpy.data.actions.get(name) or bpy.data.actions.new(name)
    act.use_fake_user = True
    if hasattr(act, 'fcurves'):
        act.fcurves.clear()
    ob.animation_data_create()
    ob.animation_data.action = act
    bones = ob.data.bones
    prev = {}
    for k, (t, mats) in enumerate(r['phases']):
        D = {b: Pg @ Matrix(mat4(m).tolist()) @ PrI for b, m in zip(r['names'], mats)}
        pose = {}
        for b in RTM_BONES:
            bone = bones[b]
            src = D.get(b) if b in D else (D.get(bone.parent.name) if bone.parent else None)
            pose[b] = _ortho(src) @ bone.matrix_local if src is not None else bone.matrix_local.copy()
        for b in RTM_BONES:
            bone = bones[b]; pb = ob.pose.bones[b]
            if bone.parent:
                rest = bone.parent.matrix_local.inverted() @ bone.matrix_local
                basis = rest.inverted() @ pose[bone.parent.name].inverted() @ pose[b]
            else:
                basis = bone.matrix_local.inverted() @ pose[b]
            loc, rot, _ = basis.decompose()
            eul = rot.to_euler('XYZ', prev[b]) if b in prev else rot.to_euler('XYZ')
            prev[b] = eul
            pb.location = loc
            pb.rotation_euler = eul
            f = k * frame_step
            pb.keyframe_insert('location', frame=f)
            pb.keyframe_insert('rotation_euler', frame=f)
    act['rtm_step'] = list(r['step'])
    act['rtm_phases'] = len(r['phases'])
    act.frame_range = (0, (len(r['phases']) - 1) * frame_step)
    return act


def pose_matrices():
    """{bone: RTM 12-float matrix} for the rig as posed now."""
    ob = bpy.data.objects[ARM]
    out = {}
    for b in RTM_BONES:
        pb = ob.pose.bones[b]
        Dbl = ob.matrix_world @ pb.matrix @ pb.bone.matrix_local.inverted()
        out[b] = unmat(np.array((PgI @ Dbl @ Pr)))
    return out


def export_rtm(action, path, phases=None, step=(0.0, 0.0, 0.0), frames=None):
    """Sample <action> on MC_Rig over its frame range into an RTM_0101 with <phases> evenly spaced phases (a looped
    move's last phase repeats its first, as the stock moves do)."""
    ob = bpy.data.objects[ARM]
    act = bpy.data.actions[action] if isinstance(action, str) else action
    ob.animation_data_create()
    ob.animation_data.action = act
    f0, f1 = frames or (act.frame_range[0], act.frame_range[1])
    n = phases or int(round(f1 - f0)) + 1
    sc = bpy.context.scene
    out = []
    for i in range(n):
        u = i / (n - 1)
        f = f0 + (f1 - f0) * u
        sc.frame_set(int(np.floor(f)), subframe=float(f - np.floor(f)))
        m = pose_matrices()
        out.append((u, [m[b] for b in RTM_BONES]))
    write_rtm(path, tuple(step), RTM_BONES, out)
    return n
