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
