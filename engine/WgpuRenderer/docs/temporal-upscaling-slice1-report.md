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

## Phases 4/6/7 — temporal camera, jitter, static-world velocity (commit 14b8807a)

Landed after the owner approved the slice-1 visuals (2026-08-29 evening):

- `WGR_TEMPORAL=1` (OFF by default — jitter with no accumulator is shimmer): 8-phase
  Halton (2,3) sub-pixel jitter folded into the main scene camera's projection at the
  single camera-ingest point in `lib.rs`. C++ matrices, culling truth, shadow cascade VPs
  and the planar-reflection camera stay unjittered by construction.
- Renderer-owned previous-camera history (`temporal.rs`), resets on first frame /
  render-size change (the explicit engine-event reset contract of plan Phase 5 is the
  next ABI change, together with `object_id`).
- Static-world velocity pass: Rg16Float at RenderExtent, reprojecting the frame-final
  far-depth resolve through unjittered current/previous matrices with camera-relative
  rebase (`cam_pos` delta), f64-inverted like `Frame.inv_view_proj`. Sky pixels fall out
  of the same math via the stored-depth clamp (d = near/dist → 1e-7 is "hundreds of km").
- `WGR_TEMPORAL_DEBUG=1`: velocity-as-colour overlay after tonemap, under the UI;
  history resets flash blue for one frame.

Validation:
- Headless device test renders the real pass and reads texels back: static camera → 0,
  yaw → horizontal velocity, strafe → horizontal velocity (the arm that would catch a
  cam-delta that never propagates). An all-zeros output cannot pass.
- In-game (1080p windowed, deploy 14b8807a): `perf_abel` static camera → uniform neutral
  (zero velocity), HUD drawn over the debug view (UI is outside the temporal path);
  `abel-walk` during its movement phase → the expected radial flow field: strong
  outward/downward motion on the near ground fading to zero at the vanishing point,
  smooth and coherent. NOTE: the walk mission is static for its first ~60 s
  (settle 30 s + static 30 s) — a capture before frame ~2600 photographs the static
  phase and says nothing about motion.

## Phases 8/9/12 — object motion vectors + debug resolve (commits 13411ee8…3aacf518)

- **Rigid motion (H):** the scene draw loop brackets each object (`DrawSortObject` RAII —
  proxies inherit the parent), `Engine::SetDrawObject` is a non-virtual hint like
  `_passKindHint` (no vtable change), and EngineWgpu maps the pointer to a stable 16-bit
  id riding bits 16-31 of `WgrDraw3D.misc` — the lane material-debug already shares, so
  layout and ABI hash are untouched (`_pad` was NOT free; it carries debug bits 0-8).
  Temporal keeps id → ABSOLUTE world (camera-relative worlds change on every pan);
  movers re-rasterise in a gfx3d-owned velocity pass (mirrors `draw_one`'s pool
  addressing; fs depth-compare against the far resolve stands in for a depth attachment).
- **Skinned motion (I):** the compute skin-bake output ping-pongs; last frame's baked
  vertices ARE the previous pose (no previous palettes, per-limb motion falls out of the
  bake). Association is (object id, mesh) → baked base_vertex — palette slots are
  per-frame arena indices and cannot carry identity. VS-skinned fallback stays excluded.
- **Debug resolve (K):** reproject-along-velocity + 3×3 clamp + 90/10 blend into a
  ping-ponged HDR pair (`WGR_TEMPORAL_DEBUG=2`), Reinhard'd for display. Deliberately
  minimal; not a production TAA.

Validation (deploy 3aacf518, all in-game via the capture harness):
- Purpose-built mission (`.tmp-movecam/missions/abel-vehvel_perf_abel.abel`): static
  camera + driverless jeep on a `setVelocity` oscillation + AI soldier on a patrol loop.
  Velocity debug view, quantified per pixel: jeep patch 3469 px at R=132 (rightward),
  soldier 158 px at R=114 (leftward) against a perfectly neutral (127,127) background —
  rigid AND skinned object motion are real, correctly signed, and confined to the movers.
- Accumulator on static `perf_abel`: fences/barbed wire crisply resolved, no double
  edges, no judder — which is the end-to-end proof of the jitter sign, velocity scale
  and reset contract (any of them wrong is immediately visible here).
- Accumulator under fast lateral camera motion: coherent image; high-frequency grass
  shows the expected minimal-TAA smear (the neighbourhood clamp is permissive there) —
  this is what the reactive mask (Phase 11) and a real upscaler exist to fix, and it is
  the reason the plan forbids polishing this pass.
- False alarm worth recording: the movecam cutscene draws its own all-round dark
  vignette — it is in the PLAIN frame too (corners RGB ≈ 3,4,4 with temporal off).
  Diagnose debug-view anomalies against a no-debug capture of the same pose first.
- Headless device tests now cover every lazily-built temporal pipeline (fullscreen
  velocity + readback both ways, rigid+skinned object pipelines, resolve + history view).

Traps added to the ledger: `cargo build` does NOT restage `dist/` (the `OpenPoseidon.exe`
target does) — a Deploy after a cargo-only build ships a stale DLL under a fresh stamp;
caught here by the deploy stamp predating the code being tested.

## Next (per the approved order)

Phase 10/11: reactive/no-history policy for water, particles, wind-swayed foliage
(engine-owned mask, vendor-translated later). Phase 13: the `TemporalUpscaler` trait
(Native/TemporalDebug as first impls). Phase 14: DLSS SR via wgpu-hal/NGX Vulkan —
NOTE: needs the NVIDIA DLSS SDK on this machine, which the owner must download/license.
