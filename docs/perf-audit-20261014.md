# Performance audit, 2026-10-14

Branch `perf/optimization-20261014`, backed up on `safety`, deployed to the game folder as
`42b22f79e`.

Every change was found by sampling the process, and none of them changes what the game shows.
- Each one is either exact by construction, with a verify lever that compares old and new
  results in a live run, or switchable by an env lever for an A/B in one binary.
- Numbers are from this machine: 16 logical cores, RTX, 1920x1080, DLSS Quality unless stated.

## How it was measured

- **Sampler.** An external sampling profiler (scratchpad `sampler.py`: SuspendThread +
  StackWalk64 + offline PDB symbolisation with line numbers). nsys and WPR need admin.
- **Scenes.**
  - Native Reforger Everon, the Durras freefly pose: `run-world.ps1`.
  - Stock Everon firefight: `tests/perf/missions/perf_combat.eden`, via `farfield-bench.ps1`.
  - Stock Everon start-up.
- **Frame figures.** "Main-thread work" is the CPU frame minus `swap`, i.e. what the main thread
  did rather than waited for. Durras with DLSS is GPU-bound, so main-thread savings there show up
  as headroom, not FPS.

## Load times

| | before | after |
|---|---|---|
| Native Reforger Everon, `SwitchLandscape` | 46.2 s | **8.6–8.8 s** |
| Native Everon, launch → mission running | — | ~14 s |
| Stock Everon, renderer creation | 5.0 s | 3.1 s (NGX-bound, see below) |
| Stock Everon, world load + mission start | — | 1.4 s |

What it took on native worlds:
- **Pak handle.** One shared overlapped `PakFile` handle per archive. Before, every member read
  opened and closed an ifstream: ~30% of the load.
- **Allocator.** `JimboAllocator` accounting is batched per thread. Four to six shared atomics per
  new/delete took 82% of the converter threads' time.
- **Parallel model building.** The 1,429 attachment part models and the 2,398 road ribbons build
  on the task pool. Registration stays serial and in the original order, so object order and
  names are unchanged.
- **Parallel textures.** Each install batch prepares exactly the textures it is about to load, on
  the pool (`PrepareNativeInstallTextures`). Verified with `WGR_NATIVE_DDS_PREPARE_VERIFY=1`:
  447 of 447 taken, 447 identical.
- **Task pool cap 8 → 16 threads.** Load −1.2 s, and −2.4 ms of main thread in the firefight.
- **Prefab pre-pass.** Every distinct placed prefab (and, round by round, the slot parts and
  children it leads to) has its model, slots, children, placement mode and flags resolved on
  the pool before the serial pass, which reads the answers in its original order. A hash over
  the whole resolve output is identical with it on and off. Resolve 3.4 → 1.85 s.
  `POSEIDON_PREFAB_PREPASS=0`.
- **MLOD round trip.** The reader scans face strings in place instead of a character at a time,
  and the writer sizes its buffer up front. Material indices are looked up once per run of
  faces, and a layer tint's mask means are memoised per file. Part models 2.48 → 1.57 s.
