# RTM read/write for the swim scripts: the Blender add-on's own module (apps/tools/BlenderAddon/io_import_p3d/rtm_io.py),
# loaded by path so it works outside Blender and without importing the add-on package.
import importlib.util
import os

_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'apps', 'tools', 'BlenderAddon',
                     'io_import_p3d', 'rtm_io.py')
_spec = importlib.util.spec_from_file_location('poseidon_rtm_io', os.path.abspath(_path))
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

read_rtm = _mod.read_rtm
read_rtm_bytes = _mod.read_rtm_bytes
write_rtm = _mod.write_rtm
mat4 = _mod.mat4
unmat = _mod.unmat
