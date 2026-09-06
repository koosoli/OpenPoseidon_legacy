# WRL-003 — Bounded water bodies: registry, per-body draw, containment (2026-09-06)

Package: WRL-003 (plus the lake portions of WRL-003a step 1-2, WRL-006 step 1 and the WRL-006b hook)
State: implemented, author-verified on a synthetic lake fixture; boat verification (WRL-006b) NOT done.
Commit: `7fe5875b` on `water-realism`. Installed pair for every capture below: `water-realism 7fe5875b 2026-09-06_13:36 deploy-ps1`.

## What exists now

- `engine/Poseidon/World/Terrain/WaterBodies.{hpp,cpp}` — simulation-owned registry: id, generation, kind (lake/river), ellipse bounds with a box reject, mean level with a linear gradient (sloping reach), wave scale, visual flow, profile. `Find(x, z)` is arithmetic, headless, deterministic on overlap (smaller area wins, then lower id).
- Collisions hook — a hull point inside a body sees the body's mean level as its surface, its wave scale in the CPU predictor (`QueryWaterSurfaceScaled`), and its datum for `underFlat`. Off-body points are byte-identical to before (one empty-vector check when nothing is registered).
- Renderer — node lane `body` (0 = ocean), a 16-entry body table in `WgrWaterParams` (288 → 800 B), per-node level and wave scale in the vertex stage, ellipse discard with a soft rim in the fragment stage, no planar sampling for bodies, four profile multipliers on the shared optics. The camera's containing body drives the underwater compositor's level and wave scale (`optics_params.zw`).
- Fixtures — `WGR_WATER_LAKE=x,z,rx,rz,level[,wave[,profile]]`, `WGR_WATER_RIVER=...`, and Water tab buttons (add lake / river at camera, clear). Re-registered on map change.

## Fixture and reproduction

Everon, `perf_water.eden`, lake at (7250, 6350), 80 x 60 m, surface 25.0 m (terrain at the centre is 23.6 m; the sea is at ~2.3 m), clear-lake profile, wave scale 0.08. Script: scratchpad `capture_lake.ps1` (installed exe, `WGR_WATER_LAKE=7250,6350,80,60,25,0.08,0`, `POSEIDON_VSYNC=0`).

| view | freefly | notes |
| --- | --- | --- |
| lakeair | `7250 6230 62 0 -28`, 12 h | lake in its hollow with the SEA visible 22 m lower in the same frame |
| lakeside | `7330 6300 30 300 -12`, 15 h | surface meets rising terrain, edge cut by the terrain, no ocean swell |
| lakeunder | `7250 6350 24.3 90 -4`, 12 h, scratch user dir with a v3 profile enabling the underwater effect | eye 0.7 m under the lake surface |
| lakeedge | `7150 6350 28 90 -6` | camera ended up under the terrain: not evidence (recorded so nobody reuses the pose) |

Log lines that prove the mechanism rather than the picture:
```
Water body fixture: lake #1 at (7250, 6350) r=(80, 60) level 25.00 wave x0.08
Water submersion: submerged=1 camY=24.30 sea=2.33 localSurface=25.00 camAbove=21.97 submersion=0.70 ... effect=1
```
`localSurface` is the lake's level and the compositor engaged with the camera 22 m above the sea datum: containment selected the body, not the sea.

## What the captures show (author's reading)

- The lake draws at 25 m inside its ellipse only; the sea in the same frame stays at its own level, so two surfaces at different elevations coexist without either flooding the other (plan section 3.4 item 5, for the non-overlapping case).
- The lake bed is the terrain from the opaque snapshot; where the terrain rises above 25 m inside the ellipse the surface is occluded, which is the intended "partial flood" of a synthetic ellipse.
- No ocean swell on the lake; ripples at 0.08 of the sea's amplitude read as sheltered water. No shore breaker train.
- Under the surface the volume is the shared-optics teal and the sky is seen through the wavy surface from below; the waterline sits at the lake's level.
- A visible bright rim runs along the lake edge: the shoreline foam band and the coverage fade both act there. On a lake this should be much weaker (plan 3.3: "little surf"); it is left as the first WRL-004 item.

## Measurements

`perf_water.eden` bench (no fixture registered, so this is the cost of the mechanism with zero bodies):

| repeat | GPU frame (ms) | Water draw | all water rows | CPU avg | CPU p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 26.72 | 5.79 | 8.21 | 28.16 | 26.58 |
| 2 | 27.25 | 6.47 | 8.88 | 27.52 | 25.84 |
| 3 | 27.59 | 6.95 | 9.34 | 27.13 | 25.96 |

Against the previous arm (a1710b3f: 26.3–27.6 / 5.9–6.2 / 8.4–8.6) the water rows sit in the same band as the WRL-001 baseline (8.4–9.7); the params UBO grew to 800 B, no other allocation. With the lake fixture registered, the lakeair single capture (1600x900, mostly terrain, little sea in frame) measured a 9.7 ms GPU frame with 2.3 ms of water draw; one lake adds its 1-4 leaf nodes and nothing else, and no capture with a body registered showed a new row in the timer table. Not measured: many bodies (the table caps at 16; metadata scales, resources do not).

## Tests

- `PoseidonTests "[Water]"`: 7 cases pass (5 new: ellipse-vs-box containment, sloping river height, overlap priority, lifecycle/generation, degenerate bodies).
- `cargo test ... water underwater shaders ffi planar`: 68 pass, including `bounded_bodies_draw_inside_their_ellipse_at_their_own_level` (seabed clamp at the body level, ellipse discard, no planar for bodies) and the layout asserts for the 800-byte params and the node lane at offset 36.

## Known limitations (honest list)

- Boats on the lake are not verified (WRL-006b steps 1–6 undone). The hook is in place and unit-exercised only through the registry; a boat placed on the fixture needs a mission and an observer.
- River flow still uses the global flow lane; the body's `flow*` fields are registered but not consumed by the shader.
- Interaction (ripple/foam) fields are camera-anchored and shared, so an event on the sea can advect onto a nearby lake at the same XZ (plan WRL-004 step 7).
- Containment is an ellipse; polygons and connected mouths are not represented.
- The lake edge carries the ocean shoreline foam band (see above).
- Bodies re-register from the env/dev fixtures only; there is no loader. Map change clears them.
