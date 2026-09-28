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
| `WGR_WATER_SKIRT_K` | Rust | |
| `WGR_WATER_BACKEND` | Rust (+ Water tab note) | TW-WATER W1. `0` = Current OP, `1` = Tidewater Native; overrides the Water-tab backend selector for the whole session (harness runs). Unset = follow the Water tab. |
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
| `WGR_TW_WIND` | Rust | TW-WATER W9c. `<m/s>` (0.5..40) sets Tidewater's local wind for the session, over the weather's and the sea-conditions preset's; in the Water tab's weather mode it also picks the sea state from Tidewater's table at that wind (`WaterWgpu.cpp`), as a real weather wind would. With no preset fetch the fetch follows the wind (`fft.rs wind_fetch_km`). For captures; the periodic `TW prepare` line reports the resulting Hs, rms slope and whitecap threshold. |
| `WGR_TW_LIP` | Rust | TW-WATER W10. `<opacity>` (0..2) overrides the Water tab's breaker lip opacity (Tidewater's `sheet`; 0 = no thrown lips) for the session. For A/B captures of the curling breakers. |
| `WGR_FRAME_STATS` | C++ bridge | TW-WATER W7. `1` logs every 5 s: wall-clock and GPU frame p50/p95/p99, the Current OP and Tidewater water GPU regions, backend allocations, the render worker's CPU phases, and (Tidewater) the CPU cost of the water `prepare` and its shore-station updates. Diagnostic only; used for the W7 parity report. |
| `WGR_TW_EARLY_SUBMIT` | Rust | TW-WATER W7e/W7f. A Tidewater frame submits its early part (water simulation, culls, far field, GI) as its own command buffer, as the planar-reflection split does in Current OP mode, so the GPU starts while the rest of the frame is recorded (on by default since W7f; P1 wall p50 ~9.2 -> ~6.9 ms). `0` keeps one submission per frame, for A/B. |
| `WGR_WATER_SOAK` | Rust | TW-WATER W7h. `<frames>[,<switches>]` flips the water backend every `<frames>` frames, `<switches>` times (default 20), then hands the choice back to `WGR_WATER_BACKEND` / the Water tab (plan §8's switch soak). Every backend switch (soak or not) logs the GPU allocations and live handle counts. Test only. |
| `WGR_TW_GRID` | Rust | TW-WATER W7h. The Tidewater CDLOD grid resolution per node (16..128, a multiple of 8; default Tidewater's 32), for the mesh-density experiment (plan §7.2). |
| `WGR_VB_UPDATE_STATS` | C++ bridge | Logs two-second, per-`VBType` create/destroy/cancel and update/build/hash/upload counts, bytes, total time and maximum call time. Diagnostic only; unset/`0` avoids timers and atomics in `VertexBufferWgpu`. |

## Engine switches added by TW-WATER (outside the renderer)

Read by the game engine, not by `engine/WgpuRenderer/`, so the scan below does not find them.

| Switch | Read by | Notes |
| --- | --- | --- |
| `WGR_AMPHIB_WAVE` | `World/Entities/Vehicles/Ground/TrackedWaterResponse.hpp` | TW-WATER W6d. `<m>` overrides how far a tracked amphibian's buoyancy follows the wave height (default 0.35 m). For tuning captures. |
| `WGR_CAR_TRACE` | `World/Entities/Vehicles/Ground/Car.cpp` | TW-WATER W6d. `1` logs a floatable car's simulation state about once a second. Diagnostic only. |

## Regenerating

This table is derived, not maintained by hand. Re-scan with:

```bash
grep -rhoE '(getenv|env::var|var)\("WGR_[A-Z_0-9]+"' engine/WgpuRenderer/ \
  | grep -oE 'WGR_[A-Z_0-9]+' | sort -u
```

A switch that appears in the code but not here means the table is stale — which
is the failure mode `RND-030` found throughout this repository's renderer
documentation, so prefer the scan over the table when they disagree.
