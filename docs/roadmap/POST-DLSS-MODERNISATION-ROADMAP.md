# OpenPoseidon post-DLSS modernisation roadmap

**Status:** Living plan  
**Parked work:** everything deliberately not being worked on, with the reason and what
would unpark it, is on one page: [PARKED.md](PARKED.md). Add to it when you park
something; delete the entry in the same commit as the work when you unpark it.  
**Created:** 2026-08-29  
**Scope:** Work that begins after the current DLSS integration reaches its ship gate  
**Revision:** v2 — ownership/ABI hardening, renderer-history correction, task QoS,
determinism rules, AI simulation LOD, physics convergence and parallel critical paths

This document turns the longer engine-modernisation roadmap into an executable
post-DLSS sequence. It accounts for work that OpenPoseidon has already completed,
keeps short, low-risk improvements separate from major architectural changes, and
uses measurements as the gate for optional GPU features.

## Where the critical path stands, 2026-09-01

**Step 9 (bounded, measurable render/simulation overlap) is DONE**, so the two tracks below
it now run concurrently rather than in sequence. `WGR_RENDER_THREAD=1 WGR_RENDER_OVERLAP=1`,
toggleable in the dev panel, proven frame-equivalent (901 of 901 scene frames identical to
the inline path, aligned by simulation tick) and measured: frame mean 43.50 -> 33.91 ms and
p95 46.97 -> 36.07 on `perf_combat` (PERF-018). `swap` falls 69%%, `sim:step` rises 84%% --
the producer stops parking in the present call and starts competing for cores, which is why
the gain is half the ceiling PERF-012 predicted rather than all of it.

Landed on the tracks since:

| | |
|---|---|
| **12S** | `AIGroup::CreateTargetList` is a resumable six-stage machine bounded by a COUNTED budget, never a clock; resume state serialized. Unbounded is byte-identical to the previous behaviour; a tiny budget diverges at a named tick in a discrete field. |
| **12S** | The near-target scan is rationed at last (PERF-017). `_trackNearTargetsTime` had been written since 2001 and read by nothing. Default 0.5 s: `track` 3.07 -> 0.27 ms/tick, AI scan 3.82 -> 1.15. It CHANGES behaviour, so it ships with an AI tab that puts it back to 0. |
| **12A** | `WGR_OBJECT_STREAM_PRELOAD` pays the object fill-in up front: full residency in 7-8 update rounds against 20-21 on Stratis. Off by default. |
| **Phase 14** | The stall detector is no longer inert -- three scopes, and it names one in a real run. |
| **Phase 13** | An AI BEHAVIOUR oracle exists (`POSEIDON_AI_TIMELINE` + `PoseidonTools timeline diff`), which is what made the two 12S items provable rather than plausible. |

**The measurement discipline moved further than the code did.** Four instruments were found
lying about what they measured: `frame_max` was the harness's own screenshot, a whole
decision document's headline rested on it; the AI oracle's first version could not tell two
units apart; `dist/` staging fails silently while the game runs; and a run-to-run spread of
21.7 vs 15.1 ms means one measurement pair proves nothing. Every one of those was caught by
a control, not by reasoning. See `scripts/README-farfield-bench.md`.

### Evening addendum, 2026-09-01

The day's second half went to the heavy-world frame and the loader, driven by the owner
playing DayZ Chernarus+ live and reporting what he felt:

- **`land:obj` fell 80% in three measured cuts** (13.2 -> 5.9 -> 2.65 ms): first the
  per-section string classification (paths re-parsed per frame, a quarter-million heap
  allocations), then the per-section binding re-resolution. Neither was submission cost;
  batching remains untouched and unneeded at this scale. `sim:step` is the largest CPU
  phase now, and PERF-019 attributed it: one unrationed walk, `SelectInterestingTarget`,
  is 94-96% of `unitThink` on both worlds, and stock Everon's target list is 2.4x
  Chernarus+'s. Its ration (`POSEIDON_AI_WATCH_PERIOD`, PERF-020) follows on the same oracle.
- **Loading was mostly a logo.** The loader waited out a hard-coded 8-second splash after
  the world was ready: Everon 7.59 -> 1.29 s, Chernarus+ 8.11 -> 2.36 s once the wait
  became a ceiling. Cutting it short stranded the title effect and swallowed all input --
  found by the owner, proven by ablation, fixed and made observable. The splash is now off
  by default (`--splash` restores it). A second claimed second (progress-refresh throttle)
  was re-measured with a proper cold/warm control and does not exist.
- **12S is answered: no AI parallelism.** After the rations the AI stage is 8-14% of the
  frame against a then-46-51% object build; the ration took the win parallelism was
  proposed for. Verdict and numbers in the AI measurement report.
- **The FP_269 audit is fully closed** -- every item landed or declined with a recorded
  reason (ARCH-001). Last two: precise sleep landed in the frame pacer; the substitution
  bank declined as a triple duplicate.
- **Toggle crash fixed**: stopping and restarting the render thread left `_renderQuit`
  set; both halves now reset the whole handoff.
- Open, filed as a task chip: a deterministic physics worker-readback hash mismatch,
  pre-existing from Aug 31, order-dependent in the suite.

### Audit revision, 2026-09-01 (AUD-001)

A read-only audit of the code against the stated goal — *a clean, modern engine compatible with
OFP, Arma 1, Arma 2, Arma 3, DayZ and Reforger maps and assets* — is at
`docs/roadmap/decisions/AUD-001-engine-audit-and-roadmap-revision-20260901.md`. What it changed
in this document, each with the finding behind it:

- **The simulation track is closed as planned and reopened as measured.** 10S exists (SIM-811),
  11S is done (SIM-801/803), 12S was answered "no", and 13S *is the ration* (PERF-017). What
  remains on that line is Phase 8's command/commit stage and Phase 13's replay — and the first
  blocker of both was not AI: the object-stream admission budget was a `steady_clock` ceiling
  and the prefetch lead a wall-clock dt, so the resident set was machine-dependent even under
  `POSEIDON_RNG_SEED` + lockstep. SIM-813 removed the last wall clock in the *AI stage*; it was
  not the last one that decided what the world contains. **Closed 2026-09-02:** admission is
  counted by default (`WGR_OBJECT_STREAM_ADMIT`, cold models per update rather than
  milliseconds; `=timed` restores the clocks) and the prefetch estimator integrates
  `Glob.time`. The resident set on a tick is now a function of the tick. The remaining wall
  clock that reaches a world decision is the LOD width (`_lodInvWidth` from the measured
  frame time), which is a rendering decision and belongs to the LOD governor line.
- **Compatibility gets its own track (D, below)** with per-generation gates. Today the
  cross-generation smoke skips on CI by its own header, DayZ has four unit tests and no
  capability-matrix row, Reforger has no matrix row at all, and no generation has a decision
  record about its config format. The readers exist; no gate would notice one breaking.
- **The next three measured targets are named** (Phase 3, "Next targets"), each with the number
  that justifies it and the control that gates it. Grass stays closed by PARKED.md's own rule.
- **Phase 3.1 gains four measurement rules** from the instruments found lying since the last
  revision — `frame_max`, the missing low percentiles, GPU regions that report one occurrence of
  many, and sidecar counter blocks whose 0 means "not armed".
- **Status corrections**: FSR 1 shipped (`74d891d6`); the upscaler boundary is substantially done
  (REN-UPS-001 §7) with three convention items carried; the stall detector has call sites.

## Current baseline

The following foundations already exist and should be extended rather than rebuilt:

- whole-engine 60 Hz fixed-step simulation;
- the process-wide enkiTS task pool;
- CPU and GPU performance tracing;
- GPU-driven culling, LOD selection and indirect drawing;
- an HDR WGPU renderer with depth prepass, CSM, GTAO, water, vegetation and far-field systems;
- asynchronous work in selected terrain, object-streaming and audio paths;
- an OpenAL Soft backend with 3D sources, Doppler, EFX/EAX reverb, voice budgeting,
  asynchronous decode and a dedicated streaming pump;
- format readers for Arma 1, Arma 2, Arma 3, DayZ and Reforger content — readers, not gates;
  what each generation actually loads, and what nobody has measured, is track D's table.

The principal missing architectural boundaries are retirement of the GL33 fallback,
immutable render extraction, safe render/simulation overlap, a coherent asynchronous
asset pipeline, an explicitly dependency-driven simulation task graph with deterministic
observable effects, and reduced dependence on global mutable world state.

The worker execution order itself does **not** need to be deterministic. Work stealing is
allowed to schedule ready jobs differently from run to run; correctness comes from
immutable inputs, isolated outputs, stable iteration/reduction rules and deterministic
commit boundaries.

## Phase 0 — Finish and stabilise DLSS

Do not begin another large renderer rewrite after the first successful DLSS frame.
DLSS is complete only when it is stable during real gameplay.

### Ship gate

- Quality, Balanced and Performance modes work correctly.
- Object, vegetation and animation motion vectors are validated.
- Camera cuts, teleports and large discontinuities reset temporal history.
- Window resize, fullscreen changes and render-scale changes are safe.
- Exposure input and colour-space placement are validated.
- HUD and menus render at output resolution after reconstruction.
- Foliage, particles, water and transparencies have explicit ghosting tests.
- Native-resolution and DLSS GPU timings are recorded.
- Unsupported hardware falls back automatically without breaking renderer startup.

## Phase 1 — Low-hanging fruit

These improvements are intentionally isolated. They can deliver value before the
larger ownership and concurrency projects begin.

| Item | Effort | Potential value | Decision |
|---|---:|---:|---|
| Generic upscaler boundary | Low–medium | High | **Substantially done** (REN-UPS-001 §7): one `TemporalUpscaler` contract over Native/FSR 1/DLSS. Carried: the colour-space and depth conventions are still implicit (its §6 items 1-3) |
| FSR 1 fallback experiment | Low | Medium if accepted | **Shipped** (`74d891d6`, `rust/src/fsr1.wgsl`); in the Options menu and the chain `[DLSS, FSR1, bilinear]` |
| Asset-request deduplication | Low | Medium | **Measured null** (below): 3 duplicate refusals in 872 puts |
| GPU-upload frame budget | Low–medium | High | **Measured null** (below): the worst frame is one object; lever ships off |
| Streaming trace counters | Low | High | **Done** — with the caveat that the `streaming.texture` block is armed by `WGR_TEXTURE_STREAM_STATS` and carries no `valid` sibling (AUD-001 A4.8) |
| HRTF capability probe and headphone option | Low–medium | High for headphones | Implement as a bounded audio slice |
| World-driven EFX environment selection | Low–medium | Medium–high | Implement with smoothing and fallbacks |
| Expand existing indirect drawing | Medium | High when CPU-bound | Profile first |
| Forward+ clustered lighting | Medium | High with many lights | Use a measured light-cost gate |
| Mesh shaders | High/experimental | Uncertain | Defer |
| Complete `RenderSnapshot` boundary | High | Very high | First major architecture milestone |

**Measured 2026-08-30** (branch `asset-obs-001`, Reforger Everon 30 m/s traverse,
1280×720, capture `.tmp-movecam/out/asset-obs-vehicle30-r2`, build `f4b9fdb0`):

- **Streaming trace counters: DONE.** The texture-upload split, prepared-texture store,
  cold-model preparer and residency counters now reach the dev panel ("Streaming" tab),
  the `--capture-metrics` sidecar (`streaming{}`) and Perfetto counters under
  `--perf-trace`, from one shared snapshot (`Dev::CollectStreamingSnapshot`).
- **Asset-request deduplication: measured null for the async path.** The prepared-texture
  store's duplicate-refusal counter read 3 over an entire high-churn traverse (872 puts):
  worker races on the same name effectively never collide, so concurrent-request
  coalescing there would be a lever without a problem. The synchronous name-identity
  dedups (texture bank, shape bank) already cover the rest. What the run DID surface:
  1797 store misses (registration-time `.rvmat` stages are not in the IR, as designed)
  and 390 whole-file RGBA8 fallback decodes costing 1.1 s — those, not dedup, are the
  next texture-path wins.
- **GPU-upload frame budget: implemented as a lever, then measured null — the spike is
  one object.** Worst single frame paid 36.2 MB / 73.3 ms of texture upload work
  (43 uploads). A per-frame upload byte ceiling on admission was implemented
  (`WGR_OBJECT_STREAM_UPLOAD_MB`, commit `44e418d2`) and A/B'd on the same traverse:
  it changed nothing (peak 36.4 vs 36.2 MB, 43 uploads in both arms), because the worst
  frame is a SINGLE admitted object — `church_01.p3d`, 43 textures, 66 ms texture work
  inside a 285 ms admission (adapt 120 ms + register 149 ms) — and any admission
  ceiling's residual is `budget + one object` by construction. The lever ships default
  OFF; the wall-clock ceiling already prevents multi-object pileup. The measured target
  is therefore **per-object admission cost on the main thread** (ShapeAdapter + GPU
  registration), which is the async-admission line in Phase 6, not a budget item.
  `streaming.texture.frame_peak_*` remains the regression gate.
- **Time from request to residency (Phase 3) now measured** on the one asynchronous
  path: request→ready avg 6.4 s / max 28.3 s, request→installed avg 13.7 s / max
  42.3 s at 30 m/s, with 415 queue-full refusals and 21,001 skipped admission
  attempts — the preparer saturates far below this camera speed. Any Phase 6/7
  streaming work has its baseline.
- **Follow-up shipped (commit `1f46625a`): workers now prepare `.emat` stage
  textures** (the IR carries `materialPath`; converter-deployed materials are loose
  files, so a pure-ifstream reader keeps the worker off the file server). Same
  traverse: store misses 1,797 → 278, worker-prepared uploads 866 → 2,419 of ~3,090,
  and **main-thread texture file reads during cold admission fell 2,542 ms → 9 ms**;
  request→installed latency improved ~20%. Frame-time effect is inside run-to-run
  noise (best repeats 40.6 vs 39.0 fps against an intra-arm spread of 31.8–39.0) —
  the win is structural, not yet a measured fps win, because per-object cost is
  dominated by adapter + registration, as above. The 390 fallback decodes were
  identified as converter-written PacARGB8888 `*_mask.paa` files; the DXT exporter
  fix is filed separately.
