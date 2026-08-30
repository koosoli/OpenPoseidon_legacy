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

## Phase 14 — DLSS Super Resolution RENDERS (commits b856cb99…dc90f2b9, deploy dc90f2b9)

DLSS SR runs in-game, end to end, on the first bring-up day:

- **SDK**: public clone of github.com/NVIDIA/DLSS at `<main checkout>/.tmp-dlss-sdk`
  (gitignored; owner authorized). `dlss` cargo feature / `-DPOSEIDON_DLSS=ON` — OFF by
  default, GPL posture unchanged; the plain build is byte-identical in behaviour.
- **Device**: WGR_DLSS=1 routes instance/device creation through ash + wgpu-hal
  `from_raw`/`device_from_raw` with wgpu's extension lists UNIONed with NGX's; strict
  fallback to the native path on any error. Booted the real game first try.
- **Init**: `Init_with_ProjectID` + a 'static-leaked `FeatureCommonInfo` (exe-dir snippet
  path + verbose log callback) + absolute data dir. The driver store on this box ships
  `nvngx_dlssg.dll` (frame gen) but NOT the SR snippet — the app-local `nvngx_dlss.dll`
  is load-bearing, and Deploy.ps1 now ships the release snippet automatically whenever
  the deployed renderer exports NGX symbols.
- **Evaluate**: wgpu forbids mixing raw-hal and wgpu encoding in one encoder → the frame
  submits in three parts at the upscale point (scene + `transition_resources`, raw NGX
  encoder, rest of frame). Inputs are exactly the temporal contract: post-DoF resolved
  HDR, far-depth resolve (DepthInverted), velocity (MVLowRes+MVJittered, scale =
  render extent), 1x1 exposure texture; output is `hdr_upscaled` (now storage-capable),
  which the post chain already read — DLSS dropped in precisely where the bilinear blit
  was, and the blit remains the fallback (latched, logged once).
- **Measured** (perf_abel, native 3441×1440): native 31.99 ms / 28.6 fps → **DLSS
  Quality (2305×965) 25.08 ms / 35.4 fps (+24 %)** — ~2.4 ms over the bilinear-67 path,
  buying near-native sharpness. 1080p Quality capture visually verified (fences, grass,
  HUD); release snippet renders without the dev watermark.

Bring-up war stories (recorded because each cost a debug cycle):
1. **A NUL byte in the source**: the initial `".\0"` string literal was written as a
   REAL NUL into the file; Rust compiles that silently, and every later textual patch
   of the init body missed without erroring — so app id / FeatureCommonInfo "changes"
   were never actually tested, while NGX read an empty path and reported
   FeatureNotFound with zero log output. GetFeatureRequirements (which said
   "supported") was the wedge that broke the false picture. Grep for `Binary file
   ... matches` is the tell.
2. The headless NGX device test (init + capability probe, seconds per iteration) is
   what made the diagnosis tractable — game launches were 90 s each.
3. Repeat-2 of the native DLSS bench caught the post-mission idle (cpu_avg 0.05 ms,
   "18445 fps") and the cheapest-repeat rule quoted it; the water-mission timing trap
   in new clothes. Always sanity-check cpu_frame alongside the GPU total.

## Phase 10/11 v1 — history-control mask (commit 657a0122, deployed)

Engine-owned reactive semantics, fed to DLSS as the SDK's TransparencyMask (this SDK
generation has no separate "reactive" input): sky/cloud pixels (far-clear depth) at
WGR_REACTIVE_SKY=0.4, water-surface pixels (opaque depth reconstructs below sea level)
at WGR_REACTIVE_WATER=0.3. Costs nothing measurable; 0-strength terms skip the pass.
Play-session tuning also landed first: jitter to DLSS in pixel space (Y flip),
sky/underwater rays from the PREPARED (jittered) camera, MVJittered flag dropped,
phase count ~8x(out/render)^2, and mip bias defaulting to HALF log2(scale) — each one
found by the owner playing and each a one-line convention truth, which is the §21
"validate inputs, never tune around them" doctrine paying out. Owner-confirmed state:
vibration gone, sharpness good; residual shadow/sky ghosting is what this mask targets.
Follow-ups (documented, not faked): wind-sway vegetation vectors or masking, particles,
cloud-shadow-delta masking, and the owner's §6.7 live dev-panel tuning surface.

## Owner acceptance + the exposure endgame (2026-08-30, commits …7b77aadc)

The owner's three-arm blind test closed the ghosting hunt:
- **DLSS auto-exposure wins** (now the default): shadows clean, only slight cloud
  ghosting left — and the NATIVE control arm showed the same cloud ghosting and the same
  building/tree wobble under fast movement. **Parity with native reached, at more fps**;
  the wobble is pre-existing engine behaviour (LOD/geometry), out of scope here.
