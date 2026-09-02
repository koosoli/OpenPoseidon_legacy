# REN-THR-004 — What happens between the world draw and the 2D draw, and which of it is a function of the clock

**Date:** 2026-08-31
**Branch:** `agent-2d-lead` (from `asset-obs-001`)
**Follows:** [REN-THR-003](REN-THR-003-what-in-the-published-frame-is-a-function-of-the-clock-20260831.md), including its appended *The experiments, run*
**Status:** static audit only. Nothing here was run, no binary was built, the game folder was not touched.

## What this had to explain

REN-THR-003's per-slice instrument produced four facts, reproducible across three environment
arms on `perf_abel`, 96-frame window at frame 400, three runs each:

1. `verts`, `batches`, `cmds` part **exactly one frame before** everything else.
2. `overlay_verts`, `overlay_indices`, `overlay_draws` and `lights` **never part**.
3. `draws3d`, `terrain_nodes`, `terrain_batches`, `water_nodes`, `water_batches`,
   `grass_batches`, `cameras` and the clear/fog scalars part together, one frame later.
4. `WGR_LOD_GOVERNOR_RANGE=1` moves the parting from 51/52 to 59/60 and no further.

The brief asked for everything that happens between the world assembly and the 2D pass, and for
whatever distinguishes `overlay_*` from `verts`. **Two of the four facts turn out not to be
facts about the renderer at all**, and saying so is most of the answer.

## Method, and its denominator

Four things were established by reading, each with the count it produced.

**Sweep 1 — the frame's actual order.** `World::Simulate`'s draw half read end to end
(`World.cpp:1490-1880`), `Landscape::Draw` (`LandscapeRender.cpp:1650-1755`),
`EngineWgpu::InitDraw` / `FinishDraw` / `NextFrame` (`EngineWgpu.cpp:1539-2215`). Written out
below as a list.

**Sweep 2 — every writer of every one of the seventeen slices.** All seventeen located, all
cleared at one point (`EngineWgpu.cpp:2197-2211`) and swapped out at one point
(`EngineWgpu.cpp:2049-2063`). **No slice carries content across a frame boundary**, so the
one-frame lead is a property of the content, not of the buffering. That was checked before
anything was concluded from it, because a double-buffered world slice would have produced the
same measurement for a completely different reason.

**Sweep 3 — every producer that can write `verts`/`batches`/`cmds`.** `AppendTriangles`
(`EngineWgpu.cpp:2220`) is the sole entry point; its eight callers are `Draw2D` (`:2254`),
`DrawPoly`×2 (`:2274`, `:2309`), `DrawLine` (`:2347`), `DrawSection` (`:8861`), `DrawDecal`
(`:8902`), `DrawDecalRotated` (`:9002`), `DrawPoints` (`:9175`). Every call site of those in
`engine/Poseidon/` was then classified against **this scene**. Result in the table below: **five
of the eight are structurally inert in `perf_abel`.**

**Sweep 4 — the clocks.** `GlobalTickCount`, `steady_clock::now`, `GetLastFrameDuration`,
`GetAvgFrameDuration`, `GetTickCount` across `engine/Poseidon/Graphics/`, `engine/Poseidon/UI/`,
`engine/Poseidon/World/Scene/`: **44 hits**, all but three of them in the Options/menu UI, the
SDL input timestamps, or a `LOG_*`. Then `Glob.uiTime` across `engine/Poseidon/`: **63 hits**,
of which the ones that reach a drawn vertex are enumerated under candidate 1.

## The order of a frame, with file:line

Simulation half, `World::Simulate(deltaT, …)` — `deltaT` is the **wall-clock frame duration**
handed down by `GameLoop.cpp:330-333`:

| # | what | file:line |
|---|---|---|
| 1 | `noAccDeltaT = deltaT` — the raw wall clock, kept unaccumulated | `World.cpp:174` |
| 2 | `Glob.uiTime += noAccDeltaT` — **a real clock, accumulated per frame** | `World.cpp:399` |
| 3 | `steps = _simulationSteps.Advance(deltaT)`; `Glob.time += 1/60` per step | `World.cpp:910`, `:1969` |