- **Per-object attribution closed the texture-path chapter (commit `7828cd3b`).**
  A `texhdr` bucket timed the one file-server touch left on admission (PAA header
  opens in `TextureBankWgpu::Load`): 2,515 opens cost 396 ms total — church_01's
  5.5 ms of its 128 ms `adapt`. So `adapt` is real conversion work, not I/O, and the
  remaining per-object cost (`create` avg fell 30.9 → 20.8 ms/model with the
  worker-prepared stage textures — a 33% per-object improvement the frame-time noise
  hid) is ShapeAdapter + GPU registration on the main thread. **The low-hanging
  texture-path work is done; the residual is the Phase-6 asynchronous-admission
  item by measurement, not assumption.**

### 1.1 Generic upscaler boundary

DLSS must not remain a special path spread throughout renderer orchestration.
Introduce one renderer-facing upscaler contract:

```text
Upscaler
├─ Native
├─ FSR 1
└─ DLSS
```

The shared contract should describe:

- internal render extent and output extent;
- colour input, format and colour-space/pre-exposure convention;
- depth input and explicit depth convention where required;
- motion vectors, units, direction and scale where required;
- jitter offset and sequence phase where required;
- exposure or pre-exposure;
- simulation tick identifier;
- snapshot generation;
- presentation frame identifier;
- history-reset token and reason;
- optional reactive/transparency/history-control masks;
- quality mode and optional sharpening policy.

Do not design this as the lowest common denominator of Native, FSR 1 and DLSS. Add an
explicit capability description so a backend can declare which inputs it requires and
which features it supports. FSR 1 may ignore temporal inputs; that must not prevent a
future FSR/XeSS or other temporal backend from receiving the full temporal contract.

Native mode bypasses reconstruction. FSR 1 ignores depth, motion vectors and history.
DLSS consumes the temporal inputs. This boundary should permit future temporal,
vendor-neutral backends without another renderer-wide refactor.

### 1.2 Optional FSR 1 fallback experiment

FSR 1 is a candidate low-hanging-fruit fallback because:

- the reference fork contains a working integration to study;
- EASU and RCAS are small compute passes;
- DLSS already requires internal/output resolution separation;
- FSR 1 requires no motion vectors, jitter or history;
- it works across GPU vendors.

Use the other fork to study pass ordering, dispatch sizing, render-scale handling,
mip bias and UI composition. Use AMD's maintained FSR 1 implementation and licence
notices as the authoritative implementation source. Do not copy the fork's
Vulkan-specific resource management into WGPU.

Treat this as a bounded experiment after the generic upscaler boundary exists. It is
accepted only if the implementation remains isolated, provides a useful performance/
quality option on non-NVIDIA hardware and does not delay GL33 retirement, stable
handles or `RenderSnapshot`. Otherwise, retain the interface and defer the vendor-
neutral fallback until a modern temporal backend is selected.

```text
render at internal resolution
        ↓
prepare colour in the required colour space
        ↓
EASU spatial upscale
        ↓
optional RCAS sharpening
        ↓
UI at output resolution
```

The exact position relative to tonemapping must be verified against the selected FSR
integration and with visual captures.

FSR 1 does not replace MSAA. DLSS also does not automatically require upstream MSAA
to be removed:

- DLSS consumes resolved single-sample inputs, while scene rasterisation may remain
  4× MSAA with alpha-to-coverage before the HDR resolve.
- Do not disable upstream MSAA/AToC for DLSS unless foliage equivalence and the expected
  performance benefit have both been demonstrated.
- FSR 1 can work without MSAA, but it benefits from a clean anti-aliased source.
- FSR 1 plus 2× MSAA may be offered experimentally if measurements justify the cost.
- Native resolution plus MSAA remains the non-temporal quality path, particularly for
  geometry edges and alpha-to-coverage vegetation.

The OpenPoseidon reference arrangement is therefore:

```text
4× MSAA + alpha-to-coverage
              ↓
         HDR resolve
              ↓
  DLSS or another upscaler
              ↓
      remaining post + UI
```

### 1.3 Small asset-loading wins

Before replacing the complete asset pipeline:

- deduplicate simultaneous requests for the same asset;
- measure which read, decode, conversion or upload operation caused each stall;
- move expensive texture decoding onto existing workers where ownership is safe;
- cap GPU uploads by bytes and/or milliseconds per frame;
- retain recently used decoded assets briefly to avoid immediate churn;
- display placeholders instead of blocking simulation where content permits;
- cancel requests that are no longer relevant before expensive later stages begin.

**Implemented 2026-08-30** (commits `5ec08e32`, `c2ade293`, branch `asset-obs-001`):
both bounded audio slices below are in. HRTF: ALC_SOFT_HRTF probe, Off/Auto/On modes
via `alcResetDeviceSOFT` (in-place device reconfigure — the safe device-switch
sequence is untouched and re-applies the mode afterwards), persisted `hrtfMode` in
audio.cfg, a 3-way Audio-options row, and a `[.device]`-tagged test that verified
On→ACTIVE/Off→inactive on real hardware; in-game boot log shows `Auto → ACTIVE
(Built-In HRTF)`, which matches the pre-change implicit default (nullptr context
attrs = DONT_CARE). EFX: `SERoom` finally has a producer — inside a building room
(`IndoorRoomAt` at the camera, deliberately not the cruder `IsSheltered`) with size
from room clearance — and environment changes ease over a ~0.6 s time constant
instead of hard-cutting (all 23 reverb parameters, settled state costs one bool per
frame). The owner's A/B verdict on the EFX half was negative ("klingt nicht
richtig und nicht gut"), so the SERoom producer and the parameter blending are
PARKED behind `POSEIDON_EXPERIMENTAL_EAX=1` (default OFF = the pre-existing EAX
behaviour exactly); they need a tuning pass with ears before shipping. The HRTF
half stands -- a hardware probe showed HRTF was already auto-active on the
owner's device before the feature, so the new modes only add control.

**Follow-up 2026-08-31** (branch `agent-audio-efx`,
`docs/roadmap/decisions/AUD-102-efx-environment-from-world-state-20260831.md`): the
parked EFX half is still parked, but it is now testable and one defect inside it is
fixed. The classifier (`Audio::ClassifyEnvironment`) and the crossfade
(`Audio::EnvironmentBlender`) were lifted into `engine/Poseidon/Audio/` as pure logic
the engine actually calls, with 17 unit cases covering the decision table, blend
monotonicity, mid-blend retargeting and the stalled-frame cap. The defect:
`ComputeEFXTarget` overwrote density with `envSize/100`, which is right for the
outdoor 2..100 size scale but wrong for `SERoom`, whose size is in the room preset's
own units -- every interior ran at a modal density of 0.02-0.08 against the 0.4287 the
preset was authored with, i.e. the hollow/ringing end of the control. Interiors now
keep the preset's density. That is a checkable reason for part of the owner's verdict;
it is not a claim the verdict is answered, and nothing has been listened to. Also
recorded there: what the classifier genuinely cannot tell apart (mountains is a bare
170 m altitude test, not a terrain class; one cell, no enclosure or distance-to-surface
cue outdoors; `SEForest` still uses the *city* preset, 2001 parity), which is the
untuned pre-existing behaviour the next listening pass should start from.

### 1.4 Small audio wins

The current OpenAL backend is already a suitable foundation. Do not replace it merely
to claim a new audio engine. Add two bounded improvements before the full `AudioWorld`
work:

1. Probe `ALC_SOFT_HRTF`, report the selected HRTF and expose `Off`, `Auto` and `On`
   headphone modes. Device/context recreation must preserve the existing safe device-
   switch behaviour and fall back cleanly when unsupported.
2. Drive the existing EFX environment from world state. Start with compatible
   `Plain`, `Forest`, `City`, `Mountains` and `Room` classifications, but blend changes
   over time rather than switching abruptly.

HRTF applies to mono 3D sources. Music, UI, radio and other intentionally non-spatial
stereo content must retain their established presentation. Add an audible test scene
covering front, rear, above, below and moving-source localisation.

## Phase 2 — Freeze and retire GL33

`RenderSnapshot` should be designed for the engine OpenPoseidon intends to keep, not
around a second renderer scheduled for removal. Freeze and deletion are separate
gates: stop feature development first, preserve its final value as reference data, and
remove it only after WGPU has proved it can be the sole supported renderer.

### 2.1 Freeze GL33 as reference data

- Stop adding features or parity work to GL33.
- Permit only fixes required to capture final references or complete the retirement
  gate.
- Record representative GL33 images and logs for all supported content families.
- Preserve selected captures as immutable visual evidence rather than preserving the
  renderer as executable architecture.
- Make new renderer plans WGPU-only unless a retirement blocker is being addressed.

### 2.2 WGPU sole-renderer gate

Before deleting GL33, prove:

- WGPU starts without silent fallback on the supported adapter/backend matrix;
- Arma 1, Arma 2, Arma 3, DayZ and Reforger compatibility smoke tests pass;
- menus, mission loading, screenshots, headless operation and developer workflows no
  longer depend on GL33;
- release and recovery documentation no longer promises GL33 as a supported fallback;
- CI contains an explicit WGPU-start assertion and no GL33-only required job;
- important visual reference scenes have archived before/after evidence;
- an intentional WGPU initialisation failure produces a clear error rather than a
  hidden renderer switch;
- a renderer-bug triage story exists that does not require launching GL33. Today
  `--render gl33` is the working second witness that separates a WGPU regression from
  an upstream engine bug — the MP integration suite uses it for exactly that. Frozen
  reference captures, a capture-diff harness or another documented alternative must
  replace that capability before the runtime is deleted.

If a supported platform cannot pass this gate, document the concrete blocker and keep
GL33 frozen while it is resolved. Do not silently expand GL33's scope again.

### 2.3 Remove GL33 and simplify renderer architecture

After the gate:

- remove the GL33 build target and runtime selector;
- remove fallback startup paths and GL-specific configuration;
- remove renderer abstractions that exist only to support two production backends;
- preserve backend-independent engine interfaces that remain useful for server, test
  or capture implementations;
- update CI, packaging, documentation and command-line help;
- run the complete WGPU compatibility and installed-game smoke matrix.

### Gate

- Native WGPU is the only production renderer.
- No release path silently falls back to GL33.
- GL33 reference evidence remains available without retaining GL33 runtime code.
- Renderer-facing ownership work can proceed without accommodating two production
  renderer implementations.

## Phase 3 — Measure the actual bottlenecks

Extend the existing trace and capture system before changing threading or ownership.

Record:

- simulation step duration;
- render extraction duration;
- renderer preparation and GPU submission duration;
- file-read latency and queue depth;
- PAA and P3D parsing time;
- decompression and CPU conversion time;
- GPU-upload bytes and duration;
- duplicate or cancelled asset requests;
- time from request to residency;
- time that a visible placeholder remains unresolved;
- active-light count and lighting shader cost;
- indirect-draw and remaining CPU-submission cost.

Maintain comparable captures for:

- settled Everon;
- fast vehicle travel;
- entering a town;
- rapid camera rotation;
- mission start;
- an AI-heavy battle;
- a night scene containing many local lights.

Every later optimisation must identify the recorded cost that it attacks and define a
before/after acceptance test.

## The GPU frame, measured and now 95% attributed (2026-08-31)

Five stock scenes, 1600×900 windowed, DLSS off, same binary, after the atmosphere gained
timer regions and the sky's SH projection was fixed. Milliseconds of GPU frame time.

| scene | frame | unattributed | grass | grass % | sky total | shadow cascades |
|---|---:|---:|---:|---:|---:|---:|
| `perf_abel` | 13.69 | 0.40 | 4.81 | **35%** | 3.33 | 1.93 |
| `perf_town` | 7.18 | 0.30 | 0.59 | 8% | 1.76 | 0.26 |
| `perf_water` | 18.72 | 0.47 | 5.93 | **32%** | 3.35 | 1.94 |
| `perf_combat` | 19.41 | 0.39 | 7.60 | **39%** | 3.02 | 3.45 |
| `perf_field` | 9.09 | 0.58 | 1.10 | 12% | 1.56 | 0.43 |

**The unattributed block is gone** — 0.3 to 0.6 ms, down from 2.7 to 3.3 ms. It was never
inter-pass overhead or anything exotic: it was the atmosphere, which had no timer region at
all, and within it a spherical-harmonic projection running on one GPU lane at 1.812 ms a
frame. Both are fixed. The frame is now better than 95% attributed on every scene, which is
what makes the rest of this table trustworthy.

**Grass is the target, and now on measured ground rather than reputation** — 32-39% of the
frame on the three vegetated scenes, three to four times the next-largest line. That is
track B.4, and it can be worked with a denominator.

The sky total (1.6-3.4 ms, mostly the cloud march) is second. Shadow cascades are third and
scale hard with scene content: 0.26 ms on the town against 3.45 ms in combat.

### How that table was arrived at, and the two wrong turns on the way

Full record: `docs/roadmap/decisions/PERF-010-a-quarter-of-the-gpu-frame-has-no-timer-20260831.md`.
Worth reading before the next performance investigation, because both wrong turns were
plausible and one of them was written into this document as a conclusion.

The block started at 27% of the frame and took three measurements to explain:

1. **The measurement itself was wrong first.** `Water draw` and `Post: god rays` were
   missing from the timer containment table, so summing the "outermost" rows double-counted
   them — on `perf_water` the sum came to 20.485 ms against a 20.322 ms frame, a **negative
   residual**. A partition that over-counts is not a partition.
