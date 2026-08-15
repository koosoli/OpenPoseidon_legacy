# RND-037 — Pass1 is 94% submission, it is linear, and two standing leads are dead

**Date:** 2026-08-15
**Branch:** `feat/texture-streaming` (merged with `feat/farfield-tier`)
**Status:** MEASURED on Arma 3 Stratis, settled, 2 repeats, `contended=false`.
**Related:** [RND-035](RND-035-the-lod-governor-was-costing-40-percent-of-the-frame-20260815.md),
[RND-036](RND-036-texture-mip-upload-cost-one-vfs-open-per-level-20260815.md)

## The split

`Scene::DrawObjectsAndShadowsPass1` is `land:obj`, and on a settled Stratis that is ~83% of
the CPU frame. `WGR_PASS1_STATS=1` splits it. Stratis, settled, governor 16, cumulative over
1,024 frames at 11,974 objects/frame:

| sub-phase | ms/frame | share |
|---|---|---|
| **submit** (instanced-run batching + `DrawSortObject`) | **35.453** | **94.3%** |
| `AdjustComplexity` | 1.040 | 2.8% |
| `SortDrawMergersByShape` | 0.857 | 2.3% |
| `_drawMergers` rebuild | 0.197 | 0.5% |
| compaction (`notUsed` sweep) | 0.046 | 0.1% |
| software occlusion pre-pass | **0.000** | 0% |
| total | 37.593 | |

## Two leads are dead

**`AdjustComplexity` is 1.04 ms.** The standing recommendation was to A/B
`WGR_SHADOW_LOD_PROP_CACHE` first, because `AdjustComplexity` runs in this phase and rescans
the object set up to 7 times. It does — and the whole of it is 2.8% of the phase. Even a
perfect memo cannot return more than ~1 ms of 37.

**The software occlusion pre-pass never runs.** `occlusion=0.000` with `occTests/frame=0`.
Established first by inspection: `EngineWgpu::PushSceneCamera` executes
`EnableObjOcc = !_cullDebug.occlusion` *every frame* while the GPU-driven path is on,
`CullDebugSettings::occlusion` defaults to true, and `_cullDebug` is only written by the dev
panel's Culling tab, which a non-interactive benchmark never opens. Every arm logs
`gpu_driven=true`. Then confirmed by the counters. It costs nothing because it is absent —
which is a different statement from "it is cheap", and the distinction matters: an off-only
lever would have measured "no effect" and been read as the latter.

## A correction: Pass1 is linear

An earlier commit on this branch claimed Pass1 was superlinear, inferring it from the two
governor arms: 4,337 → 12,477 instances (×2.88) for 67.400 → 309.530 ms (×4.59), i.e.
per-instance cost apparently rising 15.5 → 24.8 µs.

**That inference was wrong.** Those two arms differ in LOD selection as well as instance
count, so the objects are not merely more numerous but individually heavier — 1.03M → 3.72M
triangles is 238 → 298 triangles per instance. Comparing across a lever that changes two
variables cannot isolate either.

Within a single arm, submit cost per object is **3.27 → 3.03 → 2.96 µs** as the working set
grows 3,986 → 9,050 → 11,974. Flat, and if anything falling as batching improves. Pass1 is
linear in the object set. It is simply expensive per object, and there are a lot of objects.

So the target is a flat ~3 µs per object in the submission loop, not an algorithmic blowup.
At 12k objects that is 35 ms/frame, which is the whole Stratis problem.

## The bigger finding: Stratis is in Pass1 at all, and Everon is not

Same binary, same build, both settled, `WGR_PASS1_STATS=1`:

| | objects/frame reaching Pass1 | registered instances |
|---|---|---|
| Reforger Everon | **1** | 199,881 |
| Arma 3 Stratis | **11,974** | 75,274 |

Everon registers **2.7× more** objects and puts essentially **none** of them through the CPU
draw path; Stratis registers fewer and puts ~12,000 through it every frame. Pass1 on Everon
costs 0.068 ms. On Stratis it costs 37.6 ms.

That is the actual difference between the two worlds, and it reframes the problem. Optimising
the submission loop would be chasing a ~3 µs/object cost that Everon does not pay **at all**,
because `Scene::ObjectForDrawing` never creates a `SortObject` for a `GpuDrawCoverage::Full`
object — it is diverted onto the GPU-driven compute/indirect path. Everon's models qualify.
Stratis's do not.

So the question is not "why is submission 3 µs" but **"why does an Arma 3 model fail the Full
coverage test when a Reforger model passes it?"** Full coverage requires a self-contained
shape: no transparent/decal sections, no nested proxies. Something systematic about the
imported Arma 3 material or section layout is failing that, and fixing it would remove the
35 ms rather than shrink it.

**Not yet measured:** the actual `gCovFull` / `gCovPartial` / `gCovNone` split, which would
say this directly rather than by inference. It is logged only by the one-shot PERF-shapes
dump (`triPerfDumpShapes`), which a non-interactive run never arms. Wiring that to an env var
is the next cheap step, and it should come before any work on the submission loop itself.

## Where to look next, in that loop

Not yet measured — these are the candidates the code presents, in the order they are cheap
to test:

1. `DrawSortObject` calls `GEngine->GpuDrivenCoverage(obj)` per object, then `obj->Draw(...)`.
2. The batching predicate re-reads `sShape->Special()`, `GetObjSpecial()`, `Static()`,
   `NProxies()` per candidate while scanning a run, and `render::SplitLegacy` per head.
3. `DrawSectionTL` is already recorded as doing a second `_materialBindings.find` and three
   `canonical()` heap allocations per section per draw — per SECTION, so it multiplies by
   sections-per-object, not objects.

(3) is the only one already known to allocate on the hot path, and allocation at ~3 µs/object
scale is the right order of magnitude. Split the submit scope further before optimising
anything.

## Governor default flipped to 16

Recorded here because it is the same measurement. `WGR_LOD_GOVERNOR_RANGE=4` is +66% on
Everon (30.51 → 50.34 fps) and 12.53 → 2.89 fps on Stratis, both settled at identical
residency, reversed arm order. A default validated on one world and refuted on the next is
not a default, so 16 — the value two decades of shipped behaviour agree on — is restored and
4 becomes opt-in. Neither value is the fix; the submission cost above is.