Draw half, same call:

| # | what | fills | file:line |
|---|---|---|---|
| 4 | `UpdateModernObjectResidency` (object stream admit) | — | `World.cpp:1519` |
| 5 | `CapturePresentationSnapshot()` — sun, fog, camera, **local-light selection** | (feeds `lights`) | `World.cpp:1523`, `PresentationSnapshot.cpp:113-171` |
| 6 | `InitDraw` — clears `_verts`,`_batches`,`_draws3d`,`_cmds`,`_palette`,`_cameras`,`_shadowCasters` | — | `EngineWgpu.cpp:1539` |
| 7 | `BeginObjects`, ballistics/physics dev draws | `verts` (dev only) | `World.cpp:1536-1547` |
| 8 | object gather loop → `ObjectForDrawing` | — | `World.cpp:1631-1685` |
| 9 | `CloudletForDrawing` × `NCloudlets()` | — | `World.cpp:1687` |
| 10 | `EndObjects` (a no-op body) | — | `SceneDraw.cpp:297` |
| 11 | `Landscape::Draw` → `DrawGround` | `terrain_nodes`, `terrain_batches` | `LandscapeRender.cpp:1664` |
| 12 | → `DrawWater` | `water_nodes`, `water_batches` | `LandscapeRender.cpp:1682` |
| 13 | → `DrawHorizont` — **returns immediately when the procedural sky is on** | `verts` (GL33 only) | `LandscapeRender.cpp:1426-1434` |
| 14 | → `DrawObjectsAndShadowsPass1` — **the LOD governor runs here** | `draws3d`, `shadow_casters` | `LandscapeRender.cpp:1692`, `SceneDraw.cpp:1057` |
| 15 | → grass `DrawGround` layers | `grass_batches` | `LandscapeRender.cpp:1725-1732` |
| 16 | → `DrawObjectsAndShadowsPass2` (alpha + shadow depth) | `draws3d`, `shadow_casters` | `LandscapeRender.cpp:1750` |
| 17 | `ObjectsDrawn` → `DrawRain`, `GRain.UpdateAndDraw`, Pass3, `DrawFlares` | `verts` | `World.cpp:1701`, `SceneDraw.cpp:549-570` |
| 18 | `ObserveRenderedFrame` (validator) | — | `World.cpp:1707` |
| 19 | `ResolveSceneToDisplay` | — | `World.cpp:1741` |
| 20 | **the 2D pass**: map, `_ui->DrawHUD`, dialogs, chat, cursor overlay, title/cut effects | `verts`, `batches`, `cmds` | `World.cpp:1745-1845` |
| 21 | `FinishDraw` → `Engine::FinishDraw` records this frame's duration into the 16-deep ring | — | `World.cpp:1855`, `Engine.cpp:325-338` |
| 22 | `NextFrame`: fills `_lights` from the snapshot, builds `cameras`, **publishes + hashes** | `lights`, `cameras` | `EngineWgpu.cpp:1812-2165` |

**So the window the one-frame lead points at is steps 17 through 20** — everything after the
last write to `draws3d` and before the last write to `verts`.

## What is actually in `verts` in `perf_abel`, which is much less than it sounds

`tests/perf/missions/perf_abel.abel/mission.sqm` is **one `SoldierWB`, `player="PLAYER
COMMANDER"`, at (7800, 10100) on Abel, `hour=12`, no vehicles, no other groups, no triggers, no
scripts.** Against that scene:

