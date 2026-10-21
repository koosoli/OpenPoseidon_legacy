# WGPU renderer environment switches

Generated from the code on 2026-08-02 by scanning `getenv` / `env::var` call
sites under `engine/WgpuRenderer/`. **30 switches** exist across the C++ bridge
and the Rust renderer.

## Read this first

Eight of these were **inert until `9fb2e47`**. `ConfigureWgpuUltraEnvironment()`
in `apps/cwr/Game/GameApplication.cpp` applied its profile with `_putenv_s`,
which overwrites, and it runs before renderer creation reads the variables — so
setting any of the eight had no effect whatsoever. They are now applied as
defaults, so an explicit value wins and the shipped profile is unchanged.

If you are reading an older report or commit that claims one of these was used
to test something, treat the claim with suspicion: before `9fb2e47` it could not
have been.

## Inventory

| Switch | Read by | Notes |
| --- | --- | --- |
| `WGR_GPU_WATER` | C++ bridge | **Was inert.** `0` leaves the water renderer unbuilt; `Landscape::DrawWater` then falls through to the legacy per-segment mesh. |
| `WGR_WATER_FFT` | Rust | **Was inert.** |
| `WGR_HDR` | C++ bridge + Rust | **Was inert.** |
| `WGR_MSAA` | Rust | **Was inert.** |
| `WGR_PREPASS` | Rust | **Was inert.** |
| `WGR_INDIRECT` | Rust | **Was inert.** |
| `WGR_GPU_DRIVEN` | C++ bridge + Rust | **Was inert.** |
| `WGR_SHADOW_MAPS` | C++ bridge | **Was inert.** Cascaded shadow maps at startup. |
| `WGR_HDR_ENCODE` | C++ bridge | |
| `WGR_TONEMAP` | C++ bridge | |
| `WGR_EXPOSURE` | C++ bridge | |
| `WGR_CONFORM_DEBUG` | C++ bridge | |
| `WGR_GRASS_TUFT` | C++ bridge | |
| `WGR_SW_ZBIAS_MULT` | C++ bridge | |
| `WGR_GRASS` | Rust | |
| `WGR_GRASS_FILTER_SUBPIXEL` | Rust grass fragment pipelines | Default `1`: footprint-filtered normals/photo contour relief, taper-averaged procedural colour/dry tips/height AO, and opacity-weighted source blade colour means for unresolved native ribbons. `0` restores unfiltered shading for a same-binary A/B. Resolved native albedo, density and coverage are unchanged. |
| `WGR_GRASS_DENSITY` | Rust | |
| `WGR_GRASS_DISTANCE` | Rust | |
| `WGR_FOLIAGE_A2C` | Rust | Alpha-to-coverage for foliage. |
| `WGR_SKIN_BAKE` | Rust | Compute skin bake; default off. |
| `WGR_GPU_OCCLUSION` | Rust | |
| `WGR_GPU_VALIDATION` | Rust | |
| `WGR_CULL_NO_FRUSTUM` | Rust | Disables frustum culling (debug). |
| `WGR_SKY_DEBUG` | Rust | |
| `WGR_SKY_VIS_DEBUG` | Rust | |
| `WGR_TERRAIN_BLEND_WIDTH` | Rust | |
| `WGR_TERRAIN_SHADOW_SCALE` | Rust | |
| `WGR_TERRAIN_SHADOW_STEPS` | Rust | |
| `WGR_TERRAIN_SHADOW_PENUMBRA` | Rust | |
| `WGR_TERRAIN_SKIRT_K` | Rust | |
| `WGR_TERRAIN_PUDDLES` | C++ terrain bridge | `0` disables cosmetic rain puddles for this process; default `1`. Source mud/soil eligibility, slope, snow and shore exclusions always apply. Active interior-sky geometry uses its zenith layer for rain cover, independent of artistic AO; absent/outside/feathered map coverage retains open fallback. |
| `WGR_TERRAIN_PUDDLE_WETNESS` | C++ terrain bridge | `0..1` pins cosmetic wetness for a dry/wet appearance A/B. Unset or negative follows accumulated captured rain; no profile or gameplay-weather mutation. |
| `WGR_TERRAIN_PUDDLE_FIXTURE` | C++ terrain bridge | `1` logs native semantic material/BCR admission, a CPU-selected flat inland 3x3 mud patch and freefly pose, plus captured rain/accumulated/uploaded wetness every five simulation seconds. Excludes native road cells. Candidate only: the camera/site still needs runtime inspection. |
| `WGR_WATER_SKIRT_K` | Rust | |
| `WGR_WATER_BACKEND` | Rust (+ Water tab note) | TW-WATER W1. `0` = Current OP, `1` = Tidewater Native; overrides the Water-tab backend selector for the whole session (harness runs). Unset = follow the Water tab/profile; fresh or missing backend selection defaults to Tidewater Native. Explicit `0` retains legacy Current OP. Unsupported storage textures fall back to Current OP. |
| `WGR_TW_DEBUG` | Rust | TW-WATER W3h. Forces the Tidewater material debug view for the session (Water tab "TW debug view" numbering: 6 path length, 7 sea bed seen through, 8 sea depth, 10 SSR weight). Diagnostic only. |
| `WGR_TW_GLOW` | Rust | TW-WATER W3j. Overrides the Water tab's "TW water glow" (scale of the light scattered inside the water; 0 = reflection only, 1 = Tidewater's formula on OP's light) for the session. For A/B captures. |
| `WGR_TW_SPRAY` | Rust | TW-WATER W4a. `<gain>,<intensity>` overrides the Water tab's breaker spray emission gain and sprite intensity for the session (e.g. `1,8` makes every particle easy to see). For captures. |
| `WGR_TW_SPRAY_EARLY` | Rust | TW-WATER W12a. `1` draws the spray sprites inside the water pass, before the cloud composite (the order before W12a), so the clouds are painted over plumes that stand against the sky. For A/B captures. |
| `WGR_TW_WAKE` | Rust | TW-WATER W6h. `0` turns the Tidewater boat wakes (`water_tw/wake.rs`) off for the session; boats then leave no simulated wake in Tidewater mode. For A/B captures. |
| `WGR_TW_WAKE_STATS` | C++ bridge | TW-WATER W6h. `1` logs the wake slots (boat, Froude number, schedule) and the bow-spray particle / request counts with the periodic Tidewater log line. Diagnostic only. |
| `WGR_TW_WAKE_TUNE` | Rust | TW-WATER W6h.1. `foam=..,draft=..,source=..,wash=..,aer=..,amp=..` (any subset) scales the wake's foam, hull draft, source strength, propeller wash, aeration and displacement. Defaults are OP's tuned values (foam 0.2, draft 0.7, wash 0.8, the rest 1). For tuning captures. |
| `WGR_TW_BOAT_SPRAY` | Rust | TW-WATER W6i. `off` disables the boats' bow spray (`water_tw/boat_spray.rs`); a number scales the white water and mist thrown by the steady bow wave (default 1). |
| `WGR_TW_TORCH_BEAM` | Rust | TW-WATER W5e. The diver's torch beam in-scatter strength under water (Tidewater's `torchBeam`, default 3; `0` = no beam). The torch is any spot light within 2 m of the camera, e.g. the dev head torch (`POSEIDON_HEADLAMP=1`). |
| `WGR_TW_BOAT_SPRAY_TUNE` | Rust | TW-WATER W6k. `cap=..,drop=..,lig=..,white=..,mist=..,up=..` (any subset) scales the bow spray's per-frame particle cap, drops, ligaments, white water, mist and upward launch speed, relative to W6i.8. Defaults since W6l: `cap=0.45,drop=1,lig=0.35,white=0.35,mist=0.15,up=0.8`; `1` everywhere restores W6i.8. For tuning captures. |
| `WGR_TW_SPRAY_DIAG` | Rust | TW-WATER W6j.1. `1` logs the bow-spray diagnostics per 60 stepped frames (particles per kind, bursts, stem-contact depth and immersion rate, the producer's frame) with the periodic Tidewater lines. Diagnostic only. |
| `WGR_TW_SPRAY_DEBUG` | Rust | TW-WATER W6i.3. `1`..`11` switches the spray sprites to a diagnostic output (fade terms, depth, kind, ...; see `tw_spray.wgsl`). Diagnostic only. |
| `WGR_TW_LEGACY_BOAT_FX` | C++ (`Ship.cpp`) | TW-WATER W6i.6. `1` keeps the legacy WaterSource boat sprites (the grey bow/stern puffs) in Tidewater mode, where they are otherwise skipped because Tidewater draws its own wake and spray. |
| `WGR_TW_LEGACY_WATER_FX` | C++ (`Collisions.cpp`) | TW-WATER W8a. `1` keeps the legacy fireball and droplet sprites when shells, rockets, bombs and bullets hit the sea in Tidewater mode, where they are otherwise replaced by Tidewater's splash (`water_tw/impacts.rs`, sized from the ammo's `hit`, `indirectHit` and `indirectHitRange`). Damage is unchanged either way. |
| `WGR_TW_WIND` | C++ (`WaterWgpu.cpp`) | TW-WATER W9c; changed on `codex/streaming-residency` by `dea029a6`. `<m/s>` (0.5..40) overrides the weather-driven Tidewater **amplitude only** in the Water tab's Weather mode. It does not change the authored Breezy spectrum wind, heading, fetch, chop, swell, surf or whitecap parameters, and does not override explicit sea-condition presets. For A/B captures. |
| `WGR_TW_LIP` | Rust | TW-WATER W10c. `<opacity>` (0..2) overrides the Water tab's breaker lip opacity (Tidewater's `sheet`; 0 = no thrown lips) for the session. For A/B captures of the curling breakers. |
| `WGR_FRAME_STATS` | C++ bridge | TW-WATER W7. `1` logs every 5 s: wall-clock and GPU frame p50/p95/p99, the Current OP and Tidewater water GPU regions, backend allocations, the render worker's CPU phases, and (Tidewater) the CPU cost of the water `prepare` and its shore-station updates. Diagnostic only; used for the W7 parity report. |
| `WGR_TW_EARLY_SUBMIT` | Rust | TW-WATER W7e/W7f. A Tidewater frame submits its early part (water simulation, culls, far field, GI) as its own command buffer, as the planar-reflection split does in Current OP mode, so the GPU starts while the rest of the frame is recorded (on by default since W7f; P1 wall p50 ~9.2 -> ~6.9 ms). `0` keeps one submission per frame, for A/B. |
| `WGR_WATER_SOAK` | Rust | TW-WATER W7h. `<frames>[,<switches>]` flips the water backend every `<frames>` frames, `<switches>` times (default 20), then hands the choice back to `WGR_WATER_BACKEND` / the Water tab (plan §8's switch soak). Every backend switch (soak or not) logs the GPU allocations and live handle counts. Test only. |
| `WGR_TW_GRID` | Rust | TW-WATER W7h. The Tidewater CDLOD grid resolution per node (16..128, a multiple of 8; default Tidewater's 32), for the mesh-density experiment (plan §7.2). |
| `WGR_CULL_COOP` | Rust (`gfx3d/cull.rs` -> `cull.wgsl` debug bit 3) | WGPU-030. `0` walks every retained instance's sections on its own cull thread, as before. Default: an instance whose LOD has 16 or more sections is walked by the whole 64-thread workgroup in COUNT, what-if and SCATTER. Same counts, runs and draw arguments; measured case: cull_total 3.7-4.1 ms for 615 instances of ~300 sections. For A/B. |
| `WGR_VB_WHOLE` | Rust (`gfx3d/mod.rs`) | WGPU-032. `0` re-slices the vertex buffer at every mesh change in direct draws and CPU shadow casters (the old binding). Default binds the geometry pool / baked skin buffer whole and addresses the mesh with `base_vertex`, as indirect draws already do; the VS-skinning fallback keeps its slice. Same vertices, fewer `set_vertex_buffer` calls. For A/B. |
| `WGR_SHADOW_STATE_FILTER` | Rust (`gfx3d/mod.rs`) | WGPU-032. `0` re-binds pipeline, bind groups 1..4, vertex and index buffer for every CPU shadow-caster bucket (the old loop). Default skips what is already bound; groups 1..4 are always re-bound after a pipeline change. For A/B. |
| `WGR_DIRECT_BATCH` | Rust (`gfx3d/mod.rs`) | WGPU-033. Consecutive colour-pass barrier draws (transparent sections, decals, ...) that share pipeline, camera and every replay filter go out as one `multi_draw_indexed_indirect`. `0` = one draw each (the old path). Default (or `exact`) = batch only runs without a MAT-053 solid twin: bit-identical image. `all` also batches twin runs (all twins of a run first, then its blends): colour changes where two sections of one run overlap on texels the twin draws; with the default `WGR_CUTOUT_MIP_ALPHA` 0.5 that reaches ~15% on minified textures, with `WGR_CUTOUT_MIP_ALPHA=0` at most 2%. Needs `INDIRECT_FIRST_INSTANCE` and `WGR_VB_WHOLE` on. |
| `WGR_DIRECT_BATCH_TRACE` | Rust (`gfx3d/mod.rs`) | WGPU-033 diagnostic. `1` logs every 300th frame how many direct-draw runs were batched (how many with twins), how many draws they hold and how many stayed single. |
| `POSEIDON_LAND_GND_SLOW_MS` / `POSEIDON_TERRAIN_DRAW_SLOW_MS` / `POSEIDON_WATER_DRAW_SLOW_MS` | C++ (`LandscapeRender.cpp`, `TerrainWgpu.cpp`, `WaterWgpu.cpp`) | PERF-030 diagnostic. A frame whose `land:gnd` phase (sky, clouds, terrain holes, terrain draw, water, horizon, queue flushes) or WGPU terrain / water draw took at least this many ms logs one `PERF-030 ... slow` line naming the pieces (first 40, then 1 in 25), plus a `mean` line every 600 frames. Defaults 25 / 10 / 10 ms; `0` turns a breakdown off. |
| `WGR_VB_UPDATE_STATS` | C++ bridge | Logs two-second, per-`VBType` create/destroy/cancel and update/build/hash/upload counts, bytes, total time and maximum call time. Diagnostic only; unset/`0` avoids timers and atomics in `VertexBufferWgpu`. |

## Engine switches added by TW-WATER (outside the renderer)

Read by the game engine, not by `engine/WgpuRenderer/`, so the scan below does not find them.

| Switch | Read by | Notes |
| --- | --- | --- |
| `WGR_AMPHIB_WAVE` | `World/Entities/Vehicles/Ground/TrackedWaterResponse.hpp` | TW-WATER W6d. `<m>` overrides how far a tracked amphibian's buoyancy follows the wave height (default 0.35 m). For tuning captures. |
| `WGR_CAR_TRACE` | `World/Entities/Vehicles/Ground/Car.cpp` | TW-WATER W6d. `1` logs a floatable car's simulation state about once a second. Diagnostic only. |
| `POSEIDON_BOAT_LEGACY_PLANING` | `World/Entities/Vehicles/Misc/Ship.cpp` | TW-WATER W11b–e. `1` restores the 2001 planing lift for small boats (buoyancy 20x at rest rising to 120x at 17 m/s) and the water drag it gave. The default since W11c is 9x at rest to 12x flat out, with the drag read at the depth the legacy lift would give, so the hull sits deeper (~0.25 m on average). Gameplay physics, owner-approved; see `docs/tw-water-w8-w12-report.md`. |
| `POSEIDON_BOAT_DIAG` | `World/Entities/Vehicles/Misc/Ship.cpp` | Once a second per boat: position, forward speed, thrust, brake. Since TW-WATER W11d also a `Boat buoyancy` line: water contact points, their submersion (sum, mean, mean against the flat sea), the planing gain, total buoyancy / weight, height and sea level. Logging only. |
| `POSEIDON_GL33_VB_TRUST_DIRTY` | `PoseidonGL33/EngineGL33_VertexBuffer.cpp` | WGPU-031. `1` makes GL33 re-copy a vertex buffer only when `InvalidateBuffer` dirtied it, as the WGPU backend does, instead of on every draw of every VBDynamic (any CfgVehicles) shape. Off by default until checked on animated vehicles. |
| `POSEIDON_MLOD_SELECTION_POINTS` | `Asset/Formats/P3D/MLODLoader.hpp` | WGPU-020. `0` drops loose points that a named selection holds in P3DM LODs below 1e13 (the old rule). Default keeps them, as the SP3X loader does (axis points such as `osaVolantZac`). |
| `POSEIDON_MLOD_CFG_SECTIONS` | `World/Model/ShapeAdapter.cpp` | WGPU-021. `0` stops the MLOD adapter from splitting sections at the CfgModels `sections[]` selections (the old behaviour; hiddenSelections on P3DM models then change nothing). |
| `POSEIDON_INFANTRY_DROWN` | `World/Entities/Infantry/SoldierOldSim.cpp` | TW-WATER W12c. `1` restores the legacy drowning damage: AI soldiers standing deeper than 1.2 m (0.6 m prone) lose health until they die (the player was always exempt). Off by default at the owner's request until swimming is added. |

## Switches added or changed by FIXES-20261003 and Sinkhole W1b / W3

See `docs/roadmap/decisions/FIXES-20261003.md`.

| Switch | Read by | Notes |
| --- | --- | --- |
| `WGR_BLEND_SOLID_DEPTH` | Rust | MAT-053. `0` turns off the depth-writing first draw of a mostly-solid blend section (texture >= 75% a=255; only its texels at alpha >= 0.98 write depth). On by default. |
| `WGR_BLEND_PASS_TRACE` | Engine (`SceneDraw.cpp`) + C++ bridge | MAT-051 revisit trace; since MAT-053 also names each texture that gets the `WGR_DRAW3D_BLEND_SECTION` flag. Diagnostic only. |
| `POSEIDON_NET_DEPTH` | C++ bridge | MAT-NET-001. **Default changed to on** (netting as a depth-writing cutout); `0` restores the blended path. |
| `POSEIDON_SP3X_ADAPTER` | Engine (`ShapeDraw.cpp`) | FIX-SP3X-001. `1` sends OFP SP3X MLODs through `ShapeAdapter` instead of the original loader (the adapter tears skinned soldiers -- fml_vorhees). |
| `WGR_CAVE_EXPOSURE_MAX` | Rust | Sinkhole W1b. Exposure ceiling the eye adapts toward with the camera underground (default 6); above ground the profile's own ceiling. |
| `WGR_CAVE_DESPECKLE` | Rust | Sinkhole W1b. `0` turns off the underground despeckle in the tonemap (a pixel more than 3x brighter than all eight neighbours takes the brightest neighbour; hides model pinhole cracks). |
| `POSEIDON_FLASHLIGHT` | Engine (`SoldierOldFlashlight.cpp`) | Sinkhole W1b. `0` disables the soldiers' handheld test flashlight. |
| `POSEIDON_FLASHLIGHT_AI_NIGHT` | Engine (`SoldierOldFlashlight.cpp`) | Sinkhole W1b. `1` lets AI switch the flashlight on at night when not cautious (underground they always do). Off so legacy night missions are unchanged. |
| `POSEIDON_SWIM` | Engine (`SoldierOldSwim.cpp`) | Sinkhole W3. `0` turns swimming off (men walk the sea bed as before). Swimming also needs the swim moves in the config (the local swim addon). |
| `POSEIDON_SWIM_SPLASH` | Engine (`SoldierOldSwim.cpp`) | Sinkhole W3. `0` turns off the swimmers' hand and foot splashes (water-interaction kind 6, drawn by the Tidewater water). |
| `POSEIDON_SWIM_DRAWN` | Engine (`SoldierOldSwim.cpp`) | Sinkhole W3. `0`: swimmers near the camera ride the CPU wave predictor instead of the Tidewater surface as drawn (the probe grid, `wgr_water_drawn_grid`). |
| `POSEIDON_SWIM_LOG` | Engine (`SoldierOldSwim.cpp`) | Sinkhole W3. Set: logs the player's swim heights (surface, eyes, limbs) and camera four times a second. |
| `POSEIDON_AI_SUPPRESSION_LOG` | Engine (`AIUnitImpl.cpp`) | AI-PORT A. `1` logs every near miss that suppresses an AI soldier (level before/after, shooter, pinned). |
| `POSEIDON_AI_HUNT_LOG` | Engine (`AIHunt.cpp`) | AI-PORT B. `1` logs hunting phase changes, why a soldier is blocked or idle, and hand grenades thrown or refused by the group interval. |

## Engine switches set by DIAG-001 (outside the renderer)

DIAG-001 (`docs/roadmap/decisions/DIAG-01-malprave-diagnostics-20261003.md`) adds command-line
flags, not environment switches (`--diag`, `--diag-cats`, `--diag-draw`, `--diag-keep-going`,
`--diag-slow-ms`, `--diag-seed`, `--diag-fixed-dt`, `--diag-stuck-sec`; see `--help --dev`). One of
them writes an existing switch:

| Switch | Read by | Notes |
| --- | --- | --- |
| `POSEIDON_LOCKSTEP_HZ` | `Core/Game/GameLoop.cpp` (`LockstepDeltaT`) | SIM-815 lockstep pacing, read once. `--diag-fixed-dt <s>` sets it to `1/s` before the world is initialised, unless it is already set (an explicit value wins). |

## Regenerating

This table is derived, not maintained by hand. Re-scan with:

```bash
grep -rhoE '(getenv|env::var|var)\("WGR_[A-Z_0-9]+"' engine/WgpuRenderer/ \
  | grep -oE 'WGR_[A-Z_0-9]+' | sort -u
```

A switch that appears in the code but not here means the table is stale — which
is the failure mode `RND-030` found throughout this repository's renderer
documentation, so prefer the scan over the table when they disagree.
