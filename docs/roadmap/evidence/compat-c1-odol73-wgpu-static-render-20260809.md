# ODOL 73 static WGPU render proof — 2026-08-09

`ColdWarAssault.exe --check --render=wgpu --test-mission dev-missions/devtest.abel --test-model <model> --test-type screenshot` was run from the installed CWA game with the executable and `wgpu_renderer.dll` copied together from build `ae3766f`.

The harness now follows the static mission-object lifecycle: `Shapes.New` →
`NewObject(shape, -1)` → `SetTransform` → `Landscape::AddObject`. This replaced
the prior generic `Building` vehicle wrapper, which was not how an external
static model is constructed by the mission/world loader.

Control: the installed CWA `a3rock.p3d` was visibly submitted and captured by
the same harness. Its log reports 10 LODs, 2,930 level-0 faces and a 16.22435 m
bounding radius. See `compat-c1-wgpu-test-model-cwa-control-20260809.log` and
`.png`.

Arma 3 fixture: `stonesharp_big.p3d`, extracted without modification from the
owner's installed `Arma 3/Addons/rocks_f.pbo`, visibly rendered through WGPU.
The capture shows its geometry in front of the test camera; its log reports 10
LODs, 6,256 level-0 faces and a 9.141871 m bounding radius. See
`compat-c1-odol73-wgpu-rock-20260809.log` and `.png`.

Limitation: only the P3D was extracted for this smoke test. Its RVMAT/PAA
dependencies were intentionally absent, so the rock appears white. This is
evidence of production-path geometry draw and scene inclusion only; it does not
validate material assignment, texture resolution, alpha handling, Super shader
mapping, winding, normal fidelity, section ranges, or additional UV channels.