| producer of `verts` | file:line | live here? |
|---|---|---|
| `DrawHorizont` (legacy horizon quad) | `LandscapeRender.cpp:1431` | **no** — `return` when `ProceduralSkyActive()`, which is `_renderer && _sky.enabled` (`EngineWgpu.hpp:282`) |
| `Scene::DrawFlares`, sun flare | `SceneDraw.cpp:395` | **no** — same `!ProceduralSkyActive()` gate |
| `Scene::DrawFlares`, local-light flares | `SceneDraw.cpp:454` | **no** — gated on `night >= 0.2`; hour = 12 |
| `Scene::DrawRain` / `GRain.UpdateAndDraw` | `SceneDraw.cpp:549-560` | **no** — clear weather, density < 0.1 |
| cloudlets → `Object::DrawDecal` → `EngineWgpu::DrawDecal` | `Object.cpp:1336` | **no** — no particle source in the mission (see below) |
| `SmokeVolume` / `RainVolume` decals | `SmokeVolume.cpp:881`, `RainVolume.cpp:607` | **no** — same |
| `Engine::DrawFinishTexts` (`--fps` text, timed `GlobalShowMessage`) | `EngineDrawing.cpp:33` | **no on WGPU at all** — its only caller is `EngineGL33_Lifecycle.cpp:196` |
| `Shape::Draw` CPU fallback → `ClipShape` → `DrawSection` | `ShapeDraw.cpp:503`, `ClipShape.cpp:253`, `EngineWgpu.cpp:8861` | **yes** — every shape with no `_buffer`, which is every road (`ShapeDraw.cpp:239`) |
| the in-game HUD and dialogs → `Draw2D` / `DrawPoly` | `World.cpp:1745-1845` | **yes** |
| the cursor overlay | `World.cpp:1836` | **yes** |

That table is the audit's main product. **In this scene `verts` is the HUD, the cursor, and the
CPU-path surface shapes. Nothing else.** Every effect-system producer the brief asked about is
structurally off.

### The cloudlet / global-RNG hypothesis, tested and dead for this measurement

REN-THR-003 proposed that `Scene::GetSmokeGeneralization()` (`SceneDraw.cpp:1264`,
`= _lodInvWidth * 0.1f`) scales the cloudlet spawn rate, that each spawn draws ~7 values from the
shared `GRandGen`, and that this converts a small LOD difference into permanent divergence of
every later RNG consumer. The brief asked for it to be tested rather than adopted.

**It is dead here, on the scene rather than on the mechanism.** `perf_abel` has one stationary
infantryman and nothing else — no fire, no vehicle exhaust, no weapon discharge, no scripted
effect, no rain. `World.cpp:1687` iterates `NCloudlets()`, which is zero, so `Smokes.cpp` never
spawns, `Object::DrawDecal` is never called, and `EngineWgpu::DrawDecal` contributes no vertex to
`verts` in any frame of the window. A hypothesis whose entire amplification path is unreachable
cannot be the cause of a divergence measured on this scene.

**The coupling is still a real determinism defect** — a wall-clock feedback loop deciding how many
values come out of a process-global RNG is wrong on its own terms, and it will bite the moment the
measurement moves to a scene with smoke in it. It should get its own ticket. It is simply not what
is being measured.

## Two of the four facts are vacuous, and this is checkable for free

### `lights` never parts because it is empty

`CapturePresentationSnapshot` skips the entire local-light selection unless `night > 0.0f` or some
light `IsDaylightVisible()` (`PresentationSnapshot.cpp:127`). `perf_abel` is `hour=12`; the
mission places no light and no fire. So `snap.lightCount == 0`, the loop at
`EngineWgpu.cpp:1830-1839` runs zero times, `_lights` is empty every frame, and `frame.lights`
hashes to the constant an empty slice hashes to.

### `overlay_*` never parts for the same reason

`SubmitOverlay` (`EngineWgpu.cpp:6966-6991`) clears the three overlay vectors and **returns early**
on `vertCount <= 0 || indexCount <= 0 || cmdCount <= 0`. `farfield-bench.ps1` opens no dev-panel
window, so ImGui's draw data is empty and the early return fires every frame. The three overlay
slices are empty every frame.

**So the brief's item 4 — "whatever distinguishes `overlay_*` from `verts` is probably the shape of
the answer" — resolves to: one of them is empty.** There is no architectural difference to find.
The comparison carries no information, and the same is true of `lights`. This is the failure the
memory note *a diagnostic that cannot say "did not run"* records: four counters in one night that
reported 0 where they meant n/a.

### The zero-cost check

