# CLD-021 — Cloud ground shadows ended in a hard line at 2048 m

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Status:** PARTIAL — fixed and A/B verified in pixels and GPU cost; wind coupling not exercisable in the capture harness.

## The fault

Flying, there was a dead-straight horizontal edge in the distance beyond which cloud shadows stopped
affecting the ground, and it tracked the camera. `CLOUD_SHADOW_SPAN` was a fixed **4096 m centred on
the camera**, so the map's half-extent is 2048 m, while the view distance is far greater.
`cloud_sun_shadow` (`shaders/frame.wgsl`) returns a hard `1.0` outside the map's UV range.

**Measured, not eyeballed.** Everon (`ev_DALL.wrp`), camera 700 m altitude, -8 degrees, `--vd 5000`,
`WGR_CLOUD_COVERAGE=0.65`, hour 10: the terrain row-brightness profile shows a **69.7-luminance
brightening across rows 440->500, steepest slope -3.0 lum/row at row 462** — a straight edge at
exactly 2048 m. Confirmed by temporarily emitting `cloud_sun_shadow()` as greyscale from the terrain
shader (since reverted; `terrain/terrain.wgsl` is byte-identical to HEAD): shadowed grey below the
line, pure white 1.0 above it, boundary one texel wide.

## The fix

**Primary — size the span from the camera's own draw distance, keep `DIM` at 512.**
`fog_start + 1/fog_inv_range` *is* the far plane and terrain-grid cull distance, so it is exactly the
radius the map must cover. The span is now `clamp(2 * draw_distance, 4096, 16384)`, rounded up to a
**power-of-two multiple of 4096**.

The trade is **metres per texel, not GPU time**: the dispatch is unchanged (512^2, 12 march steps,
1 MB RGBA8), so range is bought with resolution — 8 m/texel on foot (`vd 1200` keeps span 4096,
verified by the absence of any level-change log line), 32 m/texel at `vd 5000`. That is the right
direction: the resolution loss only applies when the shadows in question are kilometres away and half
dissolved in fog.

**Powers of two preserve the snap**, which the original comment identifies as the whole trick (a
continuously camera-tracking origin re-rasterises the same clouds into different texels every frame,
which reads as shadows crawling independently of the wind). Every level's grid is a subset of every
finer one, so a level change re-rasterises once rather than sliding the field. Hysteresis (shrink only
below 0.45x) stops a weather-driven fog range flapping the level. Verified empirically: the span line
logs **exactly once per run** in every capture.

**Secondary — a soft fade to fully lit** over the outer 15% of the half-extent, in both
`cloud_sun_shadow` and god-rays' `cloud_visibility` (which needs it more: its sun-parallax shift
leaves the map hundreds of metres sooner). It only ever mixes *toward* 1.0, so it cannot produce the
dark edge band that the original hard `return 1.0` exists to prevent — that reasoning is preserved and
now machine-checked by tests.

**Rejected, and documented in the code as the option not taken:** a nested near/far cascade would give
8 m near and 32 m far, but at 2x the march — the expensive part — for a far field that is small on
screen and heavily fogged.

## Cost

Interleaved A/B, `GPU frame total` only (per `gpu-timer-regions-persist`, the one region re-measured
per frame), n=8 each, same camera and settings, no other `PoseidonGame` running:

| | min | median | max |
|---|---|---|---|
| before | 10.410 | 10.732 | 11.421 |
| after | 10.314 | **10.807** | 15.132 |

**+0.075 ms median, -0.096 ms min** — free within noise on a ~10.8 ms frame, as the analysis predicts.
Zero extra texels, zero extra march work, zero extra memory; the only added work is ~5 ALU ops per
shadow lookup.

## Artefacts

Under `C:\Users\mail\AppData\Local\Temp\rf\cap\`: `CLD_AB_normal.png` (before/after, normal shading),
`CLD_AB_term.png` (the transmittance term itself, greyscale), `CLD_before_norm.png` /
`CLD_after_norm.png`, `CLD_dbg_b.png` / `CLD_dbg_after.png`, and `CLD_wind_AB.png` — the fixed term at
coverage 0.55 showing large organic cloud shadows spreading continuously to the horizon, with no line,
no ring and no square.

## Files touched (uncommitted)

- `engine/WgpuRenderer/rust/src/sky/mod.rs` — span constants, `cloud_shadow_span` state,
  `cloud_shadow_span_for` / `select_cloud_shadow_span`, `begin_cloud_shadow_frame` signature,
  **7 new tests** in `cloud_shadow_range_tests`
- `engine/WgpuRenderer/rust/src/lib.rs` — derives the draw distance, logs level changes
- `engine/WgpuRenderer/rust/src/shaders/frame.wgsl` — edge fade
- `engine/WgpuRenderer/rust/src/godrays.wgsl` — matching edge fade

## Not verified

**Wind coupling could not be exercised in pixels.** The capture harness's mission hint dialog pauses
the world, so the sky is frozen between frames 600 and 2400 (sky mean |diff| 0.15). No wind code was
touched — the offset lives in `cloud_world`, upstream of everything changed here — but the "shadows
still move with the wind and do not swim" check remains open, and **this harness limitation blocks any
wind-related visual verification**, including vegetation sway. Dismissing the hint or using a mission
without it is a prerequisite for that work.

The A/B was necessarily built on a shared checkout carrying other agents' in-flight `EngineWgpu.cpp`
and `water/mod.rs` edits — identically on both sides, so the comparison holds.
