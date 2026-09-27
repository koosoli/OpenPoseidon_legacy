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
| `WGR_WATER_BACKEND` | Rust (+ Water tab note) | TW-WATER W1. `0` = Current OP, `1` = Tidewater Native; overrides the Water-tab backend selector for the whole session (harness runs). Unset = follow the Water tab/profile; fresh or missing backend selection defaults to Tidewater Native. Explicit `0` retains legacy Current OP. Unsupported storage textures fall back to Current OP. |
| `WGR_TW_DEBUG` | Rust | TW-WATER W3h. Forces the Tidewater material debug view for the session (Water tab "TW debug view" numbering: 6 path length, 7 sea bed seen through, 8 sea depth, 10 SSR weight). Diagnostic only. |
| `WGR_TW_GLOW` | Rust | TW-WATER W3j. Overrides the Water tab's "TW water glow" (scale of the light scattered inside the water; 0 = reflection only, 1 = Tidewater's formula on OP's light) for the session. For A/B captures. |
| `WGR_TW_SPRAY` | Rust | TW-WATER W4a. `<gain>,<intensity>` overrides the Water tab's breaker spray emission gain and sprite intensity for the session (e.g. `1,8` makes every particle easy to see). For captures. |
| `WGR_VB_UPDATE_STATS` | C++ bridge | Logs two-second, per-`VBType` create/destroy/cancel and update/build/hash/upload counts, bytes, total time and maximum call time. Diagnostic only; unset/`0` avoids timers and atomics in `VertexBufferWgpu`. |

## Regenerating

This table is derived, not maintained by hand. Re-scan with:

```bash
grep -rhoE '(getenv|env::var|var)\("WGR_[A-Z_0-9]+"' engine/WgpuRenderer/ \
  | grep -oE 'WGR_[A-Z_0-9]+' | sort -u
```

A switch that appears in the code but not here means the table is stale — which
is the failure mode `RND-030` found throughout this repository's renderer
documentation, so prefer the scan over the table when they disagree.
