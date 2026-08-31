# REN-THR-003 — Every input to the published frame that is a function of how fast the machine ran

**Date:** 2026-08-31
**Branch:** `agent-frame-divergence` (from `asset-obs-001`)
**Follows:** [REN-THR-002](REN-THR-002-the-equivalence-oracle-and-what-it-found-20260831.md), including its addendum
**Status:** static audit only. Nothing here was run. The experiments are written to be executed by
the session that owns the game folder.

## What this had to explain

REN-THR-002's addendum measured, over a window placed past the mission load, that three runs of
the same build and scene publish identical frames for a while and then diverge. Two further
measurements were supplied while this audit was in progress:

| window (`WGR_FRAME_HASH_FIRST`) | agreeing prefix | equal-at-index | distinct hashes per run |
|---|---:|---:|---:|
| 400 | 24 | 24 of 96 | 27–48 of 96 |
| 400, `POSEIDON_LOCKSTEP_HZ=60` | 19 | — | — |
| 800 | **0** | **0 of 96** | 87–95 of 96 |

Three constraints follow, and every candidate below is judged against all three:

1. **The divergence is absorbing.** By frame 800 not one hash matches at any index. A cause that
   makes two runs differ and then re-converge is ruled out.
2. **Lockstep does not fix it.** `POSEIDON_LOCKSTEP_HZ` pins the simulation's `deltaT`. So the
   cause must read a clock that is **not** the simulation clock — a real clock, or a measured
   frame duration. Anything downstream of `Glob.time` or `Glob.uiTime` alone is eliminated by
   this arm, because both advance by `deltaT` (`World.cpp:399`).
3. **The window at 400 is quiet and the window at 800 is busy.** 27–48 distinct hashes versus
   87–95. The brief's "one event at a fixed offset" reading and the coordinator's "differs from
   the first frame it actually moves" reading are *both* consistent with the top candidate, and
   the reason is given under it: the mechanism is a **clamped** feedback loop, and a clamp
   produces exactly a stretch of agreement followed by one clean, permanent parting.

## What is actually hashed, which is narrower than it sounds

`EngineWgpu::HashPublishedFrame` (`engine/WgpuRenderer/EngineWgpu.cpp:7210-7241`) covers
`frame.clear`, `frame.fog_color` and fifteen slices. It does **not** cover:

- `frame.shadow` — `light_vp`, `cam_pos`, `resolution`, `grass_cascade_mask`. Set at
  `EngineWgpu.cpp:2078-2148` and never folded in.
- `backing.retainedInstances`, the producer's view of the retained set size.
- Anything pushed through the twelve `wgr_set_*` calls (REN-THR-001 already says this).

That last exclusion **eliminates several otherwise excellent suspects outright**, and they are
recorded here so nobody re-proposes them:

| suspect | file:line | why it cannot be this |
|---|---|---|
| Auto-exposure time constant | `EngineWgpu.cpp:7529-7553` | `_autoExposureRate` reaches the GPU only via `wgr_set_render_params`. Not hashed. |
| Sun-glare accommodation | `EngineWgpu.cpp:7583` | Same route. Not hashed. |
| Sky easing (`kSkySmoothTau`, real clock) | `EngineWgpu.cpp:8653-8676` | Goes to `wgr_set_sky_runtime`. The frame's own `fog_color` comes from the **unsmoothed** `FogColor()` at `EngineWgpu.cpp:1663`, not from `_skyFog`. Not hashed. |
| Grass wind / track ageing (`GetLastFrameDuration`) | `EngineWgpu.cpp:7806` | `wgr_grass_set_params`. `frame.grass_batches` carries only the batch descriptor. Not hashed. |
| TAA / DLSS jitter | `EngineWgpu.cpp:6446-6474` | Applied inside the Rust renderer after publication; `cameras[i].proj` is unjittered. Not hashed. |

## Search method, and its denominator

Four sweeps, each with the count it returned, so the coverage claim is checkable rather than
asserted.

