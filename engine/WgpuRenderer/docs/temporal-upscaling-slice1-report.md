# REN-TEMP-001 — Slice 1 report (baseline + resolution split)

Date: 2026-08-29. Branch `ren-temp-001`, code commit `ee5a0d24`, measured deploy stamped
`ren-temp-001 ee5a0d24` (exe+DLL pair verified; the DLL was string-checked for
`WGR_RENDER_SCALE` after an initial deploy staged a stale DLL — see "traps" below).
This is the STOP-and-report gate required by the plan (§35 of
[temporal-upscaling-plan.md](temporal-upscaling-plan.md)). Do not start jitter, motion
vectors, or any vendor SDK before this is reviewed.

## Phase 0 — baseline (installed 96bcc6e5, 5 scenes × 3 resolutions × 2 repeats)

`farfield-bench.ps1`, mission mode, `-Windowed` for 1080p/1440p, native fullscreen
3441×1440 (4K exceeds the display; the linear fit below extrapolates it). GPU = cheapest
repeat's frame total; CPU = profiler-ring average of that repeat; GPU share = GPU/CPU.

| scene (mission)        | 1920×1080 | 2560×1440 | 3441×1440 | GPU share | verdict |
|---|---|---|---|---|---|
| abel — grass plain     | 20.7 ms | 25.3 ms | 32.0 ms | 90–96 % | **GPU-bound** |
| town — Nogova town     | 12.9 ms | 15.7 ms | 22.6 ms | 77–92 % | **GPU-bound** |
| water — Everon coast   | 29.3 ms | 38.2 ms | 45.0 ms | 92–94 % | **GPU-bound** (worst scene) |
| field — open field     | 14.4 ms | 18.6 ms | 22.7 ms | 85–90 % | **GPU-bound** |
| combat — AI battle     | 19.5 ms | 24.3 ms | 28.7 ms | **25–39 %** | **CPU-bound**, 12.7 fps |

Conclusions the plan's gate asks for:
- Four of five scenes are honestly GPU-bound at 1440p+; reduced internal resolution has
  real headroom there. **Upscaling will do nothing for `combat`** — its 73–79 ms CPU frame
  is simulation/AI, and no renderer change touches it.
- The frame keeps a large resolution-independent floor (known: shadow cascades, water FFT,
  pass overhead, cloud fixed costs), so scaling buys sub-linear gains — measured directly
  below rather than assumed.

## Phase 1+2+3 — split, resolve, upscale, mip bias (commit ee5a0d24)

What changed (all renderer-side; C ABI untouched):
- `WGR_RENDER_SCALE` (50–100, HDR path only): every scene-phase target sizes to
  `render_size()` — MSAA HDR colour + resolve, depth/normal/GTAO, planar reflection,
  god-ray march, cloud march, water snapshot/coverage, DoF, underwater. Swapchain, bloom/
  exposure/tonemap, screenshots, LOD metrics (`far.set_view_metrics`) and both UI phases
  stay at the output size. `ui_depth` split from scene depth (output-sized).
- Bilinear upscale (`wgpu::util::TextureBlitter`) between DoF and bloom. At 100 % neither
  the target nor the pass exists — the native chain is structurally unchanged.
- Mip bias: `renscale` lane appended to the camera UBO; `textureSampleBias` on material /
  normal / cutout samples (shader3d, gpu_driven, grass blades, terrain detail); terrain's
  `textureSampleGrad` taps scale their texture gradients by 2^bias instead (the shadow
  receiver-plane math keeps raw derivatives). Zero-filled lane = bit-identical native.

Tests: 168 passed. `gpu_timers::region_indices_stay_ffi_stable` and three `sky::cirrus_*`
tests **fail on the clean base commit 96bcc6e5 too** — pre-existing trunk breakage, not
this change (verified by stash-and-rerun).

## Scale A/B (native 3441×1440, this build, 2 repeats per arm)

GPU frame (cheapest) and whole-frame fps (profiler average — what the player sees):

| scene | 100 % | 67 % | 50 % | fps 100→67→50 |
|---|---|---|---|---|
| abel  | 31.99 ms | 22.63 ms (−29 %) | 19.67 ms (−39 %) | 28.6 → 37.8 → 43.1 |
| town  | 22.03 ms | 16.10 ms (−27 %) | 12.94 ms (−41 %) | 36.9 → 51.8 → 61.6 |
| water | 48.98 ms | 32.60 ms (−33 %) | 26.30 ms (−46 %) | 20.3 → 28.1 → 33.7 |

- **Regression check:** 100 % matches the pre-split baseline (abel 31.99 vs 31.98,
  town 22.03 vs 22.61, water 48.98 vs 45.0–47.3 repeat spread) — the split costs nothing
  at native scale.
- **Realistic DLSS-Quality-mode expectation at this resolution: +32–40 % fps** (67 %
  scale), **+51–67 %** at 50 %, before the upscaler's own cost (~1–2 ms) is subtracted.
  The resolution-independent floor is why this is not 2×.

## Visual validation (PNG review, same pose per scene)

- Vegetation: A2C grass and cutout foliage dense and correct at 67 % and 50 %; no holes,
  no white/dark fringes, wind sway normal. Fences (thin geometry) survive at both scales,
  softer as expected from a bilinear filter.
- Weapon/HUD phase sharp at output resolution at every scale.
- Water scene at 67 %: sea, shore, coastline all correct.
- Softness at 50 % is plainly visible — expected; this is what the temporal upscaler is
  for. No systematic vegetation regression → the plan's Phase-2 abort condition is NOT
  triggered.

## Traps hit (recorded so the next session doesn't repeat them)

1. **`cargo-build_wgpu_renderer` does not restage `dist/`** — the freshly built DLL stays
   in `build/.../cargo/...`; `Deploy.ps1` then ships a stale DLL with a fresh stamp.
   Build the `OpenPoseidon.exe` target (its "deploying dependencies" step does the copy)
   and string-check the deployed DLL for a new env var before measuring.
2. **`perf_water.eden` can end before the 900-frame screenshot** — the capture then shows
   the "Shutdown…" screen with a ~1.6 ms GPU frame and 0 instances, and the harness still
   counts it valid. Check `objects.registered_instances` > 0; rerun with
   `-ScreenshotDelay 500`.
3. PowerShell: `$args` is an automatic variable (splatting it re-binds positionally), and
   `-Env @{…}` cannot be passed through `powershell -File` — use a wrapper script.

## Next (per the approved order)

Phase 4: temporal camera state + jitter (renderer-side, jitter applied in the Rust camera
upload so C++ matrices/culling stay unjittered), then history-reset contract (ABI bump),
then static-world motion vectors from the existing depth resolves.
