# Performance audit, 2026-10-14

Branch `perf/optimization-20261014`, backed up on `safety`, deployed to the game folder as
`f801a13ba`.

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
| Native Reforger Everon, `SwitchLandscape` | 46.2 s | **13.1 s** |
| Native Everon, launch → mission running (final build, measured) | — | 17.9–18.4 s |
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

## What is left, largest first

1. **Native roads.** 2,398 legacy blend objects cost ~2 ms GPU and ~1.6 ms main thread at Durras.
   That is per-draw overhead, not fill (`POSEIDON_REFORGER_NO_ROADS=1` is the A/B). Batching them
   changes how roads blend against glass; drawing them in the surface pass is the safer first step.
2. **Main/render overlap lost while models register.** Creating a texture from the main
   thread first waits for the render thread to finish its in-flight frame
   (`AcquireProducerWindow`: 'texture create', waits up to hundreds of ms in the log). Every
   frame that registers a model therefore runs main and render back to back. The fix is to
   create textures through the render thread rather than around it. That is architecture, in
   an area with a history (see render-overlap notes), so it is left for a deliberate change.
3. **Far-tier registration textures.** The `.emat` layer maps are loaded on the main thread inside
   `RegisterGpuModel`, ~12% of the main thread during the far fill. The same prepare-then-take
   pattern applies, but the names come from the renderer's material translation.
4. **Native placements resolve.** 3.6 s of the load, a third of it waiting on serial prefab and
   `.xob` reads.
5. **Geography refresh.** ~18 ms every ~2 s while native objects stream, mostly rebuilding the
   grass road/footprint cell table.
6. **The engine allocator zero-fills every `new`.** Callers rely on it, so it cannot simply stop.
   It is ~6% of the main thread during streaming, through large buffers that are overwritten at
   once.
7. **Mid-ring grass vertex shader.** Per-blade and per-clump values are recomputed for every
   vertex: ~12 M vertex invocations per pass on stock worlds, which are GPU-bound.

## Levers added

| lever | default | effect |
|---|---|---|
| `POSEIDON_OBJCOLL_CELL_CULL` | on | `0` = full cell walk; `verify` = both, logs mismatches with hit counts |
| `POSEIDON_DRAW_WALK_PREFETCH` | on | `0` = scene walk without prefetch |
| `POSEIDON_TRACK_TARGETS_PREFETCH` | on | `0` = target tracking without prefetch |
| `POSEIDON_STREAM_TEXTURE_PREP` | on | `0` = streamed bank tables without texture preparation |
| `WGR_SHADOW_CASCADE_CACHE` | on | `0` = every cascade every frame |
| `WGR_DLSS_ASYNC_INIT` | on | `0` = synchronous NGX probe |
| `WGR_INDIRECT_VALIDATION` | on | `0` = skip wgpu's indirect-argument validation |
| `--max-threads=N` | auto, ≤16 | task pool size |

The full engine test suite matches its baseline: the same 13 pre-existing failures in 5 files,
and every new test passes.