**Sweep 1 — every real-clock read on the producer path.** `GlobalTickCount()`,
`steady_clock::now()`, `GetLastFrameDuration()`, `GetAvgFrameDuration(` across
`engine/Poseidon/World/Scene/`, `engine/Poseidon/World/Terrain/`, `engine/Poseidon/Graphics/`
and `engine/WgpuRenderer/`: **50 hits.** Classified by hand into instrumentation (a clock read
whose only consumer is a `LOG_*` or a profiling bucket) and decision (a clock read that changes
a value the frame is built from). **Six** are decisions; the rest measure and print. The six are
`SceneDraw.cpp:1095`, `LandSave.cpp:3182/3193`, `LandSave.cpp:3452/3515`, `EngineWgpu.cpp:7538`,
`EngineWgpu.cpp:7583`, `EngineWgpu.cpp:8653`, and the last three are eliminated by the "not
hashed" table above.

**Sweep 2 — every async GPU readback and its consumers.** Files mentioning
`readback`/`map_async` under `engine/WgpuRenderer/`: **16** (14 Rust, 2 C++). The only way a
readback can reach the published frame is if C++ queries it before publishing, so the sweep that
matters is the consumer side: `GetObjectStats`, `GetGrassStats`, `GetGpuMemoryStats`,
`GetWaterGpuTimings`, `GetCpuTimings` — **57 call sites** outside the Rust crate. Result below;
the short version is that **the cull statistics feed nothing but log lines and the dev panel**,
and the leading suspect named in the brief is therefore *not* the answer.

**Sweep 3 — frame counters, "frames since", LRU ages, last-used frames.** `_lastUsedFrame`,
`_residencyFrame`, `framesSince`, `_frameCounter`, `(_frame %`, `frameParity` across the same
directories: **17 hits**, in three mechanisms (texture LRU, retained-liveness sweep, water debug
stats). None is keyed on a counter that differs per run at the same hash index — the counters
increment once per published frame, so frame *N* of two runs sees the same counter. They can
only diverge as a *consequence* of something upstream, and are ranked accordingly.

**Sweep 4 — randomness.** `GRandGen` / `rand()` / `mt19937` across `engine/`: **276 hits**, of
which **32** are in `engine/Poseidon/Graphics/` and `engine/WgpuRenderer/` and **0** are in
`SceneDraw.cpp` / `Scene.cpp`. The graphics ones are all particle spawning in `Smokes.cpp`, paced
by simulation `deltaT` — but see candidate 1's amplifier, which is where this becomes relevant.

## The ranked candidates

### 1. `Scene::_lodInvWidth` — the LOD governor's framerate feedback loop — **LIKELY**

`engine/Poseidon/World/Scene/SceneDraw.cpp:1057-1262`, entered from `SceneDraw.cpp:1834`, once
per drawn frame.

```
SceneDraw.cpp:1095   float frameDuration = GEngine->GetAvgFrameDuration(4);
```

`GetAvgFrameDuration` (`engine/Poseidon/Graphics/Core/Engine.cpp:77-85`) averages the last four
entries of a 16-deep ring filled in `Engine::FinishDraw` from
`Poseidon::Foundation::GlobalTickCount()` (`Engine.cpp:327-338`) — **a real millisecond wall
clock, measured per rendered frame, with nothing to do with the simulation step**.

That number drives a persistent state variable, `_lodInvWidth` (`Scene.hpp:203`), toward a target
line, in up to five damped iterations per frame, with an anti-oscillation gate on `Glob.uiTime`
(`SceneDraw.cpp:1217-1240`). `_lodInvWidth` is then the multiplier in the LOD selector:

```
SceneDraw.cpp:639    float detail2 = distance2 * Square(_lodInvWidth);      // LevelFromDistance2
SceneDraw.cpp:667    float detail2 = distance2 * Square(_lodInvWidth);      // LevelShadowFromDistance2
SceneDraw.cpp:806    int drawLevel = LevelFromDistance2(shape, oi->distance2, ...);
SceneDraw.cpp:836    oi->drawLOD = drawLevel;
```

`oi->drawLOD` decides which LOD body is drawn and whether the object is `LOD_INVISIBLE` at all,
and `oi->sortComplexity` / `oi->sortPassOrder` are derived from it (`SceneDraw.cpp:843-846`). All
three reach `draws3d`, `shadow_casters` and the ordering of `cmds` — **hashed slices**. This is a
direct, first-order path from a wall clock to the published bytes.