`HashSlice` folds the `uint32_t` length before the bytes and starts from the FNV-1a basis
(`EngineWgpu.cpp:7196-7202`, `:7217`), so **an empty slice hashes to exactly
`0x4d25767f9dce13f5`** — basis, four zero bytes, nothing else. Grep the per-slice logs *already
produced* by the three arms REN-THR-003 ran. If the `lights` and `overlay_*` columns read
`4d25767f9dce13f5`, both facts are confirmed vacuous with no new run and no code change. This is
experiment 0 below and it should be done before anything else in this document is acted on.

## The correction REN-THR-003 needs

REN-THR-003's constraint 2 says: *"Anything downstream of `Glob.time` or `Glob.uiTime` alone is
eliminated by this arm, because both advance by `deltaT` (`World.cpp:399`)."*

`Glob.uiTime` does **not** advance by the simulation step. `World.cpp:174` sets
`noAccDeltaT = deltaT` — the raw wall-clock frame duration, before any accumulator — and
`World.cpp:399` accumulates it. The fixed step appears only inside `StepSimulation`
(`World.cpp:909-916`, `:1969`). **`Glob.uiTime` is a wall clock.**

The elimination happens to hold for the arm that ran it, because `POSEIDON_LOCKSTEP_HZ` pins
`deltaT` at `GameLoop.cpp:330-333`, upstream of both. But **none of the three arms that produced
the per-slice finding used lockstep**, so nothing downstream of `Glob.uiTime` is eliminated by any
measurement now on the table. That is where candidate 1 lives.

## The ranked candidates

### 1. The frame's wall-clock `deltaT`, read two ways — continuous into `verts`, quantised into the world — **LIKELY**

The producer reads the same drifting quantity through two different filters, and the filters have
different sensitivities. That, and not a second clock, is what a one-frame lead looks like.

**The continuous read.** `Glob.uiTime` (`World.cpp:174`, `:399`) accumulates the raw frame
duration. Its consumers on the 2D path are HUD dim ramps whose output is an alpha byte in a vertex
colour:

```
InGameUIDraw.cpp:1594   age   = Glob.uiTime - _lastUnitInfoTime;
InGameUIDraw.cpp:1601   alpha = (piDimEndTime - age) / (piDimEndTime - piDimStartTime);
InGameUIDraw.cpp:1616   age   = Glob.uiTime - _lastGroupDirTime;          // DrawGroupDir
InGameUIDraw.cpp:1625   alpha = (groupDirDimEndTime - age) / (...)
InGameUIDraw.cpp:1684   age   = Glob.uiTime - _hintTime;                  // DrawHint
InGameUIDraw.cpp:1928   age   = Glob.uiTime - _lastGroupInfoTime;         // startDim 90, endDim 95
InGameUIActions.cpp:204 return Glob.uiTime - _dimStart;                   // action menu
```

Each ends in `PackedColorRGB(c, toIntFloor(c.A8() * alpha))` on a `Draw2D`/`DrawPoly` quad, i.e.
directly in `frame.verts` — a hashed slice.

**The quantised read.** `_simulationSteps.Advance(deltaT)` (`World.cpp:910`) returns
`floor(accumulator / (1/60))`. `Glob.time` therefore takes only exact multiples of the step, and
**two runs that have executed the same number of steps have a bit-identical world**: identical
object transforms, identical animation phases, identical camera, identical terrain/water/grass node
selection. That is `draws3d`, `terrain_*`, `water_*`, `grass_batches` and `cameras` — the group
that parts together, one frame later.

**Against the four constraints:**

- **The one-frame lead: explained, and it is the only candidate that explains it structurally.**
  Two readouts of one drifting accumulator. The continuous one parts on the first frame the two
  runs' accumulated wall clocks differ *inside a ramp*; the quantised one parts on the first frame
  the accumulator crosses a step boundary differently. The wall-clock difference that makes the
  first visible is by construction on the order of one frame's drift, which on a 60 Hz step is on
  the order of one step — so the second follows within about one frame. The gap being *one* and
  being *stable across arms* is what this predicts and what was measured.
