# ARCH-001 — Auditing FP_269 as a reference architecture: what to take, what not to

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Subject:** `inspiration/otherfork/FP_269_vk_WIP` — an OFP/Poseidon fork with a Vulkan renderer
**Follows:** PERF-016 (a fork had solved the thing we were about to build), PERF-017, PERF-018

The owner asked the right question after PERF-016: *if that fork already had the multithreading
idea we spent hours arriving at, what else does it already have?* Reuse permission has been
granted, so this is a purely technical audit. Five parallel readings, every claim carrying a
file and a line.

The short answer: **two mechanisms are worth real work, about six are worth an afternoon, and
the headline items — their job system, their PhysX, their build — are worth nothing to us.**
And the most valuable thing the audit produced was not about their code at all.

## What the audit found about OUR code, and the experiment that settled it

Two agents independently flagged `World/Terrain/LandSave.cpp:3452` — object admission paced by
`steady_clock::now()` against a millisecond budget — as a SIM-813-class defect: a wall clock
reaching a simulation decision. One phrased it as deciding "whether the object gets created at
all".

**That framing is wrong, and the concern behind it is now measured rather than argued.**

Wrong, because there is a floor: the budget cannot be consulted until `admitFloor` objects have
been attempted (`LandSave.cpp:3517`), so the clock decides the *rate* of admission, not whether
an object ever exists. Every machine admits everything; slower ones take more frames.

Measured, because the AI behaviour oracle from PERF-017 answers exactly this question. Two runs
of `perf_combat` with the streaming budget set **128× apart**, everything else pinned:

```
control, same budget twice   902 ticks, 937,235 fields -> EQUIVALENT
0.5 ms vs 64 ms per update   902 ticks, 937,235 fields -> EQUIVALENT
```

So on this scene the admission clock does not reach an AI decision. **What this does not say:**
the oracle records AI state — target selection, perceived enemy distance, life state — not
collision or physics, and `perf_combat`'s objects may be resident before the window opens. It
is evidence for the scene measured, not a proof for all cases. The right next use of it is a
scene where the camera moves into unloaded terrain.

That is the audit's real yield: not a defect, but a *retired suspicion*, retired with a number.

## Take: worth real work

**1. GPU mip feedback (`ADAPT`, medium).** Their fragment shader computes `textureQueryLod` for
one thirty-second of the texture table per frame and `atomicMin`s it into a readback buffer
(`Shaders.glsl:554-557`, `TextureBank.cpp:132-194`); residency is then driven by *what the frame
actually sampled*. Ours is CPU distance and frame counters (`TextureBankWgpu.cpp:79-200`). WGSL
has no `textureQueryLod`, but the same value falls out of `dpdx/dpdy` of the UV against texture
dimensions, and we already have a `map_async` readback ring to model the transport on
(`rust/src/gfx3d/cull.rs:867-880`). No borrow problem: clear, copy and readback all sit inside
existing single-threaded `&mut` entry points. **This is the difference between a texture budget
that holds on worlds we tuned for and one that holds generally.**

**2. Resumable `CreateTargetList` (`ADAPT`, medium).** Their `AIGroup::CreateTargetList` is a
four-stage state machine with a list cursor (`Poseidon2/aiGroup.cpp:2836`, stages at
`:2843/2870/2883/2986`), and a `time_point::max()` argument means "run to completion", which
keeps a clean one-shot path for scripted callers. Ours is monolithic
(`AIGroupImpl.cpp:457-479`). The resume state — `m_createTargetListStage`,
`m_createTargetListIndex` — is **deterministic**: two integers. Only the *yield decision* reads
a clock, and that is the one line we replace with a work counter. This is the O(units × targets)
block PERF-015 named, and the AI oracle can prove the split behaviour-neutral before it ships.

One defect to not inherit: their cursors are never serialized, so a savegame taken mid-resume
restarts at stage 0 with `_expensiveThinkTime` already advanced and silently skips a stage.

## Take: worth an afternoon

| | What | Verdict |
|---|---|---|
| **JIP time and weather** | They send `DateTimeWeatherMessage` in the join handshake (`networkServer.cpp:2735`). We send **nothing** — `engine/Poseidon/Network/` has zero references to weather, overcast or the clock, verified twice. A player joining a two-hour-old mission arrives at the mission file's start time and weather. | PORT, small |
| **Wire up our own WatchDog** | Already in our tree from an earlier port, compiled, unit-tested, and called by **no production code**. Our version is better than theirs (opt-in instance vs a global that spawns a thread per translation unit). Give it call sites: the threaded `FileServer`, PBO/PAA decode, world load, and the async object-stream adapt path that is disabled over heap corruption. | PORT the call sites, small |
| **Flag-tagged colliders** | Gameplay flags (`isRoadway`, `bulletCollision`, `visibilityCollision`) packed into the collider's query filter word (`fp/physics/World.hpp:194-212`), so one collider set serves fire-geometry, view-geometry and roadway queries. We register one undifferentiated static body per model and cannot express "roadway only". | ADAPT, medium |
| **Bulk parallel model preload** | 65 workers force-load every model the world references at load time (`Poseidon2/landSave.cpp:399`). Not a replacement for streaming, but a `-preloadAll` option answers the 107 s Reforger Everon fill-in directly, and our worker-side decode is already proven safe. | ADAPT, small |
| **`fp/std/sleep`** | `CreateWaitableTimer` on Win32, EINTR-looping `nanosleep` elsewhere. 41 lines, no dependencies. | PORT, small |
| **`TypeBankWithReplacements`** | A name→asset bank with *aliasing*: `SetReplacement(old, new)` makes `Get(old)` resolve to the new asset, `ClearReplacements()` reverts. Exactly a mod/asset-substitution hook. Note its resolution has no cycle guard. | ADAPT, small |

