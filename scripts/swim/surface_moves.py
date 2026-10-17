# Surface swimming: the breaststroke, keyed as Blender actions on the MC rig with dive_moves.py's tools and exported as
# RTMs. Made by Claude in Blender, 2026-10-17 (owner: "improve the swimming animation ... that the player is swimming
# a correct breaststroke"; the diving moves stay as they are). Run inside Blender after rig['build'](...) and
# rig['import_rtm'](<tread.rtm>, 'tread') (the start and the stop join treading exactly):
#
#   sm = runpy.run_path(r'<repo>\scripts\swim\surface_moves.py')
#   sm['make_all']()
#   sm['export_all'](r'<repo>\content\default-packs\swim\rtm')
#
# The stroke, heads-up as a soldier with a slung rifle swims it (the face stays out of the water, the body ~20 deg off
# flat instead of the old ~30 deg crouch):
#   glide      arms straight ahead together, legs extended and together, toes pointed
#   outsweep   the hands part, palms out, to beyond the shoulders
#   insweep    elbows high, the hands sweep down and in under the chest; the shoulders rise -- the breath
#   recovery   the hands meet under the chin and shoot forward; the heels draw up toward the seat, knees about hip
#              width, feet turned out
#   kick       the whip kick: the feet sweep out and back and snap together, the body surges into the glide
# swim (straight, 1.5 m a stroke), swimlf / swimrf (the same stroke angled 25 deg toward the side, as before),
# swimstart (treading -> the stroke's glide) and swimstop (the glide -> treading). Steps and phase counts are the old
# moves' (config.cpp times them), so the speeds and the engine's tuning stay as they were.
import math
import os
import runpy

HERE = os.path.dirname(os.path.abspath(__file__))
dm = runpy.run_path(os.path.join(HERE, 'dive_moves.py'))
P, arms, legs = dm['P'], dm['arms'], dm['legs']
bake, bones, sample, _fill, tread_pose = dm['bake'], dm['bones'], dm['sample'], dm['_fill'], dm['tread_pose']

LOOPED = {'swim': (1.43, 41, (0.0, 0.0, -1.5)),
          'swimlf': (1.6, 41, (1.06, 0.0, -1.06)),
          'swimrf': (1.6, 41, (-1.06, 0.0, -1.06)),
          'swimfast': (1.1, 41, (0.0, 0.0, -1.5))}
ONESHOT = {'swimstart': (1.83, 33, (-0.018342, 0.0, -0.098314)),
           'swimstop': (1.1, 25, (0.012962, 0.0, -0.60875))}

# head up and looking ahead throughout (neck/head negative lift the face)
GLIDE = P(pitch=70, fwd=0.32, height=1.44, neck=-30, head=-14, spine=-4,
          arm=arms(170, 3, 0, 6, -5), leg=legs(-4, 3, 0, 6, -42))


def stroke_keys(yaw=0.0):
    """One breaststroke cycle; yaw turns the body toward the side for the angled strokes."""
    keys = [
        (0.00, GLIDE),
        # outsweep: hands apart, palms out
        (0.12, P(pitch=69, fwd=0.33, height=1.44, neck=-30, head=-14, arm=arms(166, 34, -30, 10, 10),
                 leg=legs(-4, 3, 0, 6, -42))),
        # insweep with high elbows; the shoulders rise for the breath
        (0.27, P(pitch=64, fwd=0.36, height=1.49, neck=-34, head=-16, spine=-8, arm=arms(118, 54, -38, 78, 15),
                 leg=legs(4, 4, 0, 18, -38))),
        # hands meet under the chin; the heels start up
        (0.39, P(pitch=60, fwd=0.37, height=1.50, neck=-30, head=-14, spine=-10, arm=arms(74, 24, 18, 108, -10),
                 leg=legs(22, 12, 14, 72, -5))),
        # the hands shoot forward; the legs fully drawn up, knees hip width, feet out and flexed
        (0.51, P(pitch=64, fwd=0.32, height=1.47, neck=-30, head=-14, spine=-6, arm=arms(122, 6, 4, 72, -5),
                 leg=legs(42, 20, 32, 118, 24))),
        # the whip: feet sweep out and back
        (0.63, P(pitch=67, fwd=0.30, height=1.45, neck=-30, head=-14, spine=-4, arm=arms(160, 3, 0, 24, -5),
                 leg=legs(16, 34, 22, 62, 12))),
        # legs snap together; the surge into the glide
        (0.75, P(pitch=71, fwd=0.35, height=1.44, neck=-30, head=-14, spine=-4, arm=arms(170, 2, 0, 6, -5),
                 leg=legs(-4, 5, 0, 8, -40))),
        (0.88, P(pitch=71, fwd=0.34, height=1.44, neck=-30, head=-14, spine=-4, arm=arms(170, 3, 0, 6, -5),
                 leg=legs(-4, 3, 0, 6, -42))),
    ]
    if yaw:
        for _, p in keys:
            p['yaw'] = yaw
    return keys