- **The quiet prefix: explained by the ramps being flat.** While `age < startDim` the alpha is
  exactly `1.0` and two different `Glob.uiTime` values produce byte-identical colours. `DrawGroupInfo`
  has `startDim = 90` seconds (`InGameUIDraw.cpp:1929`). A run publishes nothing different until a
  ramp is entered, and then parts permanently, because the ramps do not return to flat before the
  world has diverged. Same shape as REN-THR-003's clamp argument, on a different mechanism.
- **The governor's eight frames: explained.** `WGR_LOD_GOVERNOR_RANGE=1` changes the per-frame cost,
  therefore the accumulated wall clock, therefore the frame on which a ramp is entered and the frame
  on which the accumulator crosses. **Delay without prevention is what a pacing cause predicts and
  what a content cause does not** — a cause that lived in the LOD ladder would have been removed, not
  postponed, and the measurement said postponed.
- **`lights` and `overlay_*` staying identical: explained trivially** — they are empty (above). This
  candidate does not have to explain a stable light list, because there is no light list.

**What it does not explain.** Why the gap is *exactly* one rather than one-or-two. The argument
above makes one the expected value, not a theorem. If the measurement had ever shown two, the
argument would survive; it has not been tested against that.

### 2. The HUD dim ramps specifically, as the 2D-side reader — **LIKELY, and separable from 1**

A sub-case of 1, listed separately because it is the half that can be removed on its own and the
half most likely to be wrong. Candidate 1's *quantised* read is certain (it is what
`FixedStepAccumulator` does). Its *continuous* read has to be some specific 2D element, and the
audit found the HUD ramps by enumeration, not by observation. The other live producers of `verts` in
this scene — the cursor overlay and the CPU-path road/surface shapes — were checked for clock reads
and have none, but "I found no clock in them" is weaker than "I found a clock in the HUD".

### 3. `Scene::_lodInvWidth` — **CONFIRMED CONTRIBUTOR, not the cause**

`SceneDraw.cpp:1095` (`GetAvgFrameDuration(4)`, a real millisecond clock from
`Engine.cpp:327-338`) drives `_lodInvWidth`, which selects `drawLOD` at `SceneDraw.cpp:806`/`:836`
and reaches `draws3d`. REN-THR-003 measured this and it buys eight frames. Retained in the ranking
because it is proven, and because under candidate 1 its role is intelligible: it is one of the
things that makes one run's frame cost differ from the other's, feeding the accumulator that
candidate 1 says is the actual divergence carrier.

One inconsistency found while reading it and worth recording separately: the loop at
`SceneDraw.cpp:1086-1213` recomputes `AdjustComplexity(_drawObjects)` on every path that changes
`_lodInvWidth` **except** exhaustion of the five-iteration budget, where `_lodInvWidth` is left
changed and `drawLOD` is not recomputed. In that case the frame draws at LODs chosen for the
previous value. Rare, not the cause of anything here, but it is a real one-frame skew inside the
governor.

### 4. `EnsureUploaded` handles in `batch.texture_id` — **UNLIKELY**

`ResolveTexture` (`EngineWgpu.cpp:1487`) returns `TextureWgpu::EnsureUploaded()`
(`TextureWgpu.cpp:1286`), a GPU handle allocated on first use, and that handle is written into
`WgrDraw2DBatch::texture_id` (`EngineWgpu.cpp:2249`) — a hashed field of `batches`. If a 2D texture
were first used at a frame that depended on streaming progress, `batches` would change while
`draws3d` (whose materials are baked at model registration) would not, which is the right shape.

Against it: the lease documented at `TextureWgpu.cpp:1230-1262` makes a slot belong to the
`TextureWgpu` for the session, so an eviction and re-upload reclaim the same index; `_gpuHandle`
and `_uploadTried` make the value stable after first use; and by frame 400 the HUD atlas has been
resident for hundreds of frames. It also cannot explain the governor arm's eight frames.

### 5. The software occlusion buffer — **ELIMINATED for this scene**