- En route, the exposure gap was found (tonemap = manual x auto texture; DLSS saw only
  the texture — DLSS.Exposure.Scale now carries the manual factor for the non-auto arm),
  and MV scale moved to output-pixel space (no visible delta either way — recorded, knob
  kept). TransparencyMask on this snippet generation is visibly inert — recorded.

**The §6.7 dev panel is live** (Temporal tab beside Tonemap): temporal/DLSS toggles,
render scale 50–100 % (live, with explicit history reset; 100 % = DLAA), mip bias
auto/manual, reactive strengths, jitter phases, convention probes under a leave-alone
header, all beside an fps readout and an info block. The owner tunes without rebuilds.

## Default-on, Phase 13, DLAA, skinned-MV fix (2026-08-30, commits 972d1fdd…)

- **DLSS is baked into the exe** (972d1fdd): in a dlss build the NGX route, temporal
  path and the owner-validated 67 % scale are the plain-launch defaults — no launcher,
  no env vars. `WGR_DLSS=0`, the panel, or any failure anywhere on the route fall back
  to the untouched native path. Zero-env smoke: route up → 67 % → feature created
  (Quality).
- **Phase 13 is formalised** (f1c4710a, `src/upscaler.rs`): a vendor-neutral
  `TemporalUpscaler` trait + `UpscaleContext`, renderer holds an ordered backend chain
  ([DLSS, Bilinear], first `Done` wins). Every NGX object, raw device handle and
  failure latch lives inside the DLSS backend; FSR 3.1 / XeSS / DLSS 5 are now "one
  more impl", which is the whole point of the phase. "Native" is the absence of the
  chain (no upscale target at 100 % without DLAA).
- **DLAA is reachable**: at 100 % scale the upscale target now exists whenever the DLSS
  route is live, so DLSS runs as a pure temporal-AA pass (render == output). The
  panel's scale slider at 100 % = DLAA, exactly as its tooltip promised.
- **Skinned motion vectors were silently OFF in play** — the `skin_bake=false` gate
  log was real: compute skin bake was opt-in, and without it the ping-ponged bake
  buffers Phase 9 reads never existed, so soldiers got camera-only velocity. The
  attempted fix (bake default-on with temporal, f1c4710a) **shipped a regression and
  was reverted the same night** (be94e6d7): enabling the bake explodes all skinned
  geometry into full-screen triangle shards — the owner saw it live, and the §25
  matrix photographed it in every DLSS arm. Probes proved the fault sits in the
  bake/draw path itself (shatter persists with DLSS off, temporal off, gpu_driven
  off: `final-matrix/probe-bake-*`), i.e. the compute bake was broken all along and
  its default-off status had been hiding that. Until it is root-caused (follow-up task
  filed), skinned characters ride rigid/camera velocity — the state every
  owner-validated session actually ran. Lesson recorded: *a default flip is a
  feature's first real deployment — screenshot it before shipping it.*
- The NGX headless test was repaired (it runs from `target/debug/deps`, which has no
  snippet DLL — `WGR_NGX_SNIPPET_DIR` override, pointed at the SDK copy by the test),
  and the literal NUL byte in `feature_requirements`' wide-string terminator is gone
  (raw NULs make grep call the file binary; that failure mode already cost hours once).

## Phase 10/11 closure — difficult content posture (recorded, not faked)

Per plan §10/§11 the rule is *reactive/no-history over false vectors*. Shipped
history-control producers: **sky** (far-clear depth test) and **water** (seabed
reconstruction below sea level), both live-tunable. Deliberately deferred, with the
hooks in place (`misc` id lane + the R8 mask channel): wind-swayed foliage (vertex
sway in shader — no artefact surfaced in any owner session; A2C stipple under jitter
never materialised as shimmer), rain/particles/smoke, rotor discs, muzzle flashes.
DLSS's own auto-exposure + clamp handled all of these acceptably in play; none of the
owner's three reported artefact classes traced to them (vibration = jitter conventions,
sharpness = mip bias, ghosting = exposure). If one surfaces later, the fix is a mask
write in the relevant pass, not new architecture.

## §24/§25 — final validation matrix (installed a211baeb, fullscreen 3441×1440)

Modes: native (no temporal), bilinear 67 % (spatial-only control), DLSS Quality 67 % /
Balanced 58 % / Performance 50 %, DLAA 100 %. 4 GPU-bound scenes, 2 repeats each;
perf_combat excluded — it is CPU-bound (GPU 25–39 % in Phase 0) and no upscaler can
help it.