def start_keys():
    """From treading into the stroke's glide: lean onto the water, reach forward, one kick."""
    return [
        (0.00, P(pitch=18, fwd=0.0, height=0.97, neck=-4, head=-2, arm=arms(45, 40, 10, 70, 0), leg=legs(40, 22, 10, 80, -5))),
        (0.30, P(pitch=40, fwd=0.08, height=1.14, neck=-20, head=-10, arm=arms(80, 30, 10, 100, -5), leg=legs(45, 20, 30, 115, 22))),
        (0.55, P(pitch=58, fwd=0.20, height=1.32, neck=-28, head=-14, arm=arms(140, 6, 2, 50, -5), leg=legs(16, 34, 22, 62, 12))),
        (0.80, P(pitch=68, fwd=0.30, height=1.43, neck=-30, head=-14, arm=arms(168, 3, 0, 8, -5), leg=legs(-4, 5, 0, 8, -40))),
        (1.00, GLIDE),
    ]


def stop_keys():
    """From the glide up into treading: the hands sweep down, the legs drop under him."""
    return [
        (0.00, GLIDE),
        (0.35, P(pitch=48, fwd=0.22, height=1.32, neck=-24, head=-12, arm=arms(95, 45, -20, 50, 10), leg=legs(30, 18, 10, 70, -10))),
        (0.70, P(pitch=24, fwd=0.06, height=1.05, neck=-8, head=-4, arm=arms(50, 40, 10, 70, 0), leg=legs(38, 22, 10, 80, -5))),
        (1.00, P(pitch=18, fwd=0.0, height=0.97, neck=-4, head=-2, arm=arms(45, 40, 10, 70, 0), leg=legs(40, 22, 10, 80, -5))),
    ]


def make_all():
    acts = {}
    acts['swim'] = bake('swim', stroke_keys(), LOOPED['swim'][0], True)
    acts['swimlf'] = bake('swimlf', stroke_keys(-25.0), LOOPED['swimlf'][0], True)
    acts['swimrf'] = bake('swimrf', stroke_keys(25.0), LOOPED['swimrf'][0], True)
    # baked on one frame a phase: the arms are set per frame, and a sample between frames would blend their angles
    acts['swimfast'] = crawl_sling(crawl_arms(bake('swimfast', crawl_keys(), CRAWL_FRAMES / dm['FPS'], True)))
    tread = tread_pose(0)
    glide = bones(sample(_fill(stroke_keys()), 0.0, True))  # the stroke's first pose, trailing limbs included
    acts['swimstart'] = bake('swimstart', start_keys(), ONESHOT['swimstart'][0], False,
                             join_in=(tread, 0.0, 0.2), join_out=(glide, 0.82, 1.0))
    acts['swimstop'] = bake('swimstop', stop_keys(), ONESHOT['swimstop'][0], False,
                            join_in=(glide, 0.0, 0.12), join_out=(tread, 0.65, 1.0))
    return acts


def export_all(out_dir):
    rig = runpy.run_path(os.path.join(HERE, 'blender_swim_rig.py'))
    done = {}
    for name, (_, phases, step) in {**LOOPED, **ONESHOT}.items():
        done[name] = rig['export_rtm'](name, os.path.join(out_dir, name + '.rtm'), phases=phases, step=step)
    return done