2. **"Pass-boundary overhead" was the first explanation and it was wrong.** The arithmetic
   fitted (2.8 ms ÷ ~45 boundaries ≈ 62 µs). Tested by merging Hi-Z's eleven per-mip compute
   passes into one: that region got 25% cheaper — a reproducible 0.042 ms over ten removed
   boundaries, so **~4 µs per boundary**, not 65 — and the residual did not move. Every
   boundary in the frame is therefore ~0.18 ms. **Do not merge passes for performance on
   this hardware.**
3. **The answer was that the atmosphere had no timer region at all.** The clue was that the
   residual was constant against scene cost, pass count *and resolution* (7.7× the output
   pixels moved it 12%), so the work had to be fixed-size — which the LUTs, froxel grid,
   env map and SH coefficients all are. Instrumenting them dropped the residual from 3.13 to
   1.38 ms and named a 1.812 ms pass; fixing that pass dropped it to 0.3-0.6 ms.

The same work priced **DLSS at ~1.07 ms** (1072×603 → 1600×900, RTX 3070) — the first time
this repo has had a number for it. It is invisible to the profiler because NGX's dispatches
are not ours to bracket, so it can only be measured as a difference between arms.

Also worth carrying forward: `WGR_DLSS=0` does **not** give native. It gives **bilinear**
upscaling of the same reduced render target, so it is not a native baseline.

### 3.1 Measurement hygiene

**Rule added 2026-09-03 — CPU frames under VSync are quantised.** The bench runs the owner's
saved config, which has VSync on, on a 144 Hz display: every CPU frame ends on a 6.94 ms
boundary, so `frame_avg` mixes 20.8 and 27.8 ms frames and says nothing about a 1–3 ms change.
`POSEIDON_VSYNC=0` pins the interval for a run (logged as "pinned to 0 by POSEIDON_VSYNC");
no CPU-side A/B is quotable without that line in its log. GPU regions are unaffected. The
REN-THR-012 table and the first REN-THR-013 A/B were taken without it.

**Rule added 2026-09-03 — a wait is named where it happens, never inferred from a phase.**
`swap` contains NextFrame, and NextFrame contained a `WaitRenderIdle()` that ran every frame
(PERF-021); two decision records read "GPU-bound" off that phase. The worker now prints
`idle_before=` per block (`WGR_SLOW_BLOCK_MS=1`) and carries `Worker: *` CPU rows; a worker
idle for half the frame while the producer waits is a handoff bug, whatever the GPU says.

Each of these has already produced wrong numbers at least once in this repository:

- every capture records the exe/DLL commit (`DEPLOYED-FROM.txt`) and the render
  resolution next to the numbers; a capture without both is not evidence.
  **Enforced in the artefact since 2026-08-31**: `--capture-metrics` writes a `build{}`
  block carrying the version string (which embeds the commit the running binary was
  built from), render AND output resolution, MSAA sample count, the active upscaler, and
  `dlss_route` / `dlss_active` separately. Until then the JSON recorded neither, and the
  commit existed only in the bench script's console output, which is gone as soon as the
  terminal scrolls. A rule enforced by a script is enforced only while somebody runs the
  script. Sample from the first run that had it, which also settled what the bench had
  actually been measuring: `version 3.05-f84ec7a-dev, render 2305x965, output 3441x1440,
  msaa 4, upscaler dlss, dlss_active true`;
- the farfield bench defaults to 800×600 and overstates play-resolution framerate by
  roughly 25% — its numbers must never be quoted as play performance;
- ~~one-shot GPU passes leave their timer regions registered, so `--capture-metrics`
  reports them as if they ran every frame; only the GPU frame total is re-measured and
  trustworthy for per-frame attribution~~ — **fixed, and re-verified 2026-08-31.** The
  harvest writes `-1` for any region not bracketed in the frame it resolves, so a pass
  that stops running stops reporting: a capture on Everon shows `-1` for Whitewater and
  all three Planar rows. Every GPU row is now per-frame trustworthy, not just the total.
  The CPU encode column beside it was a different and worse defect and was fixed the same
  day: it read `-1` for **all 58 regions** in every capture ever written, because
  `begin_frame` cleared the array the reader reads and the capture lands between that
  clear and the frame's `cpu_end` calls. The CPU rows are now published on frame end like
  the GPU ones, and 13 of 58 regions report real time (the other 45 are simply not
  instrumented). First honest reading: the largest CPU encode region is the 3D colour
  sub-pass at **0.08 ms** against a 25.6 ms frame — recording the frame is not where the
  CPU's time goes;
- `--dev-map` has an empty retained set, so world-space renderer features measure as
  inert there — measure world-scale features on a real world.

**Added 2026-09-01 (AUD-001 A4), four rules from instruments found lying since the list above:**

- **`frame_max` is never quoted.** It is the harness's own screenshot: `FlushPendingScreenshot`
  runs inside `NextFrame`, which is the `swap` phase, and the 256-frame ring always contains the
  capture frame (PERF-018 correction). The JSON still writes it (`GameApplication.cpp:278`) and
  `farfield-compare.py:469` still prints it; until a flag marks the shutter frame, the field is
  not a spike metric.
- **Low percentiles before the next performance decision.** `FrameProfiler` exposes avg/p95/max
  over 256 frames and nothing else (`FrameProfiler.hpp:70-74`); p95 there rests on 13 samples and
  the ring is never reset at warmup end (`Reset()` is called only by `triPerfReset`). PERF-016 names
  1% and 0.1% lows as the metric that matters. Add them, and a warmup reset, before quoting another
  mean.
- **A GPU region opened more than once per frame reports its multiplicity, or is not summed.**
  `GpuTimers::begin` keeps the last occurrence (`gpu_timers.rs:342`): "Grass shadow" is one cascade.
  Three further paths keep *every* region's previous value — nothing written (`:389-391`), ring
  saturated (`:392-398`), map failure (`:463-466`) — and `timings()` carries no frame stamp, so
  the `-1` rule above covers the per-region mask only.
- **Every sidecar counter block carries a `valid` sibling.** `objects`, `preparer`,
  `gpu_residency` and `residency` have one; `streaming.texture`, `streaming.fileserver`,
  `prepared_store`, `lights` and `snapshot.consumed_age` do not, and for each of them 0 is
  both "healthy" and "did not run" (`GameApplication.cpp:296-342,427`). Track B.1's light-cost
  gate reads the `lights` block, so today it cannot distinguish "no lights" from "path not taken".

Also recorded there and not yet fixed: `setup` is marked twice per frame and `Mark()` is `+=`
(`World.cpp:913,1650`; `FrameProfiler.hpp:83-88`); `sim:step` is absent from `triPerfStats`;
`land:gnd`/`land:obj` read 0.000 rather than absent when `Landscape::Draw` is skipped; world-mode
warmup is a 25 s wall clock with no frame floor; nothing verifies the exe and the DLL are one
commit — `Deploy.ps1` stamps HEAD after a `Test-Path`.

### Next targets, 2026-09-01 (AUD-001 B.4)

Each carries its number, its scene and its gate. None is a guess about where time goes.

| target | number | scene / source | gate |
|---|---:|---|---|
| ~~`unitThink`~~ **done 2026-09-02 (PERF-020)** | was 0.95–2.06 ms/tick; the idle-watch walk is rationed (`POSEIDON_AI_WATCH_PERIOD`, 0.5 s): `watch` 0.44 → 0.05, `unitThink` 0.64 → 0.18 ms/tick on the same scene | Everon + Chernarus+ `perf_combat` | the oracle ran: same arm EQUIVALENT over 902 ticks, rationed vs unrationed DIVERGED at tick 340 in one unit's nearest-enemy distance — a behaviour change by design, named to the tick |
| ~~`land:obj` residual, **on Chernarus+**~~ **gate met 2026-09-02 (WLD-026)** | admission is counted (`WGR_OBJECT_STREAM_ADMIT`); on the WLD-025 pose counted and `=timed` reach the same 19,923 resident objects and the same frame (35.5 vs 35.8 ms, GPU-bound), the timed clocks had ended ~95% of updates, and two counted runs hold an identical resident set row for row — without the lockstep env | WLD-025 pose | what remains is `land:obj` itself (4.5–4.8 ms in every arm at this window), now measurable as one number |
| ~~sky cloud march~~ **done 2026-09-02 (REN-SKY-001)** | 2.92 → 1.85 ms at 64 steps (three runs per arm, bench window); 32 steps saves another 0.6 ms but shows grain on cloud edges | `perf_combat` | default stays 128 — the owner preferred that picture on sight; `WGR_CLOUD_STEPS=64` is the ~1 ms lever for a slow GPU |
| ~~cloud march at 128 steps~~ **done 2026-09-03 (REN-SKY-002)** | the light march samples a two-tap density (no warp, no detail erosion): 2.89 → 1.84 ms at 128 steps, GPU frame 20.3 → 18.0 (three alternating runs per arm, 3441×1440); crops identical | `perf_combat` | **reverted to OPT-IN 2026-09-03**: the owner preferred the four-tap picture on sight; `WGR_CLOUD_LIGHT_COARSE=1` is the ~1 ms lever for a slow GPU |
| ~~interior sky-visibility maps every frame~~ **done 2026-09-03 (REN-SKY-003)** | five 2048² depth layers re-drawn every frame (0.29 cull + 0.65 draw) although they depend only on the snapped camera position and the retained set; now cached per layer with an instance-table epoch, at most one stale layer per frame: 0.94 → 0.00 ms at rest, ≤0.19 moving | `perf_combat` | ON by default; `WGR_INTERIOR_SKY_CACHE=0` restores the redraw |

### Next targets, 2026-09-02 (after the three above closed)