Why it fits all three constraints, and fits them better than anything else found:

- **Not the simulation clock.** `GlobalTickCount()` is real time. `POSEIDON_LOCKSTEP_HZ` pins
  `deltaT` and does not touch it. This is the only candidate that explains why the lockstep arm
  was *no better* rather than merely *not perfect*.
- **Absorbing.** `_lodInvWidth` is carried frame to frame and is only ever multiplied/divided by
  a factor derived from its own current value and the measured frame time. There is no
  re-synchronising event. Two runs whose values part stay parted.
- **A clamp explains the quiet prefix.** `SetQualitySettings` (`Scene.cpp:640-653`) sets
  `_minLodInvWidth = _qualitySettings` and `_maxLodInvWidth = _qualitySettings * 16`, and the
  loop has explicit fast-path breaks at both rails (`SceneDraw.cpp:1168`, `:1189`,
  `:1177-1186`, `:1198-1207`). While the measured frame time sits well outside the target band,
  **both runs are pinned to the identical rail and publish identical frames**. The prefix ends on
  the frame the loop first leaves the rail. That is a single clean divergence at a
  quasi-fixed offset *and* "it differs from the first frame it actually moves" — the two readings
  in the brief are the same event seen from two sides, and this candidate produces both.
- **It explains the two arms parting at different offsets.** Lockstep changes the per-frame work
  (a fixed number of sim steps per frame instead of a variable one), so it changes the measured
  frame durations, so the loop leaves the rail at a different frame: 19 versus 24. A cause with
  no dependence on the frame-cost profile would have given the same offset in both arms.
- **It explains 400-quiet versus 800-busy.** At the 400 window the scene is barely animating and
  the governor is on a rail. By 800 the scene is moving, frame cost is varying, the loop is
  live every frame, and every frame differs.

**The amplifier, which is why "absorbing" is total rather than partial.**
`Scene::GetSmokeGeneralization()` returns `_lodInvWidth * 0.1f` (`SceneDraw.cpp:1264`). That
value sets `_generalize` in the particle sources (`Smokes.cpp:100-103`, `:1073-1076`,
`:1139-1142`), and `_nextTime += _interval * _generalize * _generalize` is the **cloudlet spawn
rate**. Each spawn draws roughly seven values from the shared global `GRandGen`
(`Smokes.cpp:106-112`, `:1080-1086`). So once `_lodInvWidth` differs by any amount, the number of
draws taken from the *global* random stream differs, and from that moment every RNG consumer in
the process — gameplay included — is on a different sequence, permanently. If the scene has any
smoke or dust in it at all, this converts a small LOD difference into total divergence of
everything downstream. **This coupling is worth recording independently of whether it is the
cause here**; a global RNG whose draw count depends on a wall-clock feedback loop is a
determinism defect on its own terms.

**Experiment 1 (cheapest decisive test, run this first).**

```
WGR_FRAME_HASH=1 WGR_FRAME_HASH_FIRST=800 WGR_LOD_GOVERNOR_RANGE=1
```

Three runs, everything else default, same scene, same build. `LodGovernorRange()`
(`Scene.cpp:629-641`) accepts any value `>= 1.0`; at exactly 1 it sets
`_maxLodInvWidth == _minLodInvWidth == _qualitySettings`, the constructor's
`_lodInvWidth = (_min + _max) * 0.5` (`Scene.cpp:115`) and `SetQualitySettings`'s
`saturate` (`Scene.cpp:652`) both land on that same single point, the loop's `dist` term is
identically zero and both rail fast-paths fire. **`_lodInvWidth` is then frozen at a value
identical in every run, with no code change and no rebuild.**

- **Confirms** if the agreeing prefix at window 800 goes from 0 to a large number (ideally 96).
- **Eliminates** if the prefix stays at 0. It does not merely weaken the hypothesis; the governor
  is provably inert under this setting, so a still-diverging run means the cause is elsewhere.
- Note the hashes will not match the default-arm baseline — the LOD ladder is different. The
  question is only whether the three runs agree **with each other**.
- Run it at window 400 too, as a second reading: the prefix should go to 96 there.