- **Parts and roads convert on the pool.** The bank tables are built on the main thread in model
  order, and the tables-mode conversion runs on the pool (the streamer's SIM-807 split); the
  bank adopts each shape. 1,375 parts hashed identical to the in-place conversion.
  Part install 2.28 → 1.2 s. `POSEIDON_PART_POOL_ADAPT=0`.
- **Road textures.** A tinted name whose inner image is an alpha composite cannot carry the
  lazy tint view and falls back to the full name; the install prep now prepares that name too
  (556 prepared sources identical to the main-thread decode). Roads 1.87 → 1.33 s.
- **Placement sort.** The 1.22 M placements are sorted by a precomputed (distance², index) key
  instead of a `stable_sort` that recomputed both distances per comparison (same order,
  verified).

## Frame time

**Native Everon, Durras.**
- Main-thread work per frame dropped by about 5 ms in total:
  - The HUD cursor ray: from 11–15% of the main thread to 0.5%. Line queries cull cells by a
    per-cell reach (`POSEIDON_OBJCOLL_CELL_CULL=0|verify`), and the cursor ray stops at its first
    ground hit.
  - Per-section string work: `sscanf` in tint names, a `getenv` per blend object, and
    `std::string`-keyed material caches under a mutex. Answers are now memoised on the material
    and texture objects. The transparent pass's `alphaDraw` went 3.4 → 1.7 ms.
- The 60–150 ms canopy-map hitch every 1.5 s is gone: the map builds on its own thread.
- During the far-tier fill, BC3 encoding of leaf composites splits block rows across the pool,
  byte-identical: 11.2% → 1.9% of the busy main thread.
- Streaming smoothness: every canopy change recomposed the 2048² sky-visibility map on the
  render thread (~160 ms each, every few seconds while objects stream in). It now composes on
  up to 8 threads, with identical texels (unit test). Fill frame p95 89–98 → 76–78 ms, frames
  over 100 ms 49–53 → 31–32. `WGR_SKYVIS_COMPOSE_THREADS=1` = serial.
- GPU:
  - Far shadow cascades reuse last frame's depth while it still covers the view: −2.1 ms.
  - Mid-ring grass placement tests the frustum first: −0.45 ms.

**Stock Everon firefight.**
- The simulation step went 7.6 → ~5.6 ms:
  - corpse-pose validation (a full re-check per tick and a CRT call per float);
  - `MotionType::FindPath` caching;
  - a `dynamic_cast` per soldier in three per-frame vehicle scans;
  - terrain-hole gathering per soldier;
  - prefetching in the scene walk and in target tracking.
- p95 frame 33 → 25–29 ms.
- Later the same day, also exact:
  - Soldiers lock building path points through a per-cell mirror of buildings:
    `AILocker::LockPosition` 3.8% → 0.5%.
  - Vehicle-type lookups by name are answered from a validated cache (every footprint did a
    linear type scan).
  - `Head::SetMimicMode` skips its config lookup when the mode is unchanged.
  - The A* field cost reuses each unit's exposure terms within a search batch.
  - A soldier type's skeleton weights are prepared once rather than once per animated move:
    mission start `InitVehicles` 1.03 → 0.83–0.99 s.

## 2001-era limits found

| limit | verdict |
|---|---|
| Task pool capped at 8 threads | **raised to 16**: measured load and frame gains, no regression |
| `InvSqrt` lookup table, approximate `Size()` | **replaced** with exact `sqrt` |
| Per-member file reopen in archive reads | **fixed**: shared handle |
| File-server cache (`fileHeapSize`) | already 256 MB; fine |
| `maxSounds = 6` audible voices | an audio-fidelity limit, not a performance one; left alone |
| LOD governor set point | already modernised (60 fps); left alone |
| Object-stream workers = 2 | **kept**: 4 and 6 measured no faster, the fill is bounded by per-frame admission caps by design |

## Measured and rejected

- **AVX2 build (x86-64-v3).** No change in load, part building or the streaming fill.
- **Worker-ahead native DDS preparation.** Fill 30–33 s against 24 s, from store overflow and
  expiry.
- **Preload mode.** The world fills 6.5 s sooner, but the first frame freezes for 7 s.
- **Deferring the DLSS (NGX) join past renderer creation.** Would save ~1.3 s of start-up on RTX.
  - MSAA is fixed at creation, so a late probe failure would lose it, and the owner runs 4x.
  - The September black-frame bug came from exactly this lazy design.
- **Terrain and grass on their own init threads.** Only ~0.1 s, because each init thread builds its
  own shader composer and they contend.
- **Preparing the far tier's `.emat` maps ahead of registration.** The names are a superset,
  the preparation took 17.6% of the main thread, and the far fill got slower (36.5 s against
  33.5 s).
- **Removing the grass depth prepass.** It also writes the normals AO reads, so the image would
  change.
- **A read cache for prefab files during the resolve.** It answered 95% of reads from memory, but
  once the pre-pass ran the reads in parallel it measured as noise (prepass 335 against 357 ms).
- **A mirror of each cell's non-static objects for off-screen cells in the draw walk.**
  Exact once it followed SmallArray's slot reuse (18,500 cells verified), but the CPU frame did
  not move (20.3 ms either way): only ~20 such cells a frame.
- **Prefetching ahead in `Scene::AdjustComplexity`.** Its profile share did not drop (5.0% with,
  4.2% without). The draw walk's prefetch, measured in the same A/B, does help (walk self time
  6.4% with it, 7.9% without).
- **Batching the streamer's texture preparation across an update's models.** Bank-table time per
  update fell from 30–160 ms to ~1 ms, but the admit loop is time-budgeted, so it just admitted
  more per update: the fill took 19.0–19.7 s either way and the fill's frame-time p95 did not
  improve.

## What is left, largest first

