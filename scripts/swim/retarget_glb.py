# The retarget now lives in the Blender add-on (apps/tools/BlenderAddon/io_import_p3d/retarget.py: the method, the
# bone map, the licence rules); this path is kept for the swim scripts and the pack's README. Run inside Blender:
#
#   rt = runpy.run_path(r'<repo>\scripts\swim\retarget_glb.py')
#   src, clip = rt['import_glb'](r'<dir>\swim_idle_loop.glb')
#   act = rt['retarget'](src, clip, 'tread_af', keep_from='tread', match_height_of='tread')
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON = os.path.abspath(os.path.join(HERE, '..', '..', 'apps', 'tools', 'BlenderAddon'))
if ADDON not in sys.path:
    sys.path.insert(0, ADDON)
from io_import_p3d.retarget import ARMS_AND_RIFLE, MAP, import_glb, retarget, source_bones  # noqa: E402,F401