**Experiment 1b (only if 1 confirms, to separate the two paths).** Same three runs with
`WGR_LOD_GOVERNOR_RANGE=1` plus `WGR_LOD_GOVERNOR_TARGET=1`. The target-drive arm
(`SceneDraw.cpp:1042-1051`, `:1122-1147`) changes the error metric but not the clamp; if 1
confirmed and 1b also agrees, the clamp is what mattered, which is the interpretation above.

### 2. The object stream's wall-clock admission budget — **POSSIBLE, and NOT yet eliminated**

`engine/Poseidon/World/Terrain/LandSave.cpp:3452` (`admitStart`), `:3515-3517` (`elapsed`),
consumed by the `admitBudgetSpent` lambda at `:3496-3554`. Runs once per rendered frame from
`TerrainWgpu::DrawTerrain`.

REN-THR-002's addendum records this as eliminated by `WGR_OBJECT_STREAM_MAX_BATCH_SCALE=1`.
**That experiment did not remove the clock.** Setting the scale to 1 makes
`admitCap == _modernObjectCreatesPerUpdate == admitFloor` (`:3454`, `:3477`), so the cap and the
floor coincide — but *below the floor* the hard ceiling is still consulted:

```
LandSave.cpp:3535   const bool ceiling = admitCeilingMs > 0.0f && attempted > 0 && elapsedMs >= admitCeilingMs;
```

`admitCeilingMs` defaults to **16 ms** (`ObjectStreamCeilingMs`, `LandSave.cpp:2651-2660`), and
during a fill the batch reaches 16 ms routinely. So a slow frame still admits *fewer* objects
than a fast one, on a real clock, in exactly the arm that was reported as ruling this out.

Admitted objects reach `draws3d` and `shadow_casters` via the world's draw list, so this does
change hashed bytes. Against it: admission is monotone toward a desired set, so a stationary
camera converges and the runs would re-agree — which contradicts constraint 1. It survives as a
candidate only if the camera keeps moving at window 800, which the 87–95 distinct-hash count
suggests it does.

**Experiment 2 (run second; it is the correction to a previous experiment, not a new idea).**

```
WGR_FRAME_HASH=1 WGR_FRAME_HASH_FIRST=800 \
WGR_OBJECT_STREAM_MAX_BATCH_SCALE=1 WGR_OBJECT_STREAM_MAX_MS=0 WGR_OBJECT_STREAM_MS_PER_UPDATE=0
```

`WGR_OBJECT_STREAM_MAX_MS=0` disables the ceiling outright (the `admitCeilingMs > 0.0f` guards at
`:3535` and `:3546` both fail), and with the cap equal to the floor the ordinary budget is never
reached either. The batch is then a **fixed count per update with no clock read on the decision
path**. Also confirm `WGR_OBJECT_STREAM_GPU_BUDGET` is **unset** — `GpuClampedObjectBudget`
(`LandSave.cpp:3032-3057`) is opt-in, and its own comment records that it *oscillates* when on,
which would be a divergence source of its own.

- **Confirms** if the prefix at window 800 rises substantially.
- **Eliminates** if it does not. Unlike the previous attempt, this arm genuinely removes the
  clock, so a null result here closes the hypothesis for good.

### 3. Prefetch lead: a velocity estimator smoothed on a real clock — **POSSIBLE**

`engine/Poseidon/World/Terrain/LandSave.cpp:3182` (`residencyNow = steady_clock::now()`),
`:3193` (`dt`), `:3211` (`alpha = 1 - exp(-dt / PrefetchSmoothingTau)`), lead applied at
`:3226-3252`, window centre quantised at `:3255-3259`.

The camera-velocity estimate is exponentially smoothed with a **real-time** constant
(`PrefetchSmoothingTau = 0.5 s`, `:2810`), and the resulting lead shifts the residency window
centre before it is quantised to 4 cells. Two runs with different frame pacing carry different
smoothed velocities, and the difference reaches the published frame **only when the biased centre
falls on opposite sides of a quantisation boundary** — a discrete event, which is the "single
clean divergence at a fixed offset" shape in its purest form. Lockstep does not touch it: this is
`steady_clock`, not `deltaT`.

