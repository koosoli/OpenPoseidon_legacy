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
          'swimrf': (1.6, 41, (-1.06, 0.0, -1.06))}
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
