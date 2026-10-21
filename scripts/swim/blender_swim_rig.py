# The MC soldier rig for the swim scripts (dive_moves.py) -- now the Blender add-on's (apps/tools/BlenderAddon,
# io_import_p3d/rig.py: build, RTM import/export, joint limits, the collision check), with the names these scripts use.
# Run inside Blender:
#
#   import runpy; rig = runpy.run_path(r'<repo>\scripts\swim\blender_swim_rig.py')
#   rig['build'](mesh=r'<extracted Data3D.pbo>\mc vojakw2.p3d', dll=r'<repo>\dist\x64-win-rwdi\PoseidonFormats.dll')
#   rig['import_rtm'](r'<repo>\content\default-packs\swim\rtm\tread.rtm')
#   rig['export_rtm']('swimunder', r'<out>\swimunder.rtm', phases=41, step=(0, 0, -1.6))
#
# With the add-on installed and the game data mounted (sidebar > Poseidon), its Soldier Rig panel does the same.
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON = os.path.abspath(os.path.join(HERE, '..', '..', 'apps', 'tools', 'BlenderAddon'))
if ADDON not in sys.path:
    sys.path.insert(0, ADDON)
from io_import_p3d import p3d_lib, rig as _rig  # noqa: E402

ARM = _rig.ARM
RTM_BONES = _rig.RTM_BONES
rotation_mode = _rig.rotation_mode


def build(mesh=None, dll=None, textures=None):
    if dll:
        os.environ['POSEIDON_FORMATS_DLL'] = dll
    p3d_lib.load()
    return _rig.build(bpy.context, model_path=mesh, texture_root=textures)


def import_rtm(path, name=None):
    act = _rig.import_rtm_file(bpy.data.objects[ARM], path)
    if name and act.name != name:
        act.name = name
    return act


def export_rtm(action, path, phases=None, step=(0.0, 0.0, 0.0)):
    act = bpy.data.actions[action] if isinstance(action, str) else action
    return _rig.export_rtm(bpy.data.objects[ARM], act, path, phases=phases, step=step)