Two things hold it below candidates 1 and 2. It is gated on speed: `ObjectStreamPrefetchSeconds`
defaults to 2.0 s but `ObjectStreamPrefetchMinSpeed` defaults to **8 m/s** (`:2760-2769`), with a
ramp to full lead at 16 m/s, so a walking or stationary camera never engages it at all. And once
engaged, a recentre is followed by convergence, so it is not obviously absorbing.

**Experiment 3.** `WGR_FRAME_HASH=1 WGR_FRAME_HASH_FIRST=800 WGR_OBJECT_STREAM_PREFETCH_SECONDS=0`
— 0 disables the lead entirely (`:3226`) and restores the reactive window. Confirms if the prefix
rises; eliminates if it does not. **Cheap enough to run alongside experiment 2 as a third arm**,
and worth running even if 2 confirms, because both live in the same function.

Before running it, have the harness state the camera speed in the window. **If the camera never
exceeds 8 m/s at window 800, this candidate is eliminated without an experiment** and experiment 3
should be skipped.

### 4. Texture LRU eviction driven by measured resident bytes — **POSSIBLE, second-order**

`engine/WgpuRenderer/TextureBankWgpu.cpp:80-199`, called at `EngineWgpu.cpp:2173`, immediately
after publication.

The gate is `stats.object_texture_bytes > textureBudget` (`:101-103`), read from
`wgr_get_memory_stats`. That byte count is a function of how many textures have been uploaded so
far, which is a function of how many objects the streamer admitted, which is candidate 2. The
frame counter itself (`_residencyFrame`, `:81`) increments once per published frame and is
therefore *identical* at the same hash index in two runs, so the LRU cannot originate a
divergence — but it can **amplify** one, and the amplification is destructive: an eviction leaves
models on the white fallback until `InvalidateStaleGpuModels` (`EngineWgpu.cpp:2176`) re-registers
them, and a re-registration changes the retained instance table.

Ranked here because it is a consequence, not a cause. **No separate experiment**: if experiment 2
confirms, this is downstream of it; if experiment 2 eliminates, this is eliminated with it. If
both 1 and 2 come back null, then run
`WGR_TEXTURE_EVICT_GRACE_FRAMES=36000` (the maximum, `TextureBankWgpu.cpp:153`) to make the LRU structurally unable
to evict anything within a 96-frame window, and re-measure.

### 5. Retained-liveness sweep pace — **UNLIKELY**

`engine/WgpuRenderer/EngineWgpu.cpp:6042-6129`. Sweeps `1/kRetainedLivenessSweepFrames` of the
`_gpuInstances` bucket array per frame and restarts on a rehash (`:6074-6080`). The slice size is
`bucket_count()`-dependent, so the sweep's phase depends on how many objects have been admitted —
again downstream of candidate 2. Its only output is `NoteTextureUse`, which feeds candidate 4.
Nothing it writes is hashed. **Experiment, only if 1–4 all come back null:**
`WGR_RETAINED_LIVENESS=0`.

### 6. The async GPU readbacks — **ELIMINATED for this symptom**

This was the brief's leading suspect and the audit does not support it. Every consumer of a
readback outside the Rust crate was enumerated (sweep 2, 57 call sites) and classified:

| consumer | file:line | what it feeds |
|---|---|---|
| `GetObjectStats` | `apps/cwr/Game/GameApplication.cpp:200` | a JSON capture record |
| `GetObjectStats` | `EngineWgpu.cpp:7362` | `LOG_INFO` only |
| `GetObjectStats` | `World.cpp:1507-1515` | a capability *probe* — the return value is used as "is this the WGPU engine", the stats are discarded |
| `GetGrassStats` | `EngineWgpu.cpp:8159` | `LOG_INFO` only, inside a `WGR_*`-independent placement witness |
| `GetGrassStats` | `DebugOverlay.cpp:3833`, `GameApplication.cpp:239` | dev panel, JSON |
| `GetWaterGpuTimings` / `GetCpuTimings` | `EngineWgpu.cpp:7267/7271`, `WaterWgpu.cpp:1159/1202/1236`, `DebugOverlay.cpp` ×5, `GameApplication.cpp:173/178` | logging, dev panel, JSON |
| `GetGpuMemoryStats` | `TextureBankWgpu.cpp:85/190/194` | **candidate 4** |
| `GetGpuMemoryStats` | `LandSave.cpp:3062` | `GpuClampedObjectBudget`, **off by default** |
| `GetGpuMemoryStats` | `TerrainWgpu.cpp:2653`, `DebugOverlay.cpp:2669`, `StreamingDiag.cpp:65` | reporting |

