# Native Reforger performance, 2026-09-09

Scope: native Everon from Reforger `addons` archives, not the converted WRP.
No visibility, resolution, grass density, shadow tier or object budget reductions.
Older lighting/smoke/water acceptance gaps remain open; this is not their closure.

## Reproduction

Run `scripts/Measure-NativeReforger.ps1` through `scripts/with-game-lock.sh`.
The wrapper selects `worlds/eden`, explicitly enables objects and native streaming,
uses a private profile, 1920x1080 windowed, no vsync, and a 90-second warmup.
Village camera: 5092.61,3995.64,18.3,200.9,-13.5; Coast is a second fixed pose.
Metrics/images/logs: `build/native-reforger-perf/<Pose>-<Label>/run-*.{json,png,log}`.
It rejects missing objects/terrain, rejected terrain rectangles and pending workers.
Default DLSS Quality renders internally at 1286x724, MSAA 4. Do not call that native
1920x1080 rendering. All arms retain these same settings.

## Landed correctness prerequisite: 5ecfdaea

`Landscape::Draw` rejected the valid 50 km native-grid rectangle as oversized
(-4648:-5104..2688:328; fog 49980). This skipped DrawRect, including object passes,
not just terrain. GPU CDLOD terrain/water now bypass the legacy segment-allocation
limit and cache sizing, retaining the invalid-coordinate guard and GL33 fallback.
Legacy grass shells prepare only their existing short clipping rectangle, aligned
to segments and intersected with the original rectangle. GPU grass is unchanged.

`Village-baseline` is INVALID: objects were inadvertently disabled AND terrain was
rejected. Its 5.4-6.0 ms empty-sky numbers are not performance evidence. Full-range
capture after the fix shows buildings, road, terrain, vegetation and shadows,
22,360 registered instances, and 23.502 ms GPU. Restoring rendering is not a speedup.

## Rejected solid shadow experiment: 1856f8b7, removed cb46469f

Opaque variant 0 (`alpha_ref == 0`) was tried with the identical vertex/depth state
but no fragment shader. Cutout foliage retained its shader. This also covered the
depth-only sky visibility pass; the RSM colour pass stayed unchanged.

Two installed native Village repeats each, identical executable:

| Arm | GPU total ms | Four shadow cascades ms | Shadow triangles |
| --- | --- | --- | --- |
| on 1 | 22.227 | 10.45 | 56,037,663 |
| on 2 | 22.605 | 10.48 | 56,128,794 |
| off 1 | 22.470 | 10.52 | 56,192,562 |
| off 2 | 22.826 | 10.63 | 56,048,497 |

All have 22,360 registered instances and settled workers. The initial full-range
capture had ~60.4M shadow triangles and is NOT the matching A/B control. The small
gain is not convincing against run variance; do not keep an extra pipeline or claim
a robust GPU improvement from these results. Rust renderer is restored exactly to
5ecfdaea. Switch was test-process-only, never written to the user's settings.

## Native material prefetch: d6ed7751

ObjectStreamPreparer reads native XOBs through the archive's external loader, but
its texture stage then looked up same-named *converted loose* EMATs under deploy
roots. The real material resolver prefers the mounted archive. Unused converted
PAAs consequently filled the 512 MB preparation store, with zero takes. Measured
full-range: 805.7 MB prepared cumulatively / 19.8 seconds texture worker work; warm
subsequent runs still spend ~7 seconds. This is background work, NOT a 7-second
steady-frame improvement or a directly equivalent load-time saving.

Skip only the loose-EMAT lookup for a model supplied by the native external loader.
Direct IR textures/stages still get their normal preparation; P3D/converted maps
are unchanged. No renderer inputs, material bindings or quality settings change.
Regression test supplies a native model with an actual IR texture and a loose
material decoy: only the actual texture is attempted. 74 relevant tests passed,
6,553 assertions, including native archive terrain and preparer/texture tests.

Runtime build 73fb2da2 compiled successfully and was pushed to both safety branches,
then deployed as a matched EXE/DLL.

### Installed Village verification

| Run | Texture preparation ms | Cumulative prepared MB | Live store MB | Taken models |
| --- | --- | --- | --- | --- |
| old off 1 | 7193.44 | 531.40 | 446.98 | 496 |
| old off 2 | 7017.89 | 562.96 | 350.96 | 496 |
| fixed 1 | 0.9299 | 0 | 0 | 496 |
| fixed 2 | 0.9377 | 0 | 0 | 496 |

Both fixed repeats pass the wrapper's completeness checks: 22,360 registered
instances, no queued/ready models, terrain colour present, no rejected rectangles.
The 7 unsupported models and preNLOD graffiti diagnostics already occur in controls;
they are not introduced or solved here. GPU tracked residency is unchanged at
1835.2 MB / 1603 object textures. The savings above are CPU preparation/RAM, not VRAM.

Fixed GPU totals: 23.030 / 23.022 ms. The normal automatic LOD governor is deliberately
still enabled: fixed runs draw ~8.32M main triangles versus ~7.91M in controls, so
do not sell these as a fixed-geometry FPS comparison. The governor responds to frame
history, including loading. This is another measurement trap for future GPU work.
Inspected Village images retain road/building materials, normal detail, vegetation
and shadows; no renderer quality settings or material inputs were reduced.

Coast installed verification also passed: 22,369 registered instances, 392 prepared
models taken, zero queued/ready, 0 MB prepared, 0.7896 ms texture preparation.
GPU 24.835 ms, water 1.48 ms, terrain colour 0.33 ms. Inspected the shoreline image:
water/foam, terrain, rocks, vegetation and shadows are present. This is a second-pose
smoke test, not a coast A/B speedup claim or acceptance of every existing material.

The installed game remains at matched runtime 73fb2da2. Benchmark processes exited
normally and released the game lock. The foreign Cargo.lock SHA256 remains
`9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E`.

## Remaining performance work

Far shadow cascades still dominate the GPU frame; the rejected solid-only fragment
optimization did not remove their geometry cost. Further work needs a fixed-geometry
comparison (account for the LOD governor), not lower shadow/grass/detail settings.
Native archive texture decompression/compositing and GPU registration still run
partly on the main thread and can hitch during cold admission. The corrected worker
now avoids unused work; it does not magically make those real uploads asynchronous.