Avg fps (CPU-side `cpu_frame_phases_ms.avg_fps`, best of 2 valid 256-frame captures;
the GPU frame-total timer straddles DLSS's 3-part submission split, so wall-clock is
the honest number). Percentages vs native. Data: `.tmp-farfield-bench/final-matrix/`.

| Scene | Native 100% | Bilinear 67% | DLSS Q 67% | DLSS B 58% | DLSS P 50% | DLAA 100% |
|---|---|---|---|---|---|---|
| abel  | 28.4 | 37.9 (+34%) | 35.4 (+25%) | 38.1 (+34%) | 36.5 (+29%) | 26.3 (−7%) |
| town  | 37.6 | 53.1 (+41%) | 45.9 (+22%) | 50.1 (+33%) | 47.4 (+26%) | 33.8 (−10%) |
| water | 20.0 | 27.6 (+38%) | 26.2 (+31%) | 28.7 (+44%) | 29.0 (+45%) | 18.8 (−6%) |
| field | 36.9 | 52.1 (+41%) | 46.0 (+24%) | 50.5 (+37%) | 46.7 (+26%) | 33.8 (−9%) |

Readings: (1) DLSS Quality costs ~2–7 fps of the bilinear win — the ~2.4 ms NGX
evaluate — and buys near-native sharpness for it; the shipped default (67 %) is the
right call. (2) Below ~58 % the CPU limit starts to bite (abel/field P50 < B58):
Performance mode has no headroom left to convert, so there is no reason to ship below
Balanced on this box. (3) DLAA costs 6–10 % vs native for the best image of the set
(the 4×MSAA scene plus a full-res DLSS pass) — a quality option, not a perf one.
(4) Every capture of every arm was eyeballed clean; this same table photographed the
skin-bake regression before the rerun, which is why the PNG review is not optional.

## 2026-08-30 endgame: Graphics tab, hardware gate, skinned MVs live (728f4035)

- **Tester fallback fixed**: the NGX route now asks the driver (GetFeatureRequirements)
  whether DLSS is really supported BEFORE the defaults flip on — AMD/Intel boxes got the
  generic Vulkan extensions and booted into a jittering 67 % bilinear. A runtime death
  of the backend also rolls its auto-defaults back. Fallback = untouched native, always.
- **Dev panel "Graphics"** (renamed from Temporal): one-click native, scale to 200 %
  (SSAA; DLSS skips above 100 %), MSAA Off/2x/4x/8x (next launch), "Als Standard
  speichern" → graphics.cfg (dlssMode/msaa/scale). The Options page's AA row works for
  the first time (pre-engine env seed; SetMsaaSamples/SetRenderScale were no-ops).
- **Skin-bake shatter root-caused enough to ship**: kill-door probes proved the shards
  are the PALETTE draws under bake-as-draw-source routing (a rest-pose passthrough bake
  still shattered → the draw side reads something other than what the bake writes), and
  the visible weapon is CPU-skinned and never needed the bake. Draws keep VS skinning;
  the bake now feeds ONLY the velocity pass, keyed (palette_slot, mesh); default-on with
  temporal. Boot log proves the path: "skinned velocity live: 2 skinned draw(s)".
  The dead draw-side read remains a filed follow-up (bake-as-draw-source is disconnected,
  not deleted).

## Definition of Done — reconciliation

| Plan item | State |
| --- | --- |
| Resolution split, native-res UI/HUD | done (ee5a0d24), regression-free at 100 % |
| Mip bias | done; default = half log2(scale) (owner A/B), panel/env override |
| Jitter + reset contract | done (14b8807a); phases ≈ 8·(out/render)² |
| Motion vectors: static/rigid/skinned | static+rigid done; skinned implemented but parked behind the broken skin bake (see regression note) |
| Reactive/history control | sky + water shipped; rest recorded above as reactive-by-DLSS |
| Debug resolve | done (WGR_TEMPORAL_DEBUG=2), retained as diagnostic |
| TemporalUpscaler abstraction | done (f1c4710a, `upscaler.rs`) |
| DLSS SR in-tree, feature-gated, clean fallback | done; default-on in dlss builds, GPL builds unaffected |
| DLAA | done (a211baeb wiring); measured below |
| Live tuning surface (§6.7) | done (Temporal tab) |
| Validation matrices (§24/§25) | this section |
| Options-menu plumbing for players | NOT done — dev panel only (next milestone) |
| FSR 3.1 / XeSS backends | NOT done — trait boundary ready (next milestone) |
| Frame Generation / DLSS 5 | tracked only, per plan |