# ---- the front crawl (SwimFastF, the sprint) ----------------------------------------------------------------------
# Owner, 2026-10-17: "use the crawl animation for swimming when the player uses the running option". A heads-up front
# crawl -- the face stays out, as a soldier sprinting with his kit swims it: one arm pulls under the body while the
# other recovers over the water with the elbow high, the body rolls toward the recovering arm, a six-beat flutter
# kick. One cycle is two strokes (right, then left), 1.1 s and 1.5 m, the sprint's old timing.
# The arm channels are shoulder Euler angles, and the crawl arm turns a full circle about the shoulder: no blend of
# those angles follows it (between the arm at the thigh and the arm reaching ahead it swings the hand through the
# water, or wraps). So the arms are set on every frame from where the upper arm and forearm point (crawl_arms), on
# top of the channel bake, and exported on every frame (41 frames for 41 phases).
CRAWL_PITCH = 75
CRAWL_FRAMES = 40
# (u, upper arm, forearm plane, elbow bend) for the right arm, the directions in the body frame (x out to his right
# negative, y toward his feet, z up -- the body at CRAWL_PITCH, unrolled); the left arm mirrors it half a cycle later
CRAWL_ARM = [
    (0.00, (-0.12, -1.00, +0.05), (+0.00, -1.00, -0.30), 10),   # entry, reaching
    (0.10, (-0.15, -0.90, -0.35), (+0.00, -0.30, -1.00), 45),   # catch: the forearm drops, elbow high
    (0.20, (-0.40, -0.35, -0.85), (+0.35, +0.35, -0.85), 85),   # the pull under the chest
    (0.30, (-0.35, +0.25, -0.90), (+0.15, +0.75, -0.65), 65),
    (0.40, (-0.30, +0.75, -0.60), (-0.05, +0.95, -0.30), 30),   # the push past the hip
    (0.48, (-0.35, +0.90, -0.05), (-0.15, +0.90, +0.20), 20),   # out at the thigh
    (0.56, (-0.55, +0.50, +0.65), (-0.20, +0.80, -0.30), 75),   # the elbow lifts it out
    (0.66, (-0.70, +0.00, +0.75), (-0.20, +0.30, -0.60), 95),  # recovery: elbow high, the hand hanging
    (0.76, (-0.60, -0.45, +0.65), (+0.00, -0.60, -0.60), 90),
    (0.86, (-0.40, -0.80, +0.45), (+0.00, -0.90, -0.30), 55),
    (0.94, (-0.20, -0.97, +0.15), (+0.00, -0.97, -0.20), 25),   # the hand goes in ahead of the shoulder
]


def _crawl_at(u, mirror):
    """(upper, forearm, bend) of one arm at phase u: periodic Catmull-Rom through CRAWL_ARM."""
    from mathutils import Vector
    u %= 1.0
    n = len(CRAWL_ARM)
    for i in range(n):
        u1 = CRAWL_ARM[(i + 1) % n][0] + (1.0 if i + 1 == n else 0.0)
        if CRAWL_ARM[i][0] <= u < u1:
            break
    t = (u - CRAWL_ARM[i][0]) / (u1 - CRAWL_ARM[i][0])
    pts = [CRAWL_ARM[(i + k) % n] for k in (-1, 0, 1, 2)]
    out = []
    for field in (1, 2, 3):
        vals = [p[field] for p in pts]
        if field == 3:
            out.append(dm['_catmull'](*vals, t))
            continue
        v = Vector([dm['_catmull'](*(x[c] for x in vals), t) for c in range(3)]).normalized()
        if mirror:
            v.x = -v.x
        out.append(v)
    return out


def crawl_arms(act):
    """Set both arms of <act> on every frame from CRAWL_ARM, relative to the pelvis as it rolls."""
    import bpy
    from mathutils import Matrix, Vector
    ob = bpy.data.objects['MC_Rig']
    sc = bpy.context.scene
    ob.animation_data.action = act
    wi = ob.matrix_world.to_3x3().inverted()
    # the pelvis frame the directions are written in: the crawl's pitch, unrolled
    sc.frame_set(0)
    pel = ob.pose.bones['pzadek']
    keep = pel.rotation_euler.copy()
    pel.rotation_euler = dm['R'](CRAWL_PITCH, 0, 0)
    bpy.context.view_layer.update()
    p0inv = pel.matrix.to_3x3().inverted()
    pel.rotation_euler = keep
    last = int(act.frame_range[1])
    prev = {}
    for f in range(last + 1):
        sc.frame_set(f)
        u = f / last
        pmat = ob.pose.bones['pzadek'].matrix.to_3x3()
        for s, ph in (('p', 0.0), ('l', 0.5)):
            upper, fore, bend = _crawl_at(u + ph, s == 'l')
            up = (pmat @ p0inv @ wi @ upper).normalized()
            fp = pmat @ p0inv @ wi @ fore
            fp = (fp - up * fp.dot(up)).normalized()
            bi, lo = ob.pose.bones[s + 'biceps'], ob.pose.bones[s + 'loket']
            # the way this elbow bends, in the upper arm's own axes
            rel = bi.bone.matrix_local.to_3x3().inverted() @ lo.bone.matrix_local.to_3x3()
            b = rel @ Vector((0, 0, 1))
            b = (b - Vector((0, 1, 0)) * b.y).normalized()
            src = Matrix((Vector((0, 1, 0)), b, Vector((0, 1, 0)).cross(b))).transposed()
            dst = Matrix((up, fp, up.cross(fp))).transposed()
            m = bi.matrix.copy()
            rot = (dst @ src.inverted()).to_4x4()
            rot.translation = m.translation
            bi.matrix = rot
            bpy.context.view_layer.update()
            e = bi.matrix_basis.to_euler(bi.rotation_mode, prev[s]) if s in prev else bi.matrix_basis.to_euler(bi.rotation_mode)
            prev[s] = e
            bi.rotation_euler = e
            bi.keyframe_insert('rotation_euler', frame=f)
            lo.rotation_euler = dm['R'](max(bend, 2.0))
            lo.keyframe_insert('rotation_euler', frame=f)
            bpy.context.view_layer.update()
    return act