`GetOcclusions()->TestSphereWSpace` (`SceneDraw.cpp:424`, `:492`) is a world-derived value read
after the object passes, exactly in the window the lead points at, and it is the sort of thing that
could carry a one-frame skew. Its only consumer in `verts` is `DrawFlares`, and `DrawFlares` emits
nothing in `perf_abel` (both its blocks are gated off — see the producer table). Dead here, worth
re-checking on a night scene.

### 6. `Engine::DrawFinishTexts` — **ELIMINATED structurally, not by the harness**

REN-THR-003 flagged the `--fps` readout as a confounder and cleared it by checking that
`farfield-bench.ps1` does not pass `--fps`. It is cleared more strongly than that: `DrawFinishTexts`
has exactly one caller in the tree, `EngineGL33_Lifecycle.cpp:196`. `EngineWgpu::FinishDraw`
(`EngineWgpu.cpp:1592`) calls `Engine::FinishDraw()` directly and never calls it. **On WGPU the FPS
text, the LOD readout and the whole real-clock-TTL `GlobalShowMessage` text list
(`EngineDrawing.cpp:171-179`, `:277`) never reach `_verts` at all**, whatever flags are passed.

## The experiments, cheapest decisive first

### Experiment 0 — free, no run, do this before anything else

Grep the per-slice hash lines the three arms already produced for `4d25767f9dce13f5` in the
`lights`, `overlay_verts`, `overlay_indices` and `overlay_draws` columns.

- **Confirms** the two vacuous facts if those columns carry that value. Facts 2 of the four is then
  withdrawn and no future explanation has to account for it.
- **Eliminates** if they carry anything else — in which case the light list and the overlay really
  are non-empty and stable, that is a genuine and surprising fact, and candidate 1 owes it an
  explanation it does not currently have.

### Experiment 1 — both known wall clocks off at once. No code change.

```
WGR_FRAME_HASH=1 WGR_FRAME_HASH_FIRST=400 POSEIDON_LOCKSTEP_HZ=60 WGR_LOD_GOVERNOR_RANGE=1
```

Three runs. **This combination has never been run.** `POSEIDON_LOCKSTEP_HZ` pins `deltaT` at
`GameLoop.cpp:330-333`, upstream of both `Glob.uiTime` (`World.cpp:399`) and
`_simulationSteps.Advance` (`World.cpp:910`) — so it removes candidate 1 entirely.
`WGR_LOD_GOVERNOR_RANGE=1` removes candidate 3, which lockstep does **not** remove because
`GetAvgFrameDuration` measures rendered frames regardless of what `deltaT` was pinned to. Lockstep
alone was measured (REN-THR-002, 19 frames) and read as a refutation; it was not one, because
candidate 3 was still live in that arm and the instrument was a single fold.

- **Confirms** if all seventeen slices agree for the full 96 frames. Candidate 1 is then the cause,
  the remaining Phase 5 step-6 prerequisite is "run the proof under lockstep with the governor
  frozen", and the proof becomes available immediately.
- **Partially confirms** if the parting moves substantially later but not to 96 — one more clock,
  and the per-slice column that parts first names which subsystem.
- **Eliminates** if the parting stays near 59/60. Both known wall clocks are then provably inert and
  the cause is a third one this audit did not find.

### Experiment 2 — log the slice LENGTHS next to the hashes. One line.

`HashPublishedFrame` already has every slice in hand (`EngineWgpu.cpp:7222-7233`). Adding
`frame.verts.length`, `frame.batches.length`, `frame.draws3d.length` to the existing `LOG_INFO`
answers, in one run, the question no arm has asked: **at the frame `verts` parts, is its length the
same?**

- Same length ⟹ the difference is in vertex *values* — a colour byte or a position. That is
  candidate 1/2 (an alpha ramp) and rules out "a quad appeared".
- Different length ⟹ a 2D producer emitted a different number of primitives, which points at the
  producer table above rather than at a ramp.

That file belongs to another session, so this is stated as a recommendation, not a patch — the same
constraint REN-THR-003 recorded.

### Experiment 3 — log the step count next to the hash. One line, and it tests candidate 1 directly.