0. **The firefight frame is balanced across three limits** (stock Everon, 1600×900, DLSS):
   main thread ~20 ms (draw 39%, sim 38%, nothing above 6%), render thread ~99% busy (wgpu's
   command encoding at `finish()`: about a third inside the Vulkan driver, plus binding
   flushes, render-pass begins and texture-usage tracking), GPU ~20 ms. The common factor is
   the 3,767 direct per-draw ops: skinned soldiers, weapons and effects in each pass. Moving
   skinned meshes onto the retained, instanced path with per-instance palettes would cut the
   render-thread and GPU costs together. That is a renderer project, not a tweak.

1. **Native roads.** 2,398 legacy blend objects cost ~2 ms GPU and ~1.6 ms main thread at Durras.
   That is per-draw overhead, not fill (`POSEIDON_REFORGER_NO_ROADS=1` is the A/B). Batching them
   changes how roads blend against glass; drawing them in the surface pass is the safer first step.
2. **Main/render overlap lost while models register.** Creating a texture from the main
   thread first waits for the render thread to finish its in-flight frame
   (`AcquireProducerWindow`). Measured during the native fill: **9–11 ms of main-thread wait
   per frame** (`render thread window: ... lazyWait=11.05 ms`), i.e. roughly half of a fill
   frame, for ~20 s after every native load. Stock islands create almost no textures after
   load (lazyWait ≈ 0). The fix is to queue the creates the way the instance and mesh ops
   already are. The renderer's registry assigns texture handles and slot leases, so the C++
   texture lifecycle needs pre-assigned handles or slots. That is a deliberate FFI change in an
   area with a history (the overlap race of 2026-09-02), so it is written up rather than
   rushed.
3. **Far-tier registration textures.** The `.emat` layer maps are loaded on the main thread inside
   `RegisterGpuModel`, ~12% of the main thread during the far fill. The same prepare-then-take
   pattern applies, but the names come from the renderer's material translation.
4. **Native world file parse.** ~0.8 s of the resolve is reading the 1.2 M-entry `.ent`; a third
   of that is the placement vector regrowing (and being re-zeroed by the engine heap) under a
   256 MiB single-allocation cap.
5. **Geography refresh.** ~18 ms every ~2 s while native objects stream, mostly rebuilding the
   grass road/footprint cell table.
6. **The engine allocator zero-fills every `new`.** Callers rely on it, so it cannot simply stop.
   It is ~6% of the main thread during streaming, through large buffers that are overwritten at
   once.
7. ~~Mid-ring grass vertex shader~~. **Measured, not the bottleneck.** With the per-vertex wind
   and rotor-flutter noise stubbed out, the grass timers did not move (colour 2.03 ms, prepass
   1.71 ms), so grass cost on stock worlds is not that ALU. Left alone.

## Is a Nanite-style geometry system worth it? (measured)

At the Durras pose (native Everon, fixed camera, two runs per arm), the native LOD rule was
coarsened (`POSEIDON_XOB_LOD_TPP` 0.8 → 0.2):

| | default | coarse LODs |
|---|---|---|
| main-pass triangles | 7.1 M | 5.0 M (−30%) |
| shadow-cascade triangles | 26.0 M | 15.0 M (−42%) |
| GPU frame | 22.7–23.0 ms | 19.95–20.1 ms (−12%) |
| solid colour + prepass | 2.8 ms | 1.8 ms |
| shadow cascades 1 + 2 | 3.6 ms | 2.2 ms |

So geometry is a real but minority cost (about 12% of the GPU frame for a 30–40% triangle cut).
The rest is shading, blending and per-draw cost. A full Nanite port (cluster hierarchy,
visibility buffer, software rasterizer) is not justified: wgpu has no stable mesh shaders, and
the visibility buffer needs 64-bit atomics. The triangle sink worth attacking is **shadow
cascades 1–2: 20 M triangles for 34,000 casters**, 3.6 ms. The cascade cull tests only the
cascade box's side planes. A receiver-aware cull (drop casters whose swept shadow cannot reach
the camera frustum) would recover part of it. It is exact only if the cascade depth cache is
invalidated when the receiver region changes, and if the passes that sample shadows outside the
view (planar water reflection, which shares `frame.wgsl` lighting) get their own receiver
region. Estimated 1–2 ms.

The firefight is too non-deterministic for small A/Bs: between runs of the same arm, direct draws
ranged from 2,862 to 4,365 and the GPU frame from 15.7 to 25.4 ms. Draw-batching work there
needs a deterministic bench first.

## Deterministic firefight bench, and what it found

