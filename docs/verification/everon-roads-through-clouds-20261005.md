# Everon roads visible through clouds, 2026-10-05

Owner report: original OFP Everon camera [6759.20,3483.95,1764.24], az330.9,
el-47.9, overcast1.0. Installed2f44e267 reproduces the road network over an
otherwise opaque cloud bank. Thirteen moving actual-camera captures, original
Everon WRP, 1280x720 MSAA4, 8km view, noon, fog0; producer overcast1.0 and sky
coverage0.95 recorded. Baseline: build/fog-flight/everon-cloud-road-before-20261005-181415.

Road alpha colour was submitted in the post-water transparent queue after cloud
compositing, painting roads back over the clouds. Move only terrain-conformed
mode2 OnSurface rigid, depth-tested Alpha/Opaque world draws into the existing
pre-water colour replay and exclude them from the late replay. Their terrain
receiver is already in depth. No alpha depth write, extra pass, cloud/fog march,
texture or ABI change. Non-conformed decals, shadows, relief marks, glass,
particles, monitors and cockpit draws retain the previous route. Roads are direct
barrier draws in the plan, so their per-draw ground classification is not inferred
from an unrelated instance bucket.

Validation: dedicated routing test proves admission/exclusions and unchanged
prepass eligibility; five replay-filtered tests pass, including existing opaque,
cutout and cockpit cases. Complete build and installed same-scene/clear-road
checks pending. This change does not claim to fix the previously open distant
fog ring.

Installed248df7ad at18:35; full PoseidonGame build succeeded and both binaries
were deployed by the guarded installer. Same original Everon/cloud report:
build/fog-flight/everon-cloud-road-after-20261005-183556,13 actual moving poses,
overcast1.0,fog0. First/last captures visually remove the black road network;
normal exit and validation/panic/device-loss checks passed. Clear near-road
control at9182.32/2053.96/77.15, overcast0.15,fog0:
build/fog-flight/everon-cloud-road-clear-control-20261005-183705. Thirteen views
retain the visible road, normal exit passed. Budget:12 latest published samples
per cloud run; cloud march mean0.1027→0.1050ms, cloud composite0.0920→0.1137ms,
total GPU9.4129→9.5840ms. These are small nonblocking sample sets, not matched
completed-frame benchmarks; no claim of a speedup or general performance bound.
No additional atmosphere work was introduced. Condensed runtime receipt:
everon-roads-through-clouds-20261005-runtime.json.