`Dev::LastFixedStepFrame()` (`FixedStepStats.hpp:42`) already publishes `steps` and `alpha` every
frame; today only the dev panel reads it (`FixedStepTab.cpp:32`). Log `steps` on the hash line, then
compare, across two runs: **the first frame at which `steps` differs**, against the frame `verts`
parts and the frame `draws3d` parts.

- Candidate 1 predicts: `steps` first differs at the frame the **world** slices part, and `verts`
  parted the frame before.
- If `steps` first differs several frames before or after either parting, candidate 1's account of
  the one-frame gap is wrong and should be dropped even if experiment 1 confirmed the pacing.

### Experiment 4 — take the HUD out. Smallest code change, separates candidate 2 from candidate 1.

Gate the in-game UI block at `World.cpp:1766` on an env var:

```cpp
static const bool noHud = std::getenv("POSEIDON_NO_HUD") != nullptr;
if (!noHud && _ui && IsUIEnabled() && !_cameraEffect)
```

Three runs at window 400, everything else default. There is no existing lever — `IsUIEnabled()`
(`WorldImpl.cpp:256`) reads the options object and nothing sets it from the command line.

- **Confirms candidate 2** if `verts` stops leading — i.e. the 2D slices now part on the same frame
  as the world, or stop parting.
- **Eliminates candidate 2** if the lead survives with the HUD gone. The 2D-side reader is then the
  cursor overlay or a CPU-path surface shape, both of which this audit read and found no clock in,
  which would mean the audit missed one.

### Order

0 (free) → 1 (no code) → 2 and 3 (one log line each, run together) → 4 (one gate).

If experiment 1 confirms, 2–4 are still worth running: they say *which* of the two reads carried it,
and that is what a proof has to name.

## What could not be determined statically

- **Whether the HUD panels are actually inside a dim ramp at frames 400–500.** This is the load-
  bearing part of candidate 2 and it is unverified. `_lastGroupInfoTime` and friends are set at
  `InGameUI.cpp:976-996` relative to `Glob.uiTime` at construction, with dim windows of 90–95
  seconds (`InGameUIDraw.cpp:1929`); whether frame ~450 of a bench run lands inside one depends on
  the run's wall-clock age at that point, which is not a static property. Experiment 4 answers it
  without needing to know.
- **Whether `frame.verts.length` differs at the parting frame.** Experiment 2. This is the single
  most informative unmeasured number and it costs one log line, the same shape of recommendation
  REN-THR-003 made about per-slice hashing — which turned out to carry more information than
  anything else in that document.
- **Whether the overlay and light slices are genuinely empty.** Argued from the mission file and the
  early returns; experiment 0 settles it from logs that already exist.
- **Whether the CPU-path surface shapes (roads) contribute a varying vertex count.** They are a live
  `verts` producer in this scene (`ShapeDraw.cpp:503` → `ClipShape.cpp:253` → `DrawSection`), their
  clipping is per-frame, and the road conform has been changed twice this week. No clock was found
  in that path, but the count it emits was not established.

## What this does not say

- **It does not claim candidate 1 is proven.** It is the only account found that explains the
  one-frame lead structurally rather than by coincidence, and it explains the governor's eight
  frames as postponement rather than removal, which the measurement showed and a content cause
  cannot. That is an argument. Experiment 1 is one variable pair and one run set.
- **It does not say the RNG/cloudlet coupling is harmless.** It says the coupling is unreachable in
  `perf_abel`, so it cannot be what these three arms measured. It remains a determinism defect.
- **It does not enumerate the Rust side.** The hash is taken before `wgr_render_frame`, so a
  divergence originating in the renderer is invisible to this instrument — REN-THR-002's closing
  caveat, still standing.
- **One scene.** Three of the eliminations above (flares, rain, cloudlets, the empty light list) are
  eliminations *on this mission*, not on the engine. A night scene or a scene with smoke would
  reinstate all of them, and would also make the `lights` and `overlay_*` columns say something.

> **Stale lever, verified 2026-09-02 (AUD-001):** `POSEIDON_NO_HUD` has NO READER IN THE TREE — `rg -w` over engine/, apps/, tools/, tests/, scripts/ returns nothing. Setting it does nothing. Do not run a confirming experiment with it.