`perf_combat` as benched varies too much for A/Bs. A fixed free-fly camera over the battle
(`--test-world-freefly 6100 6440 172 75 -10`; Y is absolute) with `POSEIDON_LOCKSTEP_HZ=30` and
the LOD governor pinned gives identical instance and triangle counts every run, direct draws
within ±6, and CPU frame within ±0.6 ms. GPU frame totals still wander, so compare per-pass
timers and profile shares.

Exact wins found with it (both deployed):
- Per-section texture-name classifiers (bark/trunk, plant cutout, coverage, constant-alpha
  glass, the uniform-cloth admission) are memoised on the texture: `DrawSectionTL` 9.6% → 7.3%
  of the main thread.
- Section blend routing asks the same name questions through a memo on the base `Texture`:
  `SectionIsBlend` 2.2% → 1.0%.
- CPU frame on the bench: 23.6–24.8 → 22.7 ms.

**Objects with transparent parts are drawn once, not twice** (soldiers, vehicles, glazed
buildings). Before, pass 2 re-ran each one's whole draw (`Animate`, every section walk, proxies,
`Deanimate`) only to emit its blend sections back to front. Now pass 1 also walks the blend
sections inside the same draw, under the BlendOnly filter and the same depth-read-only state.
The engine holds those `DRAW_3D` commands in a side list, and pass 2 splices them in at the
object's sorted slot (`GBlendRecord`, `Engine::BlendRecord*`; wgpu only, GL33 keeps the old path).
- Main thread on the bench: `DrawObjectsAndShadowsPass2` 20.2% → 11.3%. Pass 1 is unchanged
  (12.6% → 12.4%), `Man::Draw` 12.9% → 9.6%, and the main thread was busy ~15% less over the
  same window.
- `POSEIDON_BLEND_RECORD=verify` records and still draws twice, then compares the records byte
  for byte. Palette slot, camera and same-frame mesh handles are compared by what they resolve
  to. Results: Durras 0 mismatches in 169,788; bench 4 in 206,168, two explained kinds.
  1. One frame where a soldier's mesh switches to GPU skinning. The recording matches that
     frame's opaque parts.
  2. Muzzle flashes. `_mGunFireFrames = 1` is meant to guarantee one visible frame, but the old
     second draw decremented it first, so a flash older than 50 ms never showed. The recording
     restores the one-frame flash; the counter now ticks once per frame, as written in 2001.
- Screenshots on vs off differ by less than two runs of the same arm.

Found, not taken:
- **Corpse pose validation** could not drop its per-tick full `Valid()`: a unit test pins that a
  binding corrupted after capture is refused.
- **Durras transparent pass** (`ops post-water`, 3.3–3.6 ms GPU): roads ~1.1 ms, plus ~540 tiny
  window-glass and net-fence draws at ~4 µs GPU each (2.2 ms for 4,252 triangles).
  wgpu indirect validation is not the cause (A/B: no change).

## Levers added

| lever | default | effect |
|---|---|---|
| `POSEIDON_OBJCOLL_CELL_CULL` | on | `0` = full cell walk; `verify` = both, logs mismatches with hit counts |
| `POSEIDON_DRAW_WALK_PREFETCH` | on | `0` = scene walk without prefetch |
| `POSEIDON_TRACK_TARGETS_PREFETCH` | on | `0` = target tracking without prefetch |
| `POSEIDON_STREAM_TEXTURE_PREP` | on | `0` = streamed bank tables without texture preparation |
| `POSEIDON_PREFAB_PREPASS` | on | `0` = native placement resolve answers each prefab in the serial pass |
| `POSEIDON_PART_POOL_ADAPT` | on | `0` = native parts and roads convert in place on the main thread |
| `POSEIDON_GETENV_ONCE(name)` | — | macro: a per-site cached environment switch, for per-frame and per-object paths |
| `POSEIDON_BLEND_RECORD` | on | `0` = draw blend-owning objects twice; `verify` = both, compare, log counts every 300 frames |
| `WGR_SHADOW_CASCADE_CACHE` | on | `0` = every cascade every frame |
| `WGR_DLSS_ASYNC_INIT` | on | `0` = synchronous NGX probe |
| `WGR_INDIRECT_VALIDATION` | on | `0` = skip wgpu's indirect-argument validation |
| `--max-threads=N` | auto, ≤16 | task pool size |

The full engine test suite matches its baseline: the same 13 pre-existing failures in 5 files,
and every new test passes.