The **cull statistics specifically — `WgrObjectStats`, the two-to-three-frame-late block that the
brief names — feed nothing but log lines, the dev panel and the capture JSON.** No LOD selection,
no culling decision, no streaming admission, no residency choice reads them. The GPU cull's own
`lod_inv_width` is *pushed* to the GPU each frame (`EngineWgpu.cpp:2586`) and explicitly not fed
back — `cull.wgsl:30` says so in the field comment, and `gpu-culling-and-depth-plan.md` §3.5
records the decision. The only readback that reaches a decision is the **memory** one, and that
is candidate 4.

This is worth stating plainly because it inverts the brief's ranking, and because a null result
from an experiment aimed at the cull readbacks would have been read as evidence about the wrong
thing.

## Confounder to rule out before any of the above

`Engine::DrawFinishTexts` (`engine/Poseidon/Graphics/Core/EngineDrawing.cpp:33-74`) draws
`iFPS`, `aFPS` and `aLOD` as 2D text when `ShowFps() >= 1`. Those glyph quads land in `_verts` and
`_batches`, both hashed, and their values are `_lastFrameDuration`, `GetAvgFrameDuration()` and
`-10*log10(GetLodInvWidth())` — three wall-clock readouts. **If the capture harness passes
`--fps`, no two runs can ever agree and every result above is meaningless.** The 24-frame prefix
at window 400 suggests it is off, but confirm it from the harness's argument list before spending
a run. (Note the third line is a direct readout of candidate 1; if `--fps` were on, the `aLOD`
digits changing would be the tell.)

## The order to run them

1. **`WGR_LOD_GOVERNOR_RANGE=1`**, three runs at `WGR_FRAME_HASH_FIRST=800`, and three at 400.
   Zero code, one variable, decisive either way.
2. **`WGR_OBJECT_STREAM_MAX_BATCH_SCALE=1 WGR_OBJECT_STREAM_MAX_MS=0 WGR_OBJECT_STREAM_MS_PER_UPDATE=0`**,
   three runs at 800. This is the corrected form of an experiment already reported as done.
3. **`WGR_OBJECT_STREAM_PREFETCH_SECONDS=0`**, three runs at 800 — skip if the camera is under
   8 m/s in the window.
4. Only if all three are null: `WGR_TEXTURE_EVICT_GRACE_FRAMES=36000`, then
   `WGR_RETAINED_LIVENESS=0`.

If 1 confirms, the natural next arm is **1 combined with 2 and 3**, to find out whether the
governor is the whole of it or merely the first thing to part.

## What could not be determined without running the game

- **Whether the governor is actually on a rail during the 400 window.** The clamp argument is the
  strongest part of candidate 1 and it is unverified. The engine already logs it: `--fps` prints
  `aLOD = -10*log10(_lodInvWidth)`, and `Scene::GetQualityText()` (`Scene.cpp:670-680`) prints the
  rails. Because `--fps` is itself a confounder (above), the honest way to get this is a temporary
  `LOG_INFO` of `_lodInvWidth`, `_minLodInvWidth`, `_maxLodInvWidth` once per frame — a one-line
  change in `SceneDraw.cpp` after line 1262, in a build used only for this measurement.
- **The camera's speed in the windows at 400 and 800.** Decides whether candidate 3 is live at all.
- **Whether the object stream is still admitting at frame 800.** If it has converged, candidate 2
  is inert and its experiment is a formality.
- **Whether the scene contains any particle source.** Decides whether candidate 1's global-RNG
  amplifier is engaged, and therefore whether "absorbing" is explained by the amplifier or has to
  be explained by the LOD path alone.
