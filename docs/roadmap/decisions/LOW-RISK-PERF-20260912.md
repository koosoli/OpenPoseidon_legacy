# Low-risk performance follow-through, September 12

Baseline: asset-obs-001 09f45241, matching installed EXE/DLL. Safety mirror was
at the same commit. Foreign Cargo.lock and FPS-TERRAIN-DIAGNOSIS notes untouched.
Read the lighting handover, PARKED, post-DLSS roadmap, three-track assessment,
FPS-cap verification and terrain diagnosis before choosing these slices.

## Camera uniform uploads: integrated

55ec367f, with opt-in timing in 370b9c47. Gfx3d::prepare in
engine/WgpuRenderer/rust/src/gfx3d/mod.rs used 17 Queue::write_buffer calls per
camera. camera_upload.rs coalesces those exact writes, at unchanged offsets,
into one frame upload. Padding between dynamic-offset bindings is zeroed;
shader-visible data, per-camera GTAO gates, reflection clip and material lanes
are unchanged. The renderer still owns submission. No shader/ABI change.

WGR_CAMERA_UPLOAD_BATCH=0 restores the original writes for A/B. Default is on.
WGR_CAMERA_UPLOAD_TRACE=1 emits 512-frame CPU averages to stderr, off normally.
Measured with two cameras: 34 calls -> 1, CPU preparation/enqueue block
54.47-56.81 us -> 6.60-7.86 us. About 0.048 ms saved, NOT a GPU-time or FPS gain.
Two packing/bounds tests pass (including arbitrary bit patterns and overwrites).

Installed route: scripts/pace-series.ps1, perf_field.eden, 1600x900 native,
MSAA1, DLSS/VSync off, uncapped, shadow tier3, 10s settle/10s static/20s moving,
30s load allowance. Scratch user config; WGR_LOD_GOVERNOR_RANGE=1 pins min=max
at 0.0078125 in BOTH arms, not a production quality change. Endpoint census
matches: 52507 registered, 13892 main instances, 1908894 main triangles and
identical LOD histogram. Endpoint screenshots inspected, no apparent regression;
different cloud/wind time means these are not pixel-identical captures.

Moving-phase engine timings (not PresentMon/display timings):

| Arm | FPS | p95 ms | p99 ms | maximum ms |
|---|---:|---:|---:|---:|
| off2 | 67.20 | 16.37 | 18.10 | 48.64 |
| off3 | 70.88 | 15.80 | 16.67 | 45.68 |
| on1 | 67.95 | 16.68 | 37.51 | 47.60 |
| on2 | 67.23 | 17.26 | 37.74 | 61.08 |
| on3 | 67.88 | 16.40 | 19.46 | 49.60 |

No sustained-FPS or tail-latency gain established. on1/on2 have larger simulation
spikes; their cause is not attributed to uploads. off1 is excluded because the
renderer test suite overlapped it. No thermal/clock normalization or RAM/VRAM
peak comparison. Artifacts: .tmp-movecam/out/camera-upload-{arm}-cap0, including
CSV traces, metrics, images, metadata and stderr. off/on3 used 370b9c47, earlier
arms 55ec367f; the extra diagnostic is enabled in both timed arms.

## Same-size terrain textures: measured, default OFF

9e0a1bbb: Terrain::set_heightmap reuses height and shadow textures when their
dimensions match. rebuild_skyvis similarly updates the existing R8 texture.
WGR_TERRAIN_RESOURCE_REUSE=1 enables the experiment; default is reallocation.
Content uploads, full source copies, CPU skyvis calculation, quadtree work,
collision and shadow_dirty/generation changes are NOT skipped. Resize still
allocates. Queue order keeps previously submitted frames ahead of the writes.
This reduces allocation churn during edits; it cannot improve unchanged-terrain
FPS because set_heightmap does not run then. Twelve terrain-related tests pass.

Installed 9e0a1bbb ten-edit/restore A/B completed cleanly in both arms, all ten
edits and restore returned OK. Resource trace confirms same-size reuse on each
edit; endpoint object/triangle counts match. Same 1600x900/MSAA1/native/shadow3,
uncapped fixed-LOD scratch settings as above. Artifacts:
.tmp-farfield-bench/terrain-reuse-{0,1} and .tmp-fpsaudit/reuse-{0,1} CSV traces.
The mission and analyser are now preserved as
tests/perf/missions/perf_terrain_edits.eden and scripts/analyse-terrain-edits.py.
Use farfield-bench -WarmupSeconds 105 and POSEIDON_FRAME_TRACE, under game lock.

| Edit window | p95 ms | p99 ms | maximum ms |
|---|---:|---:|---:|
| Reallocate | 105.80 | 135.63 | 536.63 |
| Reuse | 118.45 | 190.23 | 584.59 |

Baseline/settled-edited/restored FPS: 70.39/72.65/72.71 versus
68.35/68.44/68.00. One ordered pair, no clock normalization or peak-memory
instrumentation: neither an improvement nor a causal regression is established.
The result does NOT meet the performance gate, so reuse is default OFF rather
than sold as a fix. Reopen only with isolated allocation/synchronization costs
and repeated comparisons. Collision setup, full uploads and rebuilds remain.

## Test census and priorities

Full renderer suite before terrain change: 300 passed, one ignored, one failure.
The failure is the pre-existing grass card source-string expectation: it omits
the native-grass exclusion already in grass.wgsl at baseline 09f45241. It is not
caused by camera uploads. Do not report this as a fully green suite.

Next meaningful work is not a new scheduler: partial heightfield publication
and collision preparation for editing stalls; native DDS composition/admission
for Reforger streaming; GPU attribution of cloud, grass and water for steady FPS.
Existing worker/preparer and palette batching must be reused. The old measured
null upload-budget experiment and rejected vegetation-detail cuts stay rejected.
Do not trade visibility, fog, vegetation density or material quality for numbers.