## Do not take, and why

**Their job system.** `TaskScheduler` + `TaskGroups`, 618 lines, two queues (Renderer, Physics),
runtime-swappable priority schemes. Genuinely more developed than anything we have — and it
would buy us nothing today. Its use there is parallel *command recording*, which does not
transfer: their renderer is Vulkan-direct, ours is wgpu behind a C ABI where 62 of 79 entry
points form `&mut *renderer`, and wgpu's `RenderPass` is `!Sync` regardless. **The prerequisite
is a reshaped ABI, not a scheduler.** Its other candidate use here, AI parallelism, shrank 70%
the moment PERF-017's ration landed. It also drags in their whole allocator family, spins on
init, and its move constructor calls `abort()`.

**PhysX.** Vendored, linked, and *not their collision system*: `World::RayCast` is commented out
(`fp/physics/World.cpp:457-571`) and gameplay intersection goes through a hand-written GJK/EPA
library at `fp/intersect/`. PhysX does props, dropped weapons and shell casings. A week of
integration for debris.

**Their build and test setup.** `cmake_minimum_required(VERSION 2.6)`, `Release` and
`-march=haswell` hardcoded, every dependency a vendored `add_subdirectory`, and toolchain files
containing one developer's home directory. Their `fp/test` is described in their own readme as
"junk folder, ignore" — it is in no build and contains no assertions. Both their profiling
macros are compiled out at HEAD (`fp/std/elapsed_time.hpp:1` is `#if 0`). **They have no way to
see their own AI stage at all.**

**Mesh shaders.** `VK_EXT_mesh_shader` throughout their geometry path. wgpu has no mesh-shader
API. Our compute cull path is the only route and this is not a choice we get to make.

**Their transport layer.** Every file in `El/Network/` is the unmodified 2016-2018 BI drop. Our
JIP has a persistent deduped queue, a six-pass ordered sync and world-state persistence; theirs
re-walks the live world with no queue. They are still on the dead GameSpy SDK.

## Where we are ahead, since the question was whether we are behind

Terrain LOD (they have none — `Landscape.cpp:5-21` builds one static grid, `lodMultiplier = 1`),
real object streaming, temporal upscaling (they are FSR 1.0 spatial-only; no TAA anywhere), GTAO,
Hi-Z, a physics backend seam, rate limiting / IP bans / auth / master server, and every piece of
measurement infrastructure: `AICostAccum`'s 14 nested buckets, `StateTimeline`, `CommitQueue`,
`TickStateHash`, the frame-equivalence gate, and as of today `AITimeline`.

And the determinism discipline is ours alone. Their `AICenter::UpdateGroup` still carries the
2001 wall clock we removed under SIM-813 — with a twist both readings confirmed: the loop ends
`while (steady_clock::now() > timeout)`, which continues *while the budget is already exceeded*.
Read as written it is inverted, so the body runs once and their effective ration is one group
per call — which is what our `GroupsPerCycle 1` already gives. **Their headline mechanism does
not do what its comment says.** Their `AIUnit::Think` takes a `timeout` parameter and never
reads it.

## What this does not say

- Nothing here was built or run. This is an architecture comparison, not a performance
  comparison, and no claim is made about which engine is faster.
- One agent reported their game's thread count as "not determined" from `fp/` alone. It is
  determined: `Poseidon2/main.cpp:4370` starts `threadCount - 1` workers from config, minimum 2.
- The streaming-clock experiment covers `perf_combat` and the AI oracle's fields only.
- Their per-mip streaming rests on a substrate we do not have: offline-baked `.glb` + `.gpu`
  sidecars, `.dds` with real mip chains, stored-only ZIP archives. We decode ODOL and transcode
  PAA at runtime. Any per-mip work should be scheduled *with* the question of whether we bake a
  sidecar format first, or it relocates a transcode cost rather than removing one.

## Order

1. JIP time and weather — confirmed gap, small, affects multiplayer we already ship.
2. WatchDog call sites — the instrument is already in the tree and inert.
3. Resumable `CreateTargetList` with a count budget, proven neutral by the AI oracle.
4. GPU mip feedback — the largest genuine win, and the one that wants a design pass first.
