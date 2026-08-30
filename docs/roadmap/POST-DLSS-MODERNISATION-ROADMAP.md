# OpenPoseidon post-DLSS modernisation roadmap

**Status:** Living plan  
**Created:** 2026-08-29  
**Scope:** Work that begins after the current DLSS integration reaches its ship gate  
**Revision:** v2 — ownership/ABI hardening, renderer-history correction, task QoS,
determinism rules, AI simulation LOD, physics convergence and parallel critical paths

This document turns the longer engine-modernisation roadmap into an executable
post-DLSS sequence. It accounts for work that OpenPoseidon has already completed,
keeps short, low-risk improvements separate from major architectural changes, and
uses measurements as the gate for optional GPU features.

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
- compatibility layers for Arma 1, Arma 2, Arma 3, DayZ and Reforger content.

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
| Generic upscaler boundary | Low–medium | High | Implement with the DLSS cleanup |
| FSR 1 fallback experiment | Low | Medium if accepted | Optional bounded experiment; must not block architecture work |
| Asset-request deduplication | Low | Medium | Implement early |
| GPU-upload frame budget | Low–medium | High | Implement early |
| Streaming trace counters | Low | High | Implement immediately |
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
frame). Whether it *sounds* right on headphones is the owner's A/B; nothing gates
on it.

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

### 3.1 Measurement hygiene

Each of these has already produced wrong numbers at least once in this repository:

- every capture records the exe/DLL commit (`DEPLOYED-FROM.txt`) and the render
  resolution next to the numbers; a capture without both is not evidence;
- the farfield bench defaults to 800×600 and overstates play-resolution framerate by
  roughly 25% — its numbers must never be quoted as play performance;
- one-shot GPU passes leave their timer regions registered, so `--capture-metrics`
  reports them as if they ran every frame; only the GPU frame total is re-measured and
  trustworthy for per-frame attribution;
- `--dev-map` has an empty retained set, so world-space renderer features measure as
  inert there — measure world-scale features on a real world.

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

### 4.3 Deferred reclamation

Resource destruction must be deferred until no published snapshot, worker job or
in-flight GPU submission can still reference the resource.

Use explicit generations/epochs or equivalent completed-work tracking rather than
arbitrary frame-count delays.

Mutation requested by parallel jobs should be represented as commands or result
buffers and applied at an explicit ownership boundary.

## Phase 5 — Immutable `RenderSnapshot`

This is the first major architectural project after the low-hanging improvements.

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

The largest *already measured* GPU line item gets its own entry so effort does not
flow past it to the more fashionable items above. On stock-world captures the GPU
frame is grass-dominated — roughly a third of GPU time — and stock-world scenes are
GPU-bound. (Other scenarios are CPU-bound instead: framerate-governed LOD detail has
produced slow frames where the GPU was under 10% of the cost. Phase 3 captures decide
which regime a given scene is in.)

Candidate levers, each requiring a before/after capture:

- alpha/alpha-to-coverage overdraw reduction: tighter card geometry, depth ordering,
  earlier depth rejection;
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
- maintain the Arma 1 / Arma 2 / Arma 3 / DayZ / Reforger compatibility smoke matrix
  through later architecture phases, not only during GL33 retirement;
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

10S. Introduce the serial deterministic-effects simulation task graph.  
11S. Audit worker FPU/RNG/iteration rules and validate across worker counts.  
12S. Parallelise measured query, AI, navigation, animation and vehicle workloads.  
13S. Evaluate subsystem-specific AI simulation LOD where large-battle measurements justify it.

### Asset/streaming track

10A. Build the staged asynchronous asset pipeline and hardened Derived Data Cache.  
11A. Add task QoS so streaming/decode cannot starve simulation.  
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