| target | number | scene / source | gate |
|---|---:|---|---|
| **REN-SKY-004 (2026-09-03): the reflection marched the sky's clouds.** The planar water reflection called the SAME 128-step cloud pipeline as the sky, into a half-resolution buffer read back through a roughness mip chain. It gets its own `wgr_cloud_reflection` pipeline at 32 steps; the sky's 128 is untouched | `Planar: clouds` 2.55 -> 1.20 ms on an open-sea pose, 1.17 -> 0.46 on `perf_water`; `Sky: cloud march` unmoved (5.11 vs 5.12), which is the check the second pipeline went only where it was meant to. Both poses looked at: no grain, no banding. A pixel metric cannot settle it -- the sea never repeats, so the control differs by more than the arm | Everon, new `perf_shore_9200.0.eden` (standing in the sea, reflected clouds fill the lower two thirds) | `WGR_CLOUD_STEPS_REFLECTION=128` restores the old behaviour. Also measured and NOT shipped: `WGR_WATER_FFT_CASCADES` -- 4 -> 2 moves the FFT block only 0.83 -> 0.59 ms but `Water draw` 6.21 -> 4.32, so a cascade costs in the SURFACE SHADER, not the simulation |
| **PERF-025 (2026-09-03): what the CPU still draws.** `WGR_COVERAGE_NONE_CENSUS=1` tallies the GPU-path misses BY MODEL. Everon `perf_combat`: 4,105-4,406 CPU-drawn objects a frame from 38 models, of which **2,572 are footprints** (`stopa_l/p.p3d`, 60%) and ~1,100 are road segments (27%). The draw-call number is the renderer's `direct_calls` = **3,566 a frame**; the census counts classifications, and the submit path's `objDraws=270` counts only `_drawMergers`, which marks and roads never enter. Footprints are `Mark` entities with a 10 s life, one per footfall. Both are `Offset::Decal`, deliberately outside the prepassed and GPU-driven paths | `perf_combat` | **ANSWERED AND CLOSED by PERF-026 (2026-09-03): do not build it.** `POSEIDON_MARK_TTL=0` deletes the footprints outright: `direct_calls` 3,416 -> 2,425 buys **0.19 ms** of `enc: draw` (7.55 -> 7.36) and moves the frame not at all (21.20 -> 21.24), because the scene is GPU-bound and `swap` grows 6.78 -> 10.64 to absorb it. **A CPU draw call costs 0.19 us here**, so the whole complement is ~0.7 ms, not the 6.6 PERF-024 measured -- that bracket is per-SECTION and per-state-change cost, not per call. The mark cap and the shorter life are withdrawn with the batcher |
| **PERF-024 (2026-09-03): the worker's encode, attributed.** Seven CPU-only timer rows cover `render_frame` end to end. **Recording the draws is 6.4-6.8 ms** of the worker's ~10.5 ms of real work (prepass + the 3D colour sub-pass replay); setup's own work is 2.8, everything else under 1 ms together, post and tail free. The rest of the worker's block is GPU back-pressure: the swapchain acquire read 0.03 ms in one run and 7.97 in the next on the same scene | Everon `perf_combat`, VSync off, 2 runs | **narrowed by PERF-026:** fewer recorded draws is still the CPU lever, but the unit that costs is a **section**, not a call (deleting 991 calls left 97% of the bracket behind), and on `perf_combat` the CPU is not the bound anyway |
| **REN-GI-003 (2026-09-03): per-probe visibility, hybrid GI stage 3.** Probes also store per-face distance moments; the shading gathers the eight surrounding probes BY HAND and weights each by facing and a Chebyshev visibility test, so a sunlit probe outside a wall stops bleeding into the room. Defaults: spacing 4 m -> 2.5 m, interior-sky share 0.5 -> 0.25, edge fade over two probes | an indoor pose was added to the bench (the open barn on Everon, `gi_out270.eden`): interior mean luma 17.6 -> 18.1 with GI on (at 4 m it went the wrong way, 17.6 -> 16.8), frame 4% darker, probe update 0.14-0.16 ms, no seam at the volume edge | Everon barn + `perf_town` | two shader traps found, both fatal at renderer startup and both invisible to naga's validator: a `textureLoad` in a helper called from a loop, and `i32()` of a const-foldable vector component. The test that catches them builds a pipeline from the REAL object shader |
| **REN-GI-002 (2026-09-03): the sun proxy, hybrid GI stage 2.** A reflective shadow map (depth + albedo + normal, 1024² over 96 m) drawn from the sun by the GPU-driven depth path; each probe gathers a Vogel disc of it for the first bounce, so walls and trees contribute with COLOUR. Budgeted: re-rendered when its view moves, else at most once every 8 frames (keying it on the instance epoch alone would redraw every frame with a soldier walking) | probe update 0.10–0.16 ms; the proxy redrew twice over a whole `perf_combat` run; frames within spread. **The bounce is real and small**: 0.03% of pixels at 17:00, and gain 12× moves the ground's mean luma +2.3%, so it is scaled, not broken | Everon `perf_town` 12:00/17:00, `perf_combat` | `WGR_GI_RSM=0` off, `WGR_GI_RSM_GAIN` scales; **next**: per-probe visibility against leaks, and a pose with deep shade beside a sunlit wall before the gain default is chosen |
| **REN-GI-001 (2026-09-03): the irradiance probe volume, hybrid GI stage 1.** 32x8x32 probes around the camera, ambient cubes, one eighth re-integrated per frame from 24 rays against the heightfield, the interior-sky dome maps and the SH sky; shade() blends it over the analytic sky-dome ambient. `WGR_GI=0` restores the old ambient exactly; dev-panel GI tab | probe update 0.10-0.15 ms GPU; frame within spread; canopies darken their undersides, ground bounce subtle, no colour bleeding yet | Everon `perf_combat`, `perf_town` 12:00 and 17:00 | ON by default pending the owner's look; **stage 2**: a colour proxy around the camera (walls and trees with albedo, horizontal rays) and per-probe visibility against leaks; **stage 3**: screen-space contact bounce |
| **PERF-022 (2026-09-03): the last direct calls.** The water cascade preset was re-pushed every frame (five direct calls) and the timer-row reader opened the window every frame; dedup + queue + worker snapshot: lazy waits 1.00 -> 0.02/frame, `land:gnd` 6.8 -> 0.35 ms. Frame unchanged at ~22 ms because the WORKER is the bound now (18.9 ms busy/frame under contention, `swap` 9 ms ring wait); GPU-region brackets now clock their CPU encode too, and the worker has five CPU-only rows. PERF-023: the Vehicles stage by kind: `man` 3.54 ms/tick over 193 soldiers | `perf_combat` native, VSync off, 3 runs | Everon | **parked by the owner for the GI system**; when resumed: attribute the worker encode (~10-15 ms unbracketed) and Person::Simulate (18 us per soldier per tick) |
| **PERF-021 (2026-09-03): overlap was lockstep.** `FlushPendingScreenshot()` waited for the worker every frame before checking whether a screenshot was pending (REN-THR-012 put the wait first), so the producer handed over a frame and then waited for it — every frame since 2026-09-02. Check first now; `WGR_FLUSH_WAIT_EAGER=1` restores the old order | Everon `perf_combat` native, VSync off, three alternating runs per arm: CPU frame 23.8 → 21.9 ms, p95 26.9 → 21.0, fps 42.0 → 45.6, `swap` 12.0 → 3.6; traversal phases +1.7 ms from real contention; picture identical. Found by the worker's new `idle_before=` figure (13 ms idle per 7.4 ms block) and the `Worker: *` CPU rows (acquire 0.04, present 0.16) | `perf_combat` | **the producer is now the bound (~22 ms vs ~19 GPU)**: NextFrame's 3.6 ms and the traversal are the next rows, and REN-THR-013's window is on the critical path |
| ~~the draw traversal's share of render/sim overlap~~ **REN-THR-013 landed 2026-09-03: the lazy producer window.** Render/sky/terrain params, smoke blobs and sky scalars are queued in `PendingRendererSettings`; the memory readers use the worker's snapshot; every remaining direct call acquires the window itself (`AcquireProducerWindow`, counted and named in the Perf tab and the capture JSON); `InitDraw` no longer waits (`WGR_LAZY_WINDOW=0` restores it) | on Everon `perf_combat` and `perf_town` at native resolution the traversal never opens the window (0.00 lazy waits/frame) and the CPU frame does not move because both scenes are GPU-bound (`swap` 11–15 ms) — measured with `POSEIDON_VSYNC=0`, after the first A/B under VSync read nothing (Phase 3.1 rule) | Everon native; Chernarus+ ground 800×600 for the CPU-bound case (decision record addendum) | the `opener` field names whatever still opens the window on a streaming world; texture create on cold admission is the expected next row (REN-THR-009's list) |
| `land:obj` on Chernarus+ | 4.5–4.8 ms in every arm of WLD-026 (13% of a 35 ms frame), the same number on every run now that admission is counted. **Attributed 2026-09-02** (`WGR_PASS1_STATS`, probes on: 7.8 ms): `shapeDraw` 5.1 of which `drawSection` 3.47 over 7,175 calls/frame (bind 1.09 with the bind cache at 100% hits — the residual is the per-section `Load`+`EnsureUploaded` LRU marking PERF-016 deliberately left; other 1.29; alpha 0.51; refl 0.26), proxies 0.68, scan 0.31, textures 0.32; 3,765 scalar object draws and 8,313 sections **every frame**, `runs=0` | WLD-025 pose | **Answered:** `PERF pass1 ... coverage full=1166 partial=228 none=4574` — 77% of visible objects never registered on the GPU-driven path. Every `GpuModel foliage bail` line in the log is a DayZ tree or bush (`t_quercusrobur`, `t_fagussylvatica`, `t_piceaabies`, …, `retained=0`): the TreeAdv leaf-card bail, deliberately OFF (`WGR_TREEADV_RETAINED`, EngineWgpu.cpp ~6440) since it shipped on and cost buildings their shadows, pines their trunks and near trees their LOD 0. The shadow gate is fixed; the two real blockers are (1) the retained path has no equivalent of the per-draw trunk discard (`:2711`) and (2) RegisterGpuModel skips a LOD whose sections are all blend leaf-cards, so the finest registered level is coarser. Stratis measured the prize at `land:obj` 42.7 → 0.185 ms. **This is the lever, on every world with vegetation; it is renderer work, not a cache.** **Tried 2026-09-02 (REN-VEG-001):** the three regressions are fixed in code (one ownership predicate `SectionGpuOwned`, bark solid, opaque route) and the flip measured: coverage none 4987 → 150, `land:obj` 7.65 → 1.05 ms — and the GPU frame 30.3 → 49.4 ms (fps 32 → 18), because the GPU-driven cull picks LODs from a fixed `lod_inv_width` while the per-draw path is frame-time governed: 30.5 M triangles where the CPU drew 3.8 M. Default stays off. **REN-VEG-002 (same evening):** the cull already reads the live `_lodInvWidth`; the governor is blind because diverted objects leave its complexity sum, and its target band comes from `frameRate=15` (2001 default). With the retained triangles added (`WGR_LOD_GOVERNOR_RETAINED=1`, opt-in) and the range widened, the frame went 55 → 9.7 ms at 103 fps — and the trees to their 6-triangle card, which renders as pale untextured blobs on the GPU path. **REN-VEG-003 (same evening): the governor works again** — set point 40 fps (was 2001's 15), target-point drive on, retained triangles in the estimate on every path (the first version fed a CPU-only history and pinned the loop). Chernarus+ default 31 → 20.5 ms (32 → 49 fps) with the picture close to before; Everon 29 → 24 ms. Retained trees still pin the loop because the DayZ tree ladder is 4k/2k/1k/6 triangles with nothing in between — **REN-VEG-004 (diagnosed, not fixed):** the trace shows a five-second sawtooth, not a pin — the estimate is blind to the 700× step of the tree ladder, the frame spikes 13 → 55 ms, the oscillation guard holds the cards for 5 s. Damping the loop did not help and was reverted. **REN-VEG-005 (done): the cull reports what-if triangle totals at four detail scales and the governor interpolates the cost of the step it is about to take** — the sawtooth is gone (p95 47 → 31 ms, invW off the clamp). With an honest estimate the retained trees land on the same frame as CPU trees at this GPU-bound pose (20.6 vs 20.2 ms), so the default stays CPU; the lever is clean now and wins where the frame is CPU-bound. Next measurement: a ground-level town pose with AI running |
| `Objects: colour alpha-test` **— attributed and one lever landed (REN-OBJ-001)** | 2.4 ms on Everon `perf_combat` for 0.11 M triangles: albedo-only 0.24 ms, so it is per-fragment SHADING of ~40% of the screen (A2C: 0; GTAO −0.7; interior sky −0.56; shadow maps −0.3; lights 0). Vegetation cutouts now skip the GTAO and interior-sky lookups (`WGR_FOLIAGE_SCREEN_AO=1` restores); crops identical | `perf_combat` bench window | measured properly once the override reached the GPU-driven pipelines too (they have their own constants list in `cull.rs`): 2.41 ms with the old behaviour, 1.66 with the skip, three alternating runs each — −0.75 ms, 31% of the pass, mechanism confirmed. A distant-foliage lite tier (no CSM/specular/lights past 150 m) was built, measured at 1.73 vs 1.75 ms and reverted (REN-OBJ-002). **REN-OBJ-003 (census built):** 4.31 M cutout fragments per frame survive the alpha test and are shaded at 3441×1440 — about two per covered pixel — at 0.41 ns each. The shader discards, so the hardware tests depth *after* shading, and the hidden card behind every visible one is shaded and thrown away. **REN-OBJ-004 (landed, default ON):** `@early_depth_test(force)` twins of the colour entries on the write-off (prepassed) pipelines of both families, compiled only when the device has the feature — colour alpha-test 1.77 → 0.29 ms, colour solid 0.37 → 0.08, GPU frame 22.4 → 19.1 (3 alternating runs each), crops identical. **Retained trees re-measured after early-Z (2026-09-03, Chernarus+ ground 800×600, VSync off, three alternating runs):** `WGR_TREEADV_RETAINED=1` still loses — GPU frame 13.0 → 19.8 ms, CPU frame 14.4 → 20.1 — so REN-VEG-005's default (CPU-drawn TreeAdv) stands; early-Z took the cutout colour pass to 0.38 ms but the retained set's prepass alpha-test (1.07) and the unattributed remainder outweigh the 1.7 ms `land:obj` saving. **Grass does not get this lever (checked 2026-09-03):** the default blade entries (`fs_grass`, near and mid) carry no `discard`, so the hardware already tests early there; the entries that do discard (photo tufts, alpha cards) are off on Everon and Chernarus+ (`use_photo_tuft=0`, `cards drawn=0`). Grass colour's 1.73 ms is visible-fragment shading, which is why PARKED.md closed it |


Explicitly **not** on the list: grass *colour* (closed, PARKED.md: "reopen with a corpus that
measures differently" — which the fragment census of REN-GRS-001 now is; it reopened the *shadow*
pass only, and halved it: every second blade, coverage preserved, 0.69 → 0.26 ms); meshlets (10.3% of the frame is everything they could touch, ARCH-001); and mip
feedback, which stays a *measurement* task — the 87% unrequested-detail figure (`3fa5dc97`) was
taken on `perf_combat`, where the texture budget never fires, so it is a scene fact until it is
re-run on a budget-constrained world (Chernarus+ at the 20,000-item cap).

## Phase 4 — Ownership, stable handles and ABI boundaries

This phase prepares safe asynchronous work. Introduce stable, generation-checked
handles where subsystem boundaries need them, including:

- render objects;
- meshes;
- materials;
- textures;
- lights;
- effects;
- animation data.

Do **not** mechanically replace every legacy pointer. Start at boundaries where data
crosses a thread, subsystem, renderer ABI, network lifetime or deferred-destruction
boundary.

### 4.1 Ownership and authority table

Before parallelising a subsystem, document who is allowed to mutate each class of state:

```text
authoritative simulation state  → simulation/commit thread
render history                  → renderer
OpenAL objects                  → audio thread
GPU resources                   → renderer/resource owner
network socket state            → network I/O owner
physics-owned dynamic body      → physics integration boundary
```

Worker jobs consume immutable/read-only views and produce result buffers or commands.
They must not retain unsafe pointers into mutable world containers.

This table must also identify save/load and multiplayer authority. Ephemeral render,
audio or GPU handles must not silently become savegame/network identities. Persisted
identities require an explicit versioned serialization contract.

### 4.2 Version the C++ ↔ Rust renderer boundary

`RenderSnapshot` is the opportunity to make the renderer ABI smaller and more explicit,
not to grow the existing FFI organically.

Prefer:

- versioned C-compatible structs or descriptor tables;
- explicit `size`/`version` fields where ABI evolution requires them;
- stable handles rather than C++ object pointers;
- layout/alignment tests on both sides;
- no Rust type leakage into Poseidon gameplay code;
- no gameplay-class layout assumptions inside the Rust renderer.

ABI mismatches should fail loudly at startup rather than producing render corruption.

**Status 2026-08-30: substantially already built.** The FFI is handle-based (uint64
resource handles, no C++ object pointers cross), `WgrAbiCheck::layout_hash`
(`WgrLayoutHash()`, wgpu_renderer.hpp) hashes every shared struct's layout so any size
change invalidates the pair automatically at startup, and the header now states the
threading contract (main-thread-only) explicitly. Remaining for this item: the layout
hash trips on *layout* changes only — a semantic change with an unchanged layout still
slips through (the DLSS work hit exactly this once); and per-struct `size`/`version`
fields for *forward*-compatible evolution exist nowhere. Both belong to the
`RenderSnapshot` boundary work rather than a standalone pass.

### 4.3 Deferred reclamation

**Audited 2026-08-31 — every clause already met, and NOT by the mechanism a reader would
guess.** See `docs/roadmap/decisions/REN-RES-002-deferred-reclamation-audit-20260831.md`.
The renderer contains no call to wgpu's explicit `Texture::destroy()`/`Buffer::destroy()`
anywhere; handles are dropped, and wgpu's resource tracker holds the allocation until every
submission referencing it completes. The one thing wgpu cannot defer for us — a bindless
slot INDEX, which is baked into material buffers and encoded draws — is parked and
recycled only in `frame_submitted()`, after the frame's queue submit. Snapshots carry no
handles and preparer workers hold none, so those two clauses are satisfied vacuously.

The 120-frame texture eviction window is therefore **not** the "arbitrary frame-count
delay" this section forbids: it governs thrash policy, not lifetime, and lowering it can
cost an upload but cannot cost a use-after-free. That distinction is now stated at the
call site, because it is not visible from there.

Resource destruction must be deferred until no published snapshot, worker job or
in-flight GPU submission can still reference the resource.

Use explicit generations/epochs or equivalent completed-work tracking rather than
arbitrary frame-count delays.

Mutation requested by parallel jobs should be represented as commands or result
buffers and applied at an explicit ownership boundary.

## Phase 5 — Immutable `RenderSnapshot`

This is the first major architectural project after the low-hanging improvements.

> **Status, 2026-08-31.** Steps 1–5 are done and step 6's *precondition* is done; step 6
> itself (the thread) and step 7 (overlap) are not. What landed:
>
> - **The producer/consumer handoff exists**, in four drained slices — settings, retained
>   instances, retained meshes and models, overlay textures. The producer holds its own
>   handles and nothing outside one drain per frame touches the renderer handle. Design and
>   counts in `REN-THR-001`; the slices and their verification in `REN-THR-006`.
> - **The push-frame storage is double-buffered** (`84a44e79`), which also closed the
>   `_gpuInstances` race by placement rather than by a mutex.
> - **Step 6's equivalence proof is available and reaches zero.** Two runs, or two *builds*,
>   publish content-identical frames in 901 of 901 scene frames. It took a determinism mode
>   (`SIM-816`) and three clocks removed from the published frame; the gate is
>   `scripts\Check-FrameEquivalence.ps1`.
>
> **Before building the thread, read `PERF-012`.** Overlap is worth 34–42% of a *combat*
> frame and **1.3%** of `perf_abel`, the scene every performance figure in this project has
> ever been taken on. The remaining handoff work is worth doing because it makes the boundary
> true, not because it will move the bench.
>
> What still bypasses the drain is listed in `REN-THR-006`: the CPU-path `mesh_create` and
> `mesh_set_skin`, the far-proxy bulk upload, the stats *reads*, and the stop-the-world
> surface calls.

Implement it incrementally:

1. Build a snapshot while rendering remains on the main thread.
2. Make WGPU consume snapshot data instead of inspecting `GWorld`.
3. Validate visual equivalence and temporal motion data.
4. Add generation/epoch-safe resource lifetime handling.
5. Double- or triple-buffer the snapshots.
6. Move renderer preparation to a render thread.
7. Allow simulation and rendering to overlap.
8. Add presentation interpolation only after latency and temporal-history behaviour are measured.

```text
Simulation publishes snapshot N+1
             │
             ├────────────────────┐
             │                    │
next simulation work       renderer consumes N
                                  │
                          cull / prepare / submit
```

### 5.1 Snapshot contents and history ownership

The snapshot should contain only presentation-facing state, for example:

- stable render-object IDs;
- current simulation/render-extraction transform state;
- optional adjacent simulation states needed for interpolation;
- mesh and material handles;
- current skin/pose data;
- lights and effects;
- camera and visibility data;
- terrain state;
- weather, water and sky presentation state;
- world/mission generation;
- history-reset/camera-cut token;
- snapshot generation and resource-lifetime epoch.

**Do not make simulation responsible for the previous presented transform.**

Temporal history belongs to the presentation side. The renderer should keep the previous
actually presented transform, skin palette and other temporal state keyed by stable
render-object ID. This matters when:

- several fixed simulation ticks happen before one render;
- several render frames interpolate the same pair of simulation states;
- rendering runs above 60 Hz;
- a frame is skipped or presentation stalls.

The previous temporal state must therefore mean **previous presented frame**, not previous
simulation substep.

### 5.2 Avoid turning extraction into a new CPU bottleneck

A snapshot boundary does not require copying the entire world every frame.

Measure extraction cost and evolve toward:

- compact presentation structures;
- dirty/revision tracking where useful;
- retained immutable resources referenced by handles;
- bounded transient per-frame data;
- dense instance arrays where profiling proves value.

The renderer-facing snapshot should be easy to traverse and difficult to misuse.

### 5.3 Render/simulation overlap and latency policy

Do not permit an unbounded producer/consumer queue. Define a small maximum number of
outstanding snapshots so simulation cannot run far ahead of presentation and silently add
input latency.

Provide at least:

- a serial/lockstep diagnostic mode;
- the normal overlapped mode;
- visible counters for snapshot age, queue depth and renderer wait time.

When interpolation is enabled, measure the latency cost of rendering between two completed
simulation states. Camera/local-player late update or another bounded latency-reduction
technique may be evaluated later, but must not feed presentation state back into
authoritative simulation.

### Gate

- The WGPU renderer performs no arbitrary reads from live gameplay state.
- Simulation cannot modify a snapshot being rendered.
- Renderer temporal history is based on previous **presented** state.
- Deleting an object or asset cannot invalidate an in-flight snapshot or GPU submission.
- Snapshot queue depth is bounded and observable.
- Rendering and fixed-step simulation can run concurrently without changing gameplay results.
- A serial/lockstep mode remains available as a correctness oracle.
- Rendering above 60 Hz has a defined interpolation and latency policy rather than accidental behaviour.

This is safer than copying the reference fork's present overlap because dependency and
ownership boundaries are explicit rather than relying on renderer callbacks into live
world state.

## Phase 6 — Asynchronous asset system

**Status 2026-08-30 — the measured admission target moved.** The per-object
main-thread admission cost this phase exists to remove was re-measured per bucket on
the 30 m/s Everon traverse (734 cold models, `Object stream cold model:` rows). The
dominant cost was NOT the adapter: 61% of the total (12.6 s of 20.8 s) was
`wgr_model_register`, and inside it the load-time sky-visibility bake's *synchronous
GPU readback* (`device.poll(wait_indefinitely)` per model). Two commits removed it:
the bake is now submitted without waiting and collected 1–2 frames later
(`84c3ee8a`), and models under 4 m³ bounding volume — bushes, rocks, with no
interior a 16k-voxel volume can describe — skip the bake entirely (`32e07820`,
`WGR_SKY_BAKE_MIN_VOLUME_M3`). Cold admission total: 20.8 s → 7.9 s (−62%), avg
28.4 → 10.8 ms/model, worst ffi stall 173 → 30 ms. Remaining levers by measured
size: ShapeAdapter on the main thread (4.0 s — blocked on GlobLoadTexture /
GTexMaterialBank / NewProxyObject thread-affinity), mesh+section residual (~2.2 s),
and a disk cache for bake volumes (§3d).

**Update, same day: the adapter now runs on the workers** (`91226a65..52ffa8dc`).
The thread-affinity blockers were removed one commit at a time — the RStringB bank
and the fast allocator's chunk lists took locks, the bank lookups moved into
main-thread-resolved tables (`BuildAdapterBankTables`), proxy creation split out
(`CreateAdapterProxies`), and the preparer grew a convert stage
(`SubmitConvert`/`TakeConverted`, `WGR_OBJECT_STREAM_ASYNC_ADAPT=0` reverts).
Result on the same traverse: cold admission **20.8 s → 3.7 s** across the session
(avg 28.4 → 5.1 ms per model, worst stall 227 → 86 ms); the main-thread adapt
bucket reads 0.0. The remaining per-model cost is registration data marshalling +
vertex-buffer build — Phase 6's upload-budget line.

Do not copy the other fork's polling filesystem worker pool directly. Build a staged
pipeline around a dedicated I/O service, the existing enkiTS compute workers and a
budgeted renderer upload queue:

```text
Asset request registry
        ↓
deduplicated asynchronous read
        ↓
worker decode and conversion
        ↓
Derived Data Cache
        ↓
residency decision
        ↓
budgeted GPU upload
```

Blocking disk work should not consume all general compute workers. Decode and
conversion jobs may use the shared enkiTS pool once their data ownership is explicit,
but background asset work must not starve fixed-step simulation jobs. Use task priority,
reserved capacity, frame budgets or a separate low-priority decode pool if measurements
show contention.

Define workload classes such as:

```text
latency-critical simulation
render/extraction support
interactive streaming
background decode/conversion
maintenance/cache work
```

The exact implementation is evidence-driven, but the priority relationship must be
observable in traces.

Also distinguish **gameplay-critical** assets from **presentation-only** residency. A
visual placeholder may be acceptable while a texture is loading; collision geometry,
authoritative config or another gameplay dependency must not appear asynchronously in a
way that changes simulation timing between machines.

Define an explicit gameplay-readiness barrier. Required collision, configuration,
navigation and other authoritative data may load asynchronously before mission start or
late join, but the fixed-step simulation must not begin or admit the joining peer until
that data is complete, version-verified and committed at a defined boundary. Presentation-
only textures, meshes, audio and effects may continue streaming after that barrier.

Implement one vertical asset slice at a time:

1. textures and PAA data;
2. static P3D models;
3. material interpretation;
4. animation and skinned data;
5. collision data;
6. sound assets.

Each slice must support request deduplication, cancellation, error placeholders,
bounded queues and traceable state transitions before moving to the next asset type.

**Slice status, and a measurement that should stop slice 3 from being attempted.**

Slices 1 and 2 are built: the `ObjectStreamPreparer` parses textures and static models on
its own workers, with a bounded distance-ordered queue, back-pressure instead of dropping,
and traceable states (`Unknown`/`Queued`/`Parsing`/`Ready`/`NotLoose`/`Failed`/
`Converting`/`Converted`). Slice 2 also moved ODOL geometry conversion onto those workers
by splitting `convertToLODShape`'s tail.

**Slice 3 (material interpretation) is main-thread-only and should stay that way until
something measures otherwise.** What holds it there is named at
`ShapeAdapter.hpp:44` — `BuildAdapterBankTables` touches `GlobLoadTexture`, the global
`GTexMaterialBank`, and `LoadHeaders`, none of which are thread-safe. Making them safe is
real work. What that work would buy, measured on the deployed build, 2026-08-31,
`perf_abel` at 3441×1440 with DLSS active, 256 sampled frames:

| phase | avg ms | max ms |
|---|---:|---:|
| `swap` (waiting on present) | **27.171** | 1148.448 |
| `land:gnd` | 0.616 | 13.774 |
| `drw:land` | 0.430 | 1.371 |
| everything else, summed | ~0.47 | — |
| **frame** | **28.686** | — |

**95% of the frame is `swap`.** Every piece of CPU work the main thread does, material
interpretation included, sums to about 1.5 ms — and material interpretation is a fraction
of the 0.081 ms `land:obj` bucket, because the tail split already moved the expensive part
off. There is no main-thread time for slice 3 to reclaim on a stock world. Attempt it only
with a corpus that measures differently (a material-heavy foreign world is the plausible
candidate), and say which corpus in the commit.

The same table is the answer to "why is the frame 28 ms": it is GPU-bound, not CPU-bound,
which is what the grass measurements have said all along.

**A concrete first customer for the DDC: loading Reforger's Everon natively.** See
`docs/roadmap/decisions/RFG-002-loading-everon-without-converting-it-20260831.md`. Six of
the eight formats on that path already have engine-side readers and two of them (`.edds`
textures, `.emat` materials) are already wired into the runtime — which removes the
expensive half of the offline export outright. Exactly one reader is missing engine-side
(the `.et` prefab graph, ~100 lines currently in the converter), and the rest is plumbing.
Geometry conversion does not disappear, it moves to load time — which is precisely what a
Derived Data Cache is for, and why this is the right first workload to design it against
rather than a hypothetical one.

### Derived Data Cache

Legacy data should not be converted into modern GPU form on every launch.

```text
canonical source identity / addon or mod identity
+ source-file hash
+ importer version
+ compatibility-layer version
+ derived-format version
+ platform/GPU-format capability when relevant
        ↓
content-addressed cache key
        ↓
cached derived asset
```

Candidate cached products include:

- processed meshes and vertex/index data;
- converted textures and mip chains;
- interpreted materials;
- collision hulls where appropriate;
- dependency and residency metadata.

Original community assets remain untouched. Every cache entry must be disposable and
reproducible from its source and versioned import rules.

Do not assume that one monolithic cache product is portable everywhere. Prefer a
platform-independent derived layer where possible and a GPU-format-specific layer only
where required.

Cache writes should be atomic and validated. Corrupt or partially written entries must
fall back to source regeneration rather than crashing or poisoning later runs. Cache keys
must distinguish different mods/addons that expose the same virtual path.

## Phase 7 — Streaming and residency

**Measured 2026-08-31 (the denominators this phase was missing).** The streaming
snapshot now carries the renderer's GPU residency totals beside the object count
(`gpu_residency{}` in `--capture-metrics`, rows in the Streaming tab). On the 30 m/s
Reforger Everon traverse, 1280x720, build `52d58b46`:

| quantity | value | what it says |
|---|---:|---|
| resident objects / item budget | 19,963 / 20,000 | the item budget is ALWAYS the binding constraint |
| GPU tracked | 2,336 MB | of a 2,048 MB "budget" |
| bytes per resident object | **119.9 KB** | so the 20,000-item budget promises **~2.3 GB** on this corpus |
| object textures | 992 MB (1,362 textures) | 42% of tracked |
| geometry live / pool capacity | 682 MB / **1,344 MB** | **661 MB allocated and unused** — the pool doubles and never compacts |
| geometry retired | 0 MB | nothing is ever handed back |

Three concrete conclusions, in the order they should be acted on:

1. **The "budget" is not a budget.** `WGR_DYNAMIC_VRAM_MB` defaults to a FIXED 2,048 MB
   and is never compared against the device's actual VRAM, so "over budget" is
   permanently true on this corpus while a 8-12 GB card sits half empty. Probing the
   real device heap (the renderer already takes the Vulkan HAL route for DLSS) must
   come BEFORE any byte-driven admission gate — gating on the fixed guess would shrink
   the resident set on hardware that has room to spare, i.e. trade pop-in for nothing.
2. **The item budget can then become a byte budget**: `effective items =
   min(item cap, byte budget / bytes-per-object)`, with a high/low water pair as the
   hysteresis the phase asks for. The measured 119.9 KB/object is what makes this
   expressible at all; it is also the number to re-measure per corpus, since a
   texture-heavy interior world will not look like geometry-heavy Everon.
3. **The geometry pool's 661 MB of slack is the cheapest win on the list** and is
   independent of both: doubling growth with no compaction means up to 2x live at
   steady state. Shrink-with-hysteresis when live stays well below capacity.

**Implemented 2026-08-31, and what each piece measured.**

- **The budget is now a measurement.** The renderer probes the device-local heap
  through the HAL route DLSS already uses and takes 70% of it: on this machine
  **5,611 MB of a probed 8,017 MB**, against the old fixed 2,048 MB guess.
  Consequence on the traverse: `over_budget` goes **true → false** and the texture
  LRU stops evicting entirely at defaults (60 → 0 evictions), because it is no
  longer chasing a target that meant nothing. `WGR_DYNAMIC_VRAM_MB` still overrides;
  0 is still unlimited; a backend that cannot be probed logs why and keeps 2,048.
- **The geometry pool can hand its tail back** — shrink-with-hysteresis (live under
  half of capacity for 120 consecutive frames, target 1.25x live, reallocated inside
  the submit boundary so the GPU can never be reading the freed buffer). Real
  compaction is impossible without a mesh-handle rewrite pass, because an allocation's
  offset is baked into every recorded draw the moment it is handed out; the trailing
  free block is exactly the doubling slack, which is what the 661 MB was.
- **The item budget can be clamped by measured GPU cost** — but it ships **opt-in**
  (`WGR_OBJECT_STREAM_GPU_BUDGET=1`). It does the right thing where it matters (zero
  interventions at the probed budget, so a set that fits is never shrunk) and the
  wrong thing under pressure: at 1,200 MB it clamps 20,000 → 13,608 and then
  oscillates, 96 budget changes in 200 s, because shrinking the set changes the
  per-object measurement that drove the shrink. It needs a dwell time on the
  decision, a windowed cost average, and an understanding of what the release path
  does when the desired set shrinks — all cheap, none of them safe to guess at.
- **The retained-texture liveness sweep** exists but its pin exemption is also
  opt-in (`WGR_RETAINED_LIVENESS=1`): the white buildings it was built to cure turned
  out not to be an eviction symptom at all (see REN-RES-001), so relaxing a safety
  check would buy nothing measurable.

Once asynchronous loading is reliable, build a coherent residency manager.

Phase 7 does **not** have to finish before Phase 8 begins. After the shared ownership,
stable-handle and worker-QoS foundations exist, asset/streaming work and simulation-task
work should proceed as parallel architecture tracks unless a measured dependency says
otherwise.

Track explicit states such as:

- requested;
- reading;
- decoded;
- ready for upload;
- resident;
- evictable;
- failed or cancelled.

Priorities may consider:

- distance and projected screen size;
- camera position and direction;
- player velocity and vehicle speed;
- mission importance;
- asset dependencies;
- recent use and predicted reuse.

Use CPU and GPU memory budgets rather than arbitrary item-count limits. Add hysteresis
so assets near a boundary are not repeatedly loaded and evicted. The target is
predictable frame time, not merely file loading on another thread.

## Phase 8 — Deterministic-effects simulation task graph

The whole-engine fixed step is already complete. The next simulation milestone is to
make dependencies explicit while preserving deterministic **observable effects**.

The enkiTS worker execution order itself may vary. Do not sacrifice work stealing merely
to force identical scheduling order when immutable inputs plus deterministic commit can
provide the required semantics.

First run the same task graph serially:

```text
Input / network apply
  ↓
world clock and immutable query state
  ↓
perception / visibility / navigation queries
  ↓
AI decisions / vehicle calculations / animation preparation
  ↓
deterministic command and result commit
  ↓
physics barrier and ordered gameplay events
  ↓
scripts with defined observable ordering
  ↓
build and publish RenderSnapshot
```

Only after serial equivalence is proven should independent stages run concurrently.

Initial parallel candidates:

- AI perception queries;
- visibility and sensor tests;
- navigation queries;
- animation pose preparation;
- vehicle calculations that produce result buffers;
- other read-mostly spatial queries.

Jobs should produce results or mutation commands. They should not directly modify shared
world state. Commit order must be deterministic and suitable for replay testing.

### 8.1 Worker-thread numerical environment

Before gameplay simulation runs on workers, verify that every simulation worker uses the
intended floating-point environment:

- rounding mode;
- denormal/flush-to-zero behaviour where relevant;
- platform-specific FPU/SIMD state.

Do not assume a newly created worker inherits the exact main-thread numerical state.

### 8.2 Random-number ownership

Audit the legacy random-number paths before parallel AI or vehicle work.

A shared mutable RNG called from jobs can make results depend on scheduler order. Use an
explicit strategy such as deterministic per-system/per-entity streams, preallocated random
inputs, or another design that preserves the intended gameplay distribution without
making job execution order observable.

### 8.3 Stable iteration and reductions

Parallel jobs must not accidentally make outcomes depend on:

- hash/container iteration order;
- pointer addresses;
- unordered floating-point reductions;
- completion order.

Where gameplay observes a winner, target, event or reduction result, define stable tie
breaking and commit ordering.

### 8.4 Physics integration boundary

Do not assume the physics backend is safe or profitable to query concurrently merely
because the engine has worker threads.

Keep an explicit physics ownership/barrier. Parallel query access is enabled only for
operations and backend versions whose thread-safety contract has been verified. Physics-
owned dynamic objects, kinematic gameplay proxies and legacy gameplay-owned objects must
retain explicit authority.

### 8.5 AI simulation quality-of-service / simulation LOD

Threading is not the only way to scale large battles.

Measure whether expensive AI subsystems require the same update frequency at every
importance/distance tier. Reuse Poseidon's existing near/far importance concepts where
possible rather than inventing a competing world-importance model.

Evaluate reduced-rate updates independently for:

- perception;
- target rescoring;
- path replanning;
- strategic/group planning;
- expensive environmental queries.

Do **not** simply run the whole distant AI at an arbitrary low frequency. Gameplay-critical
movement, firing, timers and network-visible state need their own evidence and rules.

Every AI-LOD change requires behavioural and multiplayer tests.

### 8.6 Scheduler and determinism validation

Run representative deterministic fixtures with:

- serial task execution;
- 1 worker;
- 2 workers;
- 4 workers;
- a typical desktop worker count;
- deliberately perturbed job scheduling where practical.

Use TSan for race detection, but do not confuse race freedom with deterministic behaviour.

Record/replay should report the first divergent authoritative state boundary. Require
strict comparison for the same build, platform and deterministic fixture. Semantic or
tolerance-aware comparison may supplement cross-platform diagnostics, but it must never
permit different discrete authoritative outcomes such as event order, selected targets,
hits, deaths, ownership changes or replicated state. Tolerance is a diagnostic tool, not
a licence for gameplay divergence.

### Gate

- The serial task-graph mode matches the pre-task-graph reference behaviour.
- Parallel worker-count changes do not alter discrete authoritative gameplay outcomes.
- Any tolerance policy is confined to documented cross-platform diagnostics and cannot
  hide divergent events, decisions or replicated state.
- Global mutable RNG order is not scheduler-visible.
- Worker numerical state is controlled.
- Jobs do not directly mutate shared world state outside declared ownership.
- AI simulation LOD is independently switchable and benchmarked rather than silently
  coupled to multithreading.

## Parallel track A — Modern geometry-aware audio

This is an optional parallel track. Its bounded early slices may proceed when audio
owners are available, but it does not block renderer separation, asset architecture,
simulation parallelism, networking or world decomposition.

The current OpenAL implementation remains the output backend. The modernisation is an
engine-side `AudioWorld`: explicit ownership, spatial queries, environment state and
bounded commands feeding OpenAL Soft. It must preserve existing mission sound classes,
volumes, radio behaviour, timing and mod compatibility.

### A.1 Architecture and audio-thread ownership

Move toward one thread owning OpenAL device, context, source, buffer and effect calls.
Simulation and gameplay publish bounded audio commands instead of calling the backend
from arbitrary threads:

```text
Simulation / gameplay / scripts
             ↓
       AudioEvent commands
             ↓
    bounded priority queue
             ↓
        AUDIO THREAD
             ↓
  OpenAL sources / EFX / HRTF
             ↓
          output device
```

Typical commands include:

- create, play, stop, pause and resume voice;
- update position, velocity, direction and gain;
- change loop, pitch, category and priority;
- publish listener and environment state;
- attach or release decoded buffers;
- request device or HRTF-mode changes.

The existing stream pump and decode workers should remain separate producers. Decode
workers produce PCM; only the audio-owning thread uploads buffers and mutates OpenAL
state. Queue overflow must be visible in diagnostics and must drop low-priority
cosmetic work before speech or gameplay-critical cues.

### A.2 `AudioSnapshot` and stable voice handles

Use stable, generation-checked voice and asset handles. Publish a compact audio-facing
snapshot at a bounded rate containing:

- listener transform and velocity;
- active source transforms and velocities;
- source category, importance and audible radius;
- environment and interior identifiers;
- relevant weather, water and time state;
- stable geometry/material query handles.

Audio interpolation should smooth transforms between simulation updates. Destroying a
game object must not invalidate a voice or decoded buffer still in use by the audio
thread.

### A.3 HRTF and output profiles

Provide:

- `Off`, `Auto` and `On` HRTF modes;
- queried capability and active-status reporting;
- safe device/context recreation;
- stereo speaker, headphones and surround-aware output profiles;
- per-device persistence with a safe default;
- a dry bypass path for A/B testing.

HRTF must remain optional. Gameplay-critical localisation must still work through
ordinary stereo or speaker panning when it is unavailable.

### A.4 Bounded geometry obstruction

Do not ray-cast every audible source every frame. Select a measured budget of important
sources and update obstruction at a lower frequency. Use physics/query workers to
produce results; the audio thread consumes smoothed parameters.

Start with:

```text
clear path       → normal direct sound
thin obstruction → small gain loss + gentle low-pass
solid wall       → larger gain loss + stronger low-pass
multiple layers  → accumulated, clamped transmission loss
```

Avoid binary audible/muted behaviour. Cache results briefly, stagger queries across
frames and use hysteresis so movement near a doorway does not cause rapid filter
flapping.

### A.5 Shared physical-material acoustics

Extend the shared physical-material description with acoustic properties rather than
inventing an audio-only list of material names:

- direct transmission loss;
- high-frequency absorption;
- reflection strength;
- scattering/diffusion;
- optional thickness or class defaults.

Concrete, soil, vegetation, wood, glass, metal and water should sound different, but
missing or legacy material information must always resolve to a stable default.

The first implementation may inspect the strongest obstruction only. Multiple-path
and diffraction approximations require evidence before adoption.

### A.6 Rooms, portals and environmental reverb

Use room/portal information where reliable:

- identify the listener and source room;
- treat doors, windows and openings as portals;
- compute a bounded transmission path across connected rooms;
- use direct-path filtering separately from reverberant energy;
- crossfade EFX environments and sends at transitions;
- fall back to outdoor geography/environment classification when room data is absent.

Room dimensions, openness and surface classes should influence reverb size, decay and
high-frequency damping. Start with OpenAL EFX algorithmic reverb. Convolution reverb
is optional and requires memory, CPU and content-authoring evidence.

### A.7 Outdoor propagation and atmosphere

Add outdoor effects incrementally:

- distance-dependent air absorption, especially at high frequencies;
- terrain/building obstruction;
- vegetation-density damping;
- wind and weather influence where audibly justified;
- altitude and open-space ambience;
- water-surface and underwater transitions.

Do not apply literal physical simulation where it harms recognition or gameplay.
Important cues need controlled minimum audibility.

For very loud distant events, evaluate delayed arrival using the speed of sound. This
is appropriate for artillery and distant explosions, but ordinary close sounds should
not acquire distracting control latency.

### A.8 Gunshots, projectiles and vehicles

Separate firearm presentation into components where source content permits:

- muzzle/mechanical sound;
- local blast;
- environment-dependent reflection or tail;
- distant report;
- projectile fly-by;
- supersonic crack when geometry and projectile state justify it.

Vehicles may expose engine, intake, exhaust, transmission, tyre/track and wind layers
with interior/exterior filtering. These remain presentation layers driven by
authoritative vehicle state, not independent audio simulation.

### A.9 Voice budgeting and virtualisation

Extend the existing voice budget instead of allocating an unbounded number of OpenAL
sources. Distinguish:

- real mixed voices;
- virtual voices whose timeline continues without a hardware/backend source;
- paused or culled voices;
- never-drop speech and gameplay-critical classes.

Priority may consider category, loudness, distance, screen/camera relevance, mission
importance, age and whether the voice is already audible. Re-entry from virtual to real
must resume at the correct playback position without a click.

### A.10 Mixing, accessibility and user control

Add output controls without changing mission semantics:

- master, effects, speech, radio, music, vehicle and ambience buses;
- headphone/speaker profile;
- HRTF mode;
- reverb and geometry-detail quality levels;
- night/low-dynamic-range mode;
- optional loud-sound protection;
- clear subtitles or visual cue support for accessibility.

Cosmetic quality settings may reduce query count, reflection detail or ambience, but
must not hide gameplay-critical sounds.

### A.11 Diagnostics and acceptance gates

Expose a runtime audio diagnostic view containing:

- real, virtual, paused and dropped voice counts;
- command and PCM queue depth;
- stream underruns and decode latency;
- active output device, format and HRTF status;
- current room, portals and environment blend;
- obstruction rays, hit materials and smoothed filters;
- active EFX sends and presets;
- per-category CPU time and query budget.

Provide repeatable test scenes for:

- headphone localisation around and above/below the listener;
- walking through a door between outdoor and indoor spaces;
- a source behind wood, glass, vegetation and concrete;
- many competing voices and deterministic priority decisions;
- rapid device switching and unsupported HRTF fallback;
- streamed music/speech during a long main-thread stall;
- distant gunfire, a supersonic fly-by and an underwater transition.

### Gate

- Existing missions and mods retain their sound timing and class behaviour.
- One documented thread owns OpenAL mutations.
- HRTF capability and fallback behaviour are verified.
- Obstruction and reverb transitions are smooth and bounded.
- Audio queries cannot create unbounded simulation cost.
- Audio remains stable during render stalls, device switches and streaming pressure.
- Headless/server builds do not link or initialise an audio backend.

The broader engine roadmap already describes this direction under `AUD-000` in
[the main modernisation roadmap](modernisation/PoseidonWGPU_Modernisation_Roadmap_v4.5.3.md#aud-000--geometry-aware-audioworld--suggested-staged-capability).

## Parallel track B — Evidence-gated renderer features

These features are independent candidates, not steps that block the engine-
architecture critical path. Each requires a measured problem and an acceptance gate.

### B.0 Renderer structural cleanup after GL33 retirement

Once WGPU is the sole renderer and the temporal work is stable, use that simplification
to reduce renderer orchestration debt.

Evaluate an explicit render/frame graph for pass dependencies and transient resources,
but adopt it only if it makes the real WGPU pipeline easier to reason about. The target is
not a fashionable abstraction; it is to make pass ordering, resource lifetime and debug
capture explicit.

Incrementally break large orchestration files into ownership-focused modules. Avoid a
flag-day cosmetic refactor.

Also track runtime shader/pipeline creation as a hitch source. Where wgpu/platform
support allows it, use pipeline prewarming/caching or background preparation so entering a
new material/effect combination does not produce an avoidable presentation stall.

### B.1 Forward+ clustered lighting

The detailed design already exists in
[the Forward+ plan](../../engine/WgpuRenderer/docs/forward-plus-plan.md).

Implement it when profiling demonstrates that the current flat light loop is material.
Likely target scenes include night-time towns, streetlights, vehicle lights and many
overlapping explosions. It is not a prerequisite for `RenderSnapshot`, asset streaming
or render/simulation overlap.

**Measured 2026-08-30 (commit `92ebd1d8`): the gate stays closed.** The frame's light
selection now publishes candidates/selected/cap to the Perf tab and the capture
sidecar (`lights{}`), and `WGR_MAX_ACTIVE_LIGHTS` is the cost A/B (0 removes every
local light). A full night flight across stock Everon read a constant
**12 candidates / 12 selected** — the selection walks ALL scene lights, so that is
the island-wide total of active local lights — and a Reforger-Everon night scene read
0 with a GPU frame identical to the lights-off arm (24.49 vs 24.51 ms). Twelve lights
in a flat loop are no workload for Forward+; the gate re-opens if content with
hundreds of active lights appears, and any capture now reports the number that would
show it.

### B.2 Expand indirect drawing

Indirect drawing, GPU culling and GPU LOD selection are already substantially
implemented. The remaining plan is to:

- cover remaining profitable object categories;
- reduce residual CPU-built per-frame draw lists;
- improve retained GPU-scene updates;
- preserve portable adapter fallbacks;
- measure submission, culling and compaction cost.

The detailed implementation history and remaining work live in
[the GPU culling and indirect-draw plan](../../engine/WgpuRenderer/docs/gpu-culling-and-depth-plan.md).

### B.3 Meshlets and mesh shaders

Treat these as two separate milestones:

1. portable compute-driven meshlets using stable WGPU facilities and indirect draws;
2. an optional native mesh-shader path after WGPU support and target-adapter coverage
   are mature enough.

Proceed only if measurements show vertex processing, geometry submission or coarse
culling as a relevant bottleneck. Mesh shaders will not solve vegetation workloads
that are dominated by alpha overdraw, fragment shading or texture bandwidth.

### B.4 Vegetation overdraw and grass cost

> **CLOSED for now, 2026-08-31. Grass went from 4.882 ms to 2.604 ms and the frame from
> 13.34 ms to 10.5 ms.** Every lever this section listed has been resolved one way or the
> other: the vertex count was taken (indexing, −39%), the prepass was eliminated as a wash,
> the cascade-count optimisation turned out to already exist and be on by default, and the
> photo-card path is measured, characterised and parked as a content job. What is left is
> re-authored cards — see `PARKED.md`. Reopen this with a corpus that measures differently,
> not with another idea.

The largest *already measured* GPU line item gets its own entry so effort does not
flow past it to the more fashionable items above. On stock-world captures the GPU
frame is grass-dominated — roughly a third of GPU time — and stock-world scenes are
GPU-bound. (Other scenarios are CPU-bound instead: framerate-governed LOD detail has
produced slow frames where the GPU was under 10% of the cost. Phase 3 captures decide
which regime a given scene is in.)

**Measured 2026-08-31, and one candidate lever is already eliminated.**

The cost, `perf_abel` at 1600×900 with DLSS off, 13.69 ms frame:

| | ms | share |
|---|---:|---:|
| Grass colour | 2.467 | 18.0% |
| Grass prepass | 1.443 | 10.5% |
| Grass shadow | 0.899 | 6.6% |
| **total** | **4.81** | **35%** |

And what it draws: **near 29,136 instances from 270,400 candidates → 10,488,960
vertices; mid 46,743 from 732,736 → 5,048,244; far 0.** That is **15.5 M vertices of
grass per frame, drawn three times** (prepass, colour, shadow) — and the near tier is
360 vertices per instance against the mid tier's 108.

- ~~earlier depth rejection via the prepass~~ — **eliminated, it is a wash.** The grass
  prepass costs 1.449 ms and saves 1.353 ms of grass colour: net zero. Turning the whole
  prepass off drops the frame 1.3 ms, but only because ambient occlusion goes with it,
  which is a feature trade rather than a win. Do not chase this.
- ~~**The vertex count is the open lever**~~ — **taken, 2026-08-31: grass is 39% cheaper.**
  A blade is five quads drawn NON-INDEXED, so it issued 30 vertices where only 12 are
  distinct: the vertex shader derives everything from the row and the side, so the
  duplicated quad corners were computing bit-identical results. Indexed, the
  post-transform cache collapses them.

  | | before | after |
  |---|---:|---:|
  | Grass prepass | 1.462 ms | **0.853 ms** |
  | Grass colour | 2.505 ms | **1.225 ms** |
  | Grass shadow | 0.915 ms | 0.895 ms |
  | grass total | 4.882 ms | **2.973 ms** |
  | GPU frame | 13.34 ms | **12.15 ms** |

  Identical geometry by construction; the index table is tested against the old decode
  kept verbatim as an oracle.

  **Grass shadow does not improve, and it really is not vertex-bound.** This entry briefly
  said otherwise and that was a wrong correction, so both halves are recorded. The code
  observation was true — `draw_shadow` was still calling `draw_indirect`, issuing all 30
  vertices per blade, and reading an *indexed* argument buffer with a non-indexed draw,
  which only works because the trailing fields happen to be zero. But converting it
  measured **nothing**: `Grass shadow` is 0.903 ± 0.007 ms over four runs, against 0.895
  and 0.915 before. The conversion is kept because that argument-buffer mismatch is a real
  latent bug, not because it bought anything.

  And the 0.9 ms is **not the grass shadow total** — that was the third wrong reading of
  this line in one afternoon. `draw_shadow` runs once per cascade, and a GPU timer region
  opened several times in a frame reports the LAST occurrence, not the sum (documented at
  `GpuTimers::begin` now). So the row is one cascade's grass draw. Its exceptional
  stability (±0.007 against ±0.238 for the prepass) is that: one fixed draw, not a fixed
  cost.

  **The cascade-count optimisation this line points at already exists and is on by
  default.** C++ derives a mask from the cascade split distances and the grass reach, and
  `WGR_GRASS_SHADOW_CASCADES` *overrides* it rather than intersecting — so `=4` restores
  the old unconditional behaviour instead of measuring it. That is what the numbers below
  actually compare, and they are worth keeping because they price the existing
  optimisation:

  | grass cascades | frame | cascade sum |
  |---|---:|---:|
  | forced 4 (old behaviour) | 16.47 ms | 4.021 ms |
  | forced 3 | 13.89 ms | 3.235 ms |
  | forced 2 | 11.68 ms | 1.960 ms |
  | **derived (default)** | **12.22 ms** | — |

  The arithmetic behind the mask, for anyone checking it: `CascadeSplits` is a PSSM blend
  at λ = 0.90 over `shadowsZ` = 250 m, giving roughly 0.1 / 6.9 / 17.0 / 50.6 / 250 m for
  four cascades, while the near ring reaches 520 × 0.16 m ÷ 2 = **41.6 m** at 1600×900.
  Cascade 3 therefore starts beyond the grass entirely. The reach is
  resolution-derived, so the mask has to be too — which is why it is computed rather than
  a constant.

  A warning for whoever measures grass next: **this scene is noisy and single runs mislead.**
  Frame total is 12.215 ± 0.703 ms and the prepass 1.076 ± 0.238 across four identical
  runs. One run "showed" the shadow conversion making the prepass and colour worse; four
  showed it was variance. Take at least three. Also note the
  clump renderer was already the cheaper of the two procedural looks (360 vertices but
  centres 3.20x apart against 180 at 1.72x — net 1.7x fewer vertices), so that is not a
  lever either.
  The resolution-derived near-radius clamp was already in (`near_radius.min(resolvable)`
  in `grass/mod.rs`), so the other obvious form of this — near blades drawn beyond the
  distance at which a blade covers a pixel — was already handled too.
- **The photo-card path is 34× cheaper and is NOT a drop-in replacement.** Measured
  2026-08-31 on `perf_abel`, 1600×900, DLSS off. The atlas is resident (9 photographed
  clump families) and the path is simply not opted into on OFP worlds, which build no map
  clutter atlas; `WGR_GRASS_CLUTTER_SOURCE=2` forces it.

  | | frame | grass | vertices |
  |---|---:|---:|---:|
  | procedural blades (today) | 13.34 ms | 4.882 ms | 15,537,456 |
  | cards @ 1.11 m (default) | **7.62 ms** | 0.142 ms | 75,768 |
  | cards @ 0.45 m | 8.63 ms | 0.641 ms | 460,380 |
  | cards @ 0.28 m | 10.28 ms | 1.234 ms | 1,058,352 |

  **The cost is real and the look is not there.** At the default spacing the field is
  isolated tufts on bare ground — a semi-desert, not a meadow. Densifying does not fix it:
  the atlas is authored as large sparse clumps, so at 0.28 m it becomes a wall of
  oversized reeds towering over the camera. Spacing and card size are coupled and only
  the first has a slider. Closing this needs **re-authored smaller cards**, not a setting,
  and that is a content job with a 3-5 ms prize attached. (The owner's earlier verdict on
  cards — "flickering, far too dense", Takistan — predates the own-grid placement and the
  alpha-to-coverage fix, so it is not evidence against the current path.)
- density and draw-distance shaping through the existing grass photo-card gates;
- cheaper shading for the far grass bands.

None of this blocks the architecture tracks. It is listed because on current evidence
it outranks Forward+ (few dynamic lights in typical scenes) and meshlets (explicitly
not an overdraw solution) as a frame-time win.


## Parallel track C — Physics maturity and ownership

Physics continues as its own roadmap, but it must converge with the ownership and task-
graph architecture rather than becoming a second independent simulation scheduler.

Principles:

- keep the engine-owned `PhysicsBackend` boundary;
- keep one explicit fixed-step physics phase/barrier;
- declare each object's authority as gameplay-owned, physics-owned, kinematic or static;
- expose query snapshots/services rather than arbitrary backend access from every worker;
- enable concurrent queries only where the selected backend contract proves them safe;
- preserve multiplayer authority, save/load and late-join behaviour for physics-owned
  objects before enabling them by default;
- do not introduce a second physics-specific worker pool without measuring
  oversubscription against enkiTS and renderer/audio/I/O threads.

Ragdolls, more dynamic props, richer collision events and later vehicle integration may
advance in this track when their own physics roadmap gates are satisfied. They do not
block `RenderSnapshot` or asynchronous asset work.

## Parallel track D — Six-generation compatibility, gated per generation

**Added 2026-09-01 (AUD-001 A3).** The owner's goal names six generations. Until now that goal
lived in one baseline sentence, one continuous-foundation bullet and ~40 decision records, with
no place that says per generation what loads, what is known broken, and what nobody has looked at.
This track is that place. It runs in parallel with A–C and blocks none of them.

### D.0 Where each generation stands (documented, not hoped)

| gen | worlds | models | textures | configs | animations | materials |
|---|---|---|---|---|---|---|
| OFP | loads | loads | loads | loads | loads (739/739 RTM) | n/a |
| Arma 1 | loads (OPRW 18/20) | partial (ODOL 40) | **unmeasured** | **broken** (`debin` fails, AST-016A) | partial | partial (terrain family NOT_PRODUCED) |
| Arma 2/OA | loads (OPRW 24) | partial | **unmeasured** | **unmeasured** | **broken** (8 of 3,493 RTM) | partial; OA foliage shards open |
| Arma 3 | loads (OPRW 25) | loads (ODOL 73) | partial | **broken** (`A3_CONFIG_PARSING` UNSUPPORTED) | **broken** (39 of 6,157 RTM) | partial (8 of 23 shader families) |
| DayZ | loads (OPRW 29) | loads (ODOL 54) | census only | **unmeasured** | **broken by construction** (0 `.rtm`, 8,484 `.anm`) | partial; runtime binding uncensused |
| Reforger | partial (by conversion; native `.ent` parses, unwired) | partial (`.xob` export; `.et` missing) | loads (`.edds`) | n/a | **absent** | **broken** (RFG-003, MAT-054; corpus not re-exported) |

Evidence per cell is in AUD-001 A3 and the records it cites. Two open playtest defects belong here
and in no other track: the bridged A1 T-72 turret vanishing when crewed (AST-016D) and the OFP
cockpit transparency (HANDOVER-20260826).

### D.1 Gates — one per generation, each able to fail

A generation is *gated* when all three hold. None does today.

1. **The cross-generation smoke runs somewhere it can fail.** `test_compat_smoke.cpp` skips on CI
   by its own header (no corpus on hosted runners) and skips in any worktree whose `packages/` lacks
   the corpora. Either a corpus-bearing job (self-hosted, or a synthetic corpus checked in under the
   fixture policy) or the test is a 2026-08-31 measurement, not a gate.
2. **One committed synthetic fixture per container format per generation.** Today every binary
   fixture under `tests/fixtures/` is OFP-era or synthetic-OFP; there is no `.edds`, `.emat`,
   `.xob`, `.pak`, OPRW 18/24/25/29 or ODOL 40/49/54/73 byte fixture, and DayZ has four unit tests.
3. **A capability-matrix row per generation.** The generated matrix carries no DZ-*, RFG-*, ARF-*
   or AST-016* row; DayZ appears only as a passing count and Reforger nowhere, because the
   generator reads the ledger and those findings live only in decision records. Either they enter
   the ledger or the matrix stops claiming to be the compatibility authority.

### D.2 First tasks, cheapest first

- Ledger rows for DZ-001..009, RFG-001..003, ARF-001..002, AST-016A..D, MAT-054/055, WLD-025 (half
  an hour each; PARKED.md already records how `CLD-021` was under-claimed the same way).
- The "unmeasured" cells: Arma 1 and Arma 2/OA textures (every PAA channel claim has an Arma 3
  denominator only, and MAT-034 says so); configs for every generation (no decision record exists
  for any config format; the A1/A3 `config.bin` failures have a symptom and no ticket); DayZ
  material binding at runtime (31,045 RVMATs counted, none checked to bind).
- Animation is the largest documented gap on three generations at once (BMTR v3/v5, `.anm`) and
  is already scoped in AST-016-017; it is a track-D item, not an asset-track item.
- Reforger re-export and native load stay parked by the owner (PARKED.md); they are listed here so
  the park is visible from the track that owns them.

## Long-term continuation to the target engine

The phases above are the executable post-DLSS programme. Completion does not imply
that engine modernisation is finished. Continue into the following outcomes using the
main modernisation roadmap and status ledger as the authority for detailed tickets.

### Phase 9 — Data-oriented hot paths

- Profile before changing layouts.
- Separate hot, frequently iterated simulation data from cold configuration and debug
  state.
- Prefer dense iteration and stable IDs in proven AI, vehicle, animation and
  visibility hotspots.
- Retain compatibility adapters at boundaries rather than forcing all legacy content
  into one new representation at once.
- Measure cache behaviour, allocation count and end-to-end frame time; container
  conversion alone is not success.

### Phase 10 — Memory and spatial-query services

- Add subsystem arenas or pools where lifetime and contention measurements justify
  them.
- Track CPU and GPU memory by owner, asset type and residency state.
- Provide shared, thread-safe read/query services for terrain, objects, rooms,
  visibility, navigation, physics and audio without exposing mutable `GWorld` data.
- Use revision-aware invalidation when terrain, destruction, streaming or editor
  operations change spatial state.
- Keep specialised structures where one universal spatial database would reduce
  correctness or performance.

### Phase 11 — Network I/O and dedicated-server scaling

- Separate socket I/O, packet decode and connection maintenance from simulation.
- Feed the fixed-step server through bounded inbound command queues and immutable
  outbound replication snapshots.
- Use stable network identities rather than container addresses.
- Define authority transfer/handoff explicitly for dynamic and physics-owned objects.
- Add interest management and replication budgets based on measured server load.
- Keep gameplay authority and commit ordering deterministic.
- Ensure new handle/task architectures do not silently change save/load, reconnect or
  late-join identity semantics.
- Prove that headless builds link neither graphics nor audio backends.
- Maintain soak tests for connection churn, packet loss, late join, host/authority
  departure and high entity counts.

### Phase 12 — SQF and scripting services

- Profile real missions before optimising the interpreter.
- Preserve ordered gameplay semantics and explicit script-visible timing.
- Move file, asset, pathfinding and other slow services behind asynchronous requests
  where script compatibility permits.
- Cache parsed/compiled forms and reduce allocation in measured interpreter hotspots.
- Do not execute ordinary mission scripts concurrently merely to claim script
  multithreading.

### Phase 13 — Determinism, record/replay and regression evidence

- Record external input, random seeds/stream identities, network commands and relevant
  content/config versions.
- Record the worker-count/task-graph configuration used for the run.
- Hash authoritative simulation state at defined fixed-step boundaries.
- Maintain a strict hash for same-platform debugging and a semantic/tolerance-aware
  state comparison for cases where bit identity is not a valid cross-platform goal.
- Replay serial and parallel task-graph runs and report the first divergent boundary.
- Preserve diagnostic snapshots around the first divergent tick.
- Add deterministic workload fixtures for AI, vehicles, physics, scripts and
  destruction.
- Treat visual and audio presentation as separately testable unless it feeds gameplay
  authority.

### Phase 14 — Frame pacing, hitch diagnostics and production optimisation

- Track CPU frame time, GPU frame time, present intervals and queue depth separately.
- Track simulation-to-presentation snapshot age and input-latency proxies once render
  overlap/interpolation exists.
- Attribute hitches to simulation, extraction, render preparation, shader/pipeline
  creation, asset decode, upload, residency or operating-system stalls.
- Prewarm/cache or asynchronously prepare shader/pipeline state where measured creation
  hitches justify it.
- Enforce bounded per-frame work for streaming, uploads and destruction cleanup.
- Add long-duration percentile reporting rather than relying on average FPS.
- Evaluate profile-guided optimisation only after representative workloads and build
  reproducibility are established.

### Phase 15 — World decomposition and progressive `GWorld` removal

- Introduce explicit world-owned contexts and services one subsystem at a time.
- Replace arbitrary global access with narrow read views, commands and stable handles.
- Keep server, simulation, renderer, audio and tools dependencies one-directional.
- Add build-time/include/dependency guards where practical so forbidden dependencies
  cannot quietly grow back.
- Make world creation/destruction and test-world isolation explicit.
- Verify save/load and mission-transition lifetime behaviour at every ownership extraction.
- Remove each `GWorld` use only after its replacement has ownership, lifetime and test
  coverage.
- Start from the count, not the name (AUD-001 A1): `GEngine` 1,445 uses / 111 files, `GWorld`
  1,412 / 148, `Glob` 1,212 / 172, `GScene` 478 / 93, `GLandscape` 413 / 99. The ownership table
  and the snapshot design plan for four of the five; `GEngine` — the object a render thread
  actually moves — has no row. Draw-context globals (`ShapeDraw.cpp:224-228`,
  `PresentationSnapshot.cpp:54-55,272`, the `StaticStorage` scratch in `ClipShape.cpp` /
  `TransLight.cpp`) are what a Phase 8 worker touching draw code hits first.
- The end gate is not zero textual references at any cost; it is that mutable global
  world access is no longer an architectural dependency.

## Continuous foundation — repository, CI and documentation

This work runs throughout every phase:

- keep the status ledger authoritative;
- make the actual shipping trunk/default branch and CI triggers unambiguous before the
  large architecture branches multiply;
- remove obsolete build options and dead compatibility scaffolding after their gates;
- keep warnings, format, static analysis and tests enforced in CI;
- keep ASan/UBSan/TSan coverage appropriate to the code being changed;
- maintain installed-game smoke tests for runtime-sensitive changes;
- maintain the six-generation compatibility gates of track D through later architecture
  phases, not only during GL33 retirement — and note that the smoke matrix currently runs on
  no CI job at all;
- preserve benchmark inputs and evidence with enough metadata to reproduce them;
- add ABI/layout tests for versioned C++ ↔ Rust boundary structures;
- add architecture guards preventing server code from linking renderer or audio
  backends and preventing renderer modules from reaching back into mutable gameplay
  ownership;
- maintain save/load and mission-transition regression cases for ownership/lifetime
  changes;
- define configurable performance-regression gates for representative scenarios using
  percentile/frame-time evidence rather than only average FPS;
- update user-facing configuration and migration documentation with every retired or
  replaced subsystem.

## Recommended critical-path execution order

The common critical path is:

1. Complete and stabilise DLSS.
2. Introduce and harden the generic upscaler boundary.
3. Freeze GL33 and archive its final reference evidence.
4. Pass the WGPU sole-renderer gate. Actual deletion (2.3) is evidence-gated and may
   trail the critical path: keeping the frozen runtime as a cheap triage oracle through
   `RenderSnapshot` bring-up is acceptable — what must not resume is feature or parity
   work on it. Only the freeze blocks later steps, not the removal.
5. Fix trunk/CI ambiguity and extend engine/streaming/performance observability.
6. Implement the cheapest request-deduplication and bounded-upload wins.
7. Introduce ownership/authority rules, stable handles, deferred reclamation and a
   versioned C++ ↔ Rust boundary.
8. Build the immutable `RenderSnapshot` boundary.
9. Enable bounded, measurable render/simulation overlap.

After those shared foundations, **split the architecture work rather than serialising it**:

### Simulation track

**Status 2026-09-01 (AUD-001):** every numbered item below is either done or answered. 10S exists
(`RunSimStagesSerial`, SIM-811, with declared stage resources — SIM-814). 11S is done (SIM-801
worker FP environment, SIM-803 stable iteration). 12S was measured and answered "no" (below).
13S *is the ration*: PERF-017's `POSEIDON_AI_TRACK_PERIOD` is a reduced-rate perception update,
independently switchable, gated on the behaviour oracle — which is what 8.5 asks for. What the
track still owes is Phase 8's command/commit stage and Phase 13's replay, and the first blocker
of both is that the resident object set is a function of wall time (`LandSave.cpp:3510,3581,3219`),
so two runs cannot agree on what the world contains before the AI ever runs.

10S. Introduce the serial deterministic-effects simulation task graph. **Done (SIM-811).**  
11S. Audit worker FPU/RNG/iteration rules and validate across worker counts. **Done (SIM-801/803).**  
12S. Parallelise measured query, AI, navigation, animation and vehicle workloads. **Answered no
     (2026-09-01): after the rations the AI stage is 8-14% of the frame.**
     - **Ration before you parallelise.** PERF-016 reviews Arma 3 2.20's OPREP against what is
       measured here. 68% of the AI stage is one loop whose throttle exists, is written, and is
       never read; enabling it deletes the work instead of distributing it, and what remains
       afterwards should decide whether a dependency job system is worth building at all.
       Enabling it changes AI behaviour, so it is gated on the `StateTimeline` oracle and
       behavioural tests — that gate, not the job system, is the next task on this line.
     - Scope any such system against `perf_combat`. Three of four bench scenes are GPU-bound
       (PERF-012), and `perf_abel` — the scene most historical figures were taken on — would
       show none of it.  
13S. Evaluate subsystem-specific AI simulation LOD where large-battle measurements justify it.
     **The first one shipped as the tracking ration (PERF-017); the next candidate is `unitThink`
     (PERF-019), on the same oracle.**
14S. *(new)* Make the object-stream admission budget counted rather than timed, so replay and the
     frame equivalence oracle see the same resident set on two machines. Proposed, not decided —
     see AUD-001 §B; it changes what a slow machine shows.

### Asset/streaming track

10A. Build the staged asynchronous asset pipeline and hardened Derived Data Cache.  
11A. Add task QoS so streaming/decode cannot starve simulation. *(Partly in place, and
one of its helpers was broken but latent: `poSetMyPriority` cast a thread ID to a HANDLE
and was a silent no-op on Windows. No thread was actually mis-prioritised — the preparer
carried a direct-API workaround from the commit that introduced its workers — so this was
a trap for the next caller rather than a live defect. Fixed 2026-08-31, workaround removed,
with a test that reads the priority back from the OS and that the old code fails. What is genuinely in place: 2 workers, a bounded
distance-ordered queue with back-pressure rather than dropping, and the main-thread FP
environment applied on each worker.)*  
12A. Add residency budgets, hysteresis and predictive streaming.

These two tracks may run concurrently after shared critical-path step 9. Neither a finished DDC nor a
finished residency manager is a prerequisite for beginning the simulation task graph.

Then converge on:

14. Optimise measured data, memory and spatial-query hotspots.
15. Separate network I/O and scale the dedicated server.
16. Optimise SQF and asynchronous script services without breaking ordering.
17. Establish deterministic record/replay verification.
18. Harden frame pacing, latency visibility and production hitch diagnostics.
19. Decompose world ownership and progressively remove `GWorld` dependencies.

## Optional and parallel work

These items may proceed when an owner is available and their prerequisites are met,
but they do not block the critical path:

- prototype FSR 1 after the generic upscaler boundary; accept or shelve it using the
  defined quality/performance gate;
- add HRTF capability handling and smoothly world-driven EFX environments;
- move toward audio commands, stable voice handles and single-thread OpenAL ownership;
- add bounded geometry/material obstruction and room/portal propagation;
- extend outdoor, ballistic, vehicle and underwater audio based on evidence;
- add Forward+ clustered lighting when lighting cost justifies it;
- expand indirect drawing wherever CPU submission remains material;
- evaluate compute-driven meshlets;
- consider experimental native mesh shaders only as an optional fast path;
- continue the physics roadmap in parallel, converging on the shared fixed-step,
  ownership, query-service, save/load and multiplayer-authority boundaries.

## Decision discipline

Advanced features are not goals by themselves. Every candidate follows:

```text
measured problem
      ↓
small experiment
      ↓
repeatable before/after evidence
      ↓
adopt, revise or reject
```

The roadmap should be updated when implementation evidence changes dependencies or
shows that another bottleneck has become dominant.