def crawl_keys():
    keys = []
    for i in range(24):
        u = i / 24
        p = P(pitch=CRAWL_PITCH, fwd=0.32, height=1.44, neck=-30, head=-14, spine=-4,
              roll=20 * math.cos(2 * math.pi * (u - 0.22)))  # the recovering arm's shoulder up
        for s, ph in (('p', 0.0), ('l', 0.5)):
            # the shoulder girdle as for a reach; the arm itself is set by crawl_arms
            p.update({s + 'flex': 120.0, s + 'abd': 30.0, s + 'twist': 0.0, s + 'elbow': 40.0, s + 'wrist': 0.0})
            k = math.sin(6 * math.pi * u + (0.0 if s == 'p' else math.pi))  # six beats a cycle, the legs in turn
            p.update({s + 'hip': 4 + 14 * k, s + 'hipabd': 3, s + 'hiptwist': 4, s + 'knee': 14 + 12 * max(0.0, -k),
                      s + 'ankle': -48})
        keys.append((u, p))
    return keys


def _proxy_frame(points):
    """The engine's frame for a proxy triangle (ShapeLOD.cpp): p0 the corner of the two shortest edges, the
    direction along the shortest, up along the second -- as (origin, direction, up)."""
    p0, p1, p2 = points
    d = lambda a, b: (a - b).length_squared
    if d(p0, p1) > d(p0, p2):
        p1, p2 = p2, p1
    if d(p0, p1) > d(p1, p2):
        p0, p2 = p2, p0
    if d(p0, p2) > d(p1, p2):
        p0, p1 = p1, p0
    return p0, (p1 - p0).normalized(), (p2 - p0).normalized()


def crawl_sling(act):
    """The crawl's recovering right arm swept through the rifle where the other moves sling it (up behind the right
    shoulder, measured in game). For the crawl the rifle lies along the spine on his back, muzzle to his feet,
    between the arms: the 'zbran' bone is set on every frame so the weapon proxy lands there."""
    import bpy
    from mathutils import Matrix, Vector
    ob = bpy.data.objects['MC_Rig']
    mesh = bpy.data.objects['MC_Soldier']
    sc = bpy.context.scene
    grp = mesh.vertex_groups['zbran'].index
    to_arm = ob.matrix_world.inverted() @ mesh.matrix_world
    tri = [to_arm @ v.co for v in mesh.data.vertices if any(g.group == grp and g.weight > 0.5 for g in v.groups)]
    o, fwd, up = _proxy_frame(tri)
    side = fwd.cross(up)
    rest = Matrix((side, fwd, up)).transposed().to_4x4()  # columns: the proxy's axes, as the rest weapon frame
    rest.translation = o
    pb = ob.pose.bones['zbran']
    ob.animation_data.action = act
    for f in range(int(act.frame_range[1]) + 1):
        sc.frame_set(f)
        h = ob.pose.bones['hrudnik'].matrix
        spine = h.to_3x3() @ Vector((0, 1, 0))           # the chest bone runs up the spine
        back = spine.cross(ob.pose.bones['pbiceps'].matrix.translation - ob.pose.bones['lbiceps'].matrix.translation)
        back.normalize()
        if back.dot(ob.matrix_world.inverted().to_3x3() @ Vector((0, 0, 1))) < 0:
            back = -back
        # the rifle model's barrel lies along the proxy's side axis (measured in game: with the direction along the
        # spine it lay across his shoulders)
        barrel = spine  # muzzle to his feet
        rdir = back.cross(barrel)
        want = Matrix((barrel, rdir, back)).transposed().to_4x4()
        want.translation = h.translation + back * 0.13 - spine * 0.05
        deform = want @ rest.inverted()                  # the weapon's deform: rest frame -> wanted frame
        pb.matrix = deform @ pb.bone.matrix_local
        bpy.context.view_layer.update()
        pb.keyframe_insert('location', frame=f)
        pb.keyframe_insert('rotation_euler', frame=f)
    return act
