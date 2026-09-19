# Local curling-breaker prototype

The user clarified that the required effect is a travelling swell whose lip curls into a hollow barrel and collapses, not a heightfield surf/foam adjustment. This prototype retains the installed old ocean as the default and adds an explicitly enabled folded mesh at one authored Nogova beach.

## Scope and implementation

- `WGR_WATER_CURLING_BREAKER=1` enables a separate 36m-wide sheet, 6144 triangles plus 512 spray billboards. No new simulation textures or geometry buffers; pipeline/bind-group metadata is allocated only when enabled.
- A world-anchored ten-second arrival moves westward, grows a roughly 2.7m crest, curls the lip past vertical, drops into a shallow turbulent wash, and fades. Along-crest timing introduces a progressive break. This is authored animation, not a fluid solver or a general bathymetry-driven coastline solution.
- Normals come from the folded parametric surface. The tail borrows existing ocean displacement. Existing ocean geometry, physics, AI, boats, saved settings and the prior surf opt-in remain separate.
- Self-occlusion uses a separate depth-writing pass. Its bind group excludes scene depth to avoid read/write feedback; subsequent depth resolves are invalidated. This can add a resolve on multisampled configurations and must be included when judging performance. Disabled mode creates neither this pipeline nor this pass.
- The fixed fixture is terrain-guarded. It is not advertised as working around every coastline, or as part of planar reflections/authoritative boat-water queries.

## Checks and provisional budget

The GPU geometry test executes the production WGSL function. It verifies a change in horizontal tangent sign across the lip (actual overhang), shoreward travel, collapse below 0.3m, finite output and a stable repeating world anchor. Its first failure exposed nonperiodic turbulence; using cycle age fixed that failure. Shader composition passed. The first runtime check passed device validation but exposed a reversed visibility/fog blend and an unhelpful rear camera view; those were corrected before accepting visuals.

The provisional target is at most 0.5ms additional GPU work in the representative coastal view, with no cost from the optional draw when disabled. Measurements must use the screenshot-free interval, not capture stalls. Four stage captures are sufficient for shape inspection; no screenshot series is requested for timing.

## Status

Build, GPU shape test and initial runtime check completed. Corrected visual inspection, matched performance measurement, committed build and installation are pending. The old ocean remains default. Owner visual acceptance is not claimed.