- **Which of the fifteen hashed slices first differs.** This is the single most valuable thing
  nobody has measured, and it is cheap: hashing the slices *separately* and logging fifteen
  numbers instead of one would name the subsystem directly and would have made most of this audit
  unnecessary. `HashPublishedFrame` already computes them one at a time
  (`EngineWgpu.cpp:7217-7233`); the change is to log them rather than fold them. That file belongs
  to another session at the time of writing, which is why it is a recommendation and not a patch.

## What this does not say

- **It does not claim candidate 1 is proven.** It is the only candidate found that satisfies all
  three constraints simultaneously, which is a strong argument and not a measurement. The
  experiment is one variable and one run set; it should be run before the claim is repeated.
- **It does not enumerate the Rust side.** Everything after `wgr_render_frame` is out of scope
  because the hash is taken before it. A divergence originating in the renderer would be invisible
  to this instrument entirely — which is REN-THR-002's own closing caveat and still stands.
- **`frame.shadow` is unhashed and the audit did not chase it.** The cascade matrices are built
  from the camera and the sun, both snapshot-derived, but the grass-cascade mask
  (`EngineWgpu.cpp:2099-2133`) depends on `_grassPhotoTuftActive` and the cascade split distances.
  If the hash is ever extended, that block is the first place to look for a surprise.

---

## The experiments, run

Run by the session holding the game folder, on `perf_abel`, three runs per arm, hashing a
96-frame window at frame 400. The instrument was extended first: `HashPublishedFrame` now
reports **each of the seventeen slices separately** as well as the fold, which was this
document's own cheapest-high-value suggestion and which turned out to carry the most
information of anything measured here.

**Experiment 0 — the `--fps` confounder: clear.** `farfield-bench.ps1` never passes it, so
the FPS/LOD text is not in `_verts` and no measurement in this line of work is void.

**Experiment 1 — freeze the LOD governor (`WGR_LOD_GOVERNOR_RANGE=1`): a real contributor,
and not the whole cause.**

| arm | 2D slices part at | world slices part at |
|---|---:|---:|
| everything free | **51** | **52** |
| governor frozen | **59** | **60** |
| governor frozen **+** object stream clocks pinned | **59** | **60** |

Freezing the governor buys eight frames, and buys them *exactly*: two arms that differ in
four other environment variables both part at 59/60, which is why this is read as signal
rather than as run-to-run noise. But divergence still happens, so `_lodInvWidth` is **a**
clock in the published frame and not the only one.

**Experiments 2 and 3 — the object stream, eliminated a third time.** With the hard ceiling
and the per-update budget pinned as this document correctly insisted (`MAX_MS=0`,
`MS_PER_UPDATE=0`, not just `MAX_BATCH_SCALE=1`) and prefetch disabled, the divergence
point does not move by a single frame. The earlier eliminations were right for the wrong
reason; this one is right for the right one.

## The finding the per-slice hashes produced

In **every** arm, the same shape:

- `verts`, `batches` and `cmds` — the 2D path — part **exactly one frame before**
  everything else.
- `overlay_verts`, `overlay_indices`, `overlay_draws` and `lights` **never part at all**,
  across 96 frames and three runs, in any arm.
- Everything else — `draws3d`, `terrain_nodes`, `terrain_batches`, `water_*`,
  `grass_batches`, the camera and the fog scalars — parts together, one frame later.

The one-frame lead is the useful part, and it is reproducible. The natural reading is that
the divergence begins **inside** a frame, after the world has been assembled and before the
2D pass is: the 2D content of frame *N* already reflects it while the world content of frame
*N* does not, and the world catches up at *N+1*. That points at something read between those
two points in the frame — which is a much smaller region of code to search than "anything
that touches a clock".

That the light set never parts is worth recording too: whatever varies is not the lighting
solution, and any future explanation has to be consistent with lights being stable while
terrain, water and grass are not.

## Where this leaves it

One clock confirmed in the published frame (`Scene::_lodInvWidth`, from
`GetAvgFrameDuration`), three candidates eliminated by measurement, and the remaining cause
localised to what happens between the world draw and the 2D draw of the same frame. Not
solved. Phase 5 step 6 still cannot take its proof, and the queue still cannot be shown
equivalent.
