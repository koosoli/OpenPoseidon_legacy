# PERF-014 — `land:obj` on Chernarus+ is 2,450 scalar `Shape::Draw` calls, and nothing batches

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Follows:** WLD-025, which measured `land:obj` at 38–49% of the CPU frame on Chernarus+ and
said plainly that it did not explain why. This explains why.
**World:** `packages/dayz-compat/world/chernarusplus/chernarusplus.wrp`, DayZ addons,
pose `5704.58 2523.37` alt 120 az 80.9, hour 12, `perf_combat` mission, 1280x720, DLSS off.

## What `land:obj` covers — checked, not assumed

`land:obj` is `Scene::DrawObjectsAndShadowsPass1()` and one `EnableReorderQueues(true)` call.
The two marks that bound it are 4 lines apart (`LandscapeRender.cpp:1689` and `:1693`) and
the phase name is honest. PERF-013 found a phase whose mark sat hundreds of lines from what
its name implied; this one does not have that problem, and the check cost one file read.

## The subdivision

Two levels. The six-way split of Pass1 already existed (`WGR_PASS1_STATS`); the split of
`submit` — which is 95% of Pass1 and named no line of code — is new here and spans three
translation units, because the work does (`SceneDraw.cpp`, `Object.cpp`, `ShapeDraw.cpp`).

All figures are **windowed deltas between the 512- and 1024-frame reports**, not the logged
cumulative averages. The logged averages run from frame 0 and include the streamer fill,
during which the resident set is a third of its settled size; reading them as steady state
understates every bucket and the object count with it.

`WGR_RENDER_THREAD` pinned explicitly in both arms — it was flipped to on-by-default
mid-session, so an unpinned run is not comparable to either.

| bucket | rt=0 run a | rt=0 run b | rt=1 run a | rt=1 run b | what it is |
|---|---:|---:|---:|---:|---|
| **Pass1 total** | 12.833 | 13.446 | 14.601 | 15.039 | = `land:obj` |
| the other five, together | 0.611 | 0.628 | 0.632 | 0.645 | compact, complexity, mergers, occlusion, sort |
| submit | 12.222 | 12.818 | 13.969 | 14.394 | 95.2% of Pass1 |
| — scan | 0.234 | 0.247 | 0.215 | 0.238 | run-formation walk |
| — animate | 0.107 | 0.109 | 0.107 | 0.111 | Animate + Deanimate |
| — clipfog | 0.095 | 0.098 | 0.099 | 0.099 | bsphere clip + fog constant |
| — lights | 0.048 | 0.047 | 0.051 | 0.050 | SelectLights |
| — proxies | 1.334 | 1.271 | 1.458 | 1.513 | DrawProxies + all recursion |
| — textures | 0.233 | 0.240 | 0.226 | 0.233 | PrepareTextures |
| — **shapeDraw** | **9.636** | **10.255** | **11.263** | **11.575** | **`Shape::Draw`** |
| shapeDraw as % of Pass1 | 75.1% | 76.3% | 77.1% | 77.0% | |

Denominators, same window:

| count/frame | rt=0 a | rt=0 b | rt=1 a | rt=1 b |
|---|---:|---:|---:|---:|
| mergers walked | 2,754 | 2,754 | 2,753 | 2,752 |
| objects skipped by the loop | ~250 | ~250 | ~250 | ~250 |
| **top-level `Object::Draw` bodies** | **2,450** | **2,450** | **2,447** | **2,448** |
| nested (proxy) draws | 140 | 127 | 116 | 121 |
| sections walked | 5,347 | 5,734 | 6,060 | 6,060 |
| **instanced runs formed** | **0** | **0** | **0** | **0** |

## Which part dominates, and why

**`Shape::Draw` is 75–77% of `land:obj`**, at **~4.0 us per object** and **~1.8 us per
section**. Everything else in the phase, added together — the compaction, the complexity
governor, the merger rebuild, the sort, the scan, the animation, the clipping, the fog, the
light selection, the texture prepare and the entire proxy recursion including everything it
recurses into — is under 20%.

The shape is not "too many items". 2,450 objects and ~5,700 sections is a modest frame; a
section queued into a draw list should not cost 1.8 us. It is a **fixed per-object and
per-section submission cost paid 2,450 times**, and only ~2.3 sections come out of each
object, so the per-object overhead is amortised across almost nothing.

**Nothing is batched, and it structurally cannot be.** `runs=0` in all six runs, and there are
two independent, each-sufficient reasons:

1. **Measured:** the scene-wide local-light veto rejects **100%** of candidate heads —
   `vetoLights=1965` against `heads=1965`. `noLocalLights` is `NLights() == 0`, and
   `perf_combat` has local lights, so one condition disables batching for every object in
   the frame. By contrast only **1** object per frame fails `Static()` and **27** carry
   proxies. The object set is not the problem.
2. **By inspection:** `EngineWgpu` overrides **none** of `InstancedRunReset` /
   `InstancedRunAdd` / `BeginInstancedRunUpload` / `EndInstancedRun`. The base
   `Engine::InstancedRunAdd` returns `false`, which keeps a run at length 1, so `runLen >= 4`
   can never hold on the default backend regardless of the veto. The CPU instancing path is
   live only on GL33.

So the batching code in the submission loop is dead on the backend everyone runs, and would
still be dead on this mission if the backend implemented it.

## What that is worth

Measured, not estimated. A predicate scan over the shape-sorted list, run beside the
submission loop and touching no engine state, counts the runs the frame **actually contains**,
using the batcher's own clauses minus the light veto and minus the backend refusal:

```
wouldRuns=135  wouldObjs=1432  ofObjDraws=72.9%  meanRunLen=10.6  maxRunLen=90
wouldRuns=136  wouldObjs=1437  ofObjDraws=72.9%  meanRunLen=10.6  maxRunLen=90
```

**72.9% of drawn objects sit in a run of at least 4 identical adjacent draws**, averaging 10.6
and reaching 90. The geometry offers the batching; two separate pieces of code decline it.

That is the size of the prize and **not** a claim that it is all recoverable — instanced
submission still pays per section, and none of it is landed or tried here. Deliberately not
attempted in this pass: the measurement that justifies the work should not arrive in the same
commit as the work.

The cheap end of it is genuinely cheap and the expensive end is not:

- **Lifting the light veto alone buys nothing on wgpu** — reason 2 still holds. Anyone
  tempted by the one-line fix should read that first; it would measure as "no effect" and be
  misread as "batching does not help".
- **The real question is whether this belongs in CPU instancing at all.** The GPU-driven path
  already diverts 22.0% of objects (`coverage full=852` against `partial=216 none=2791`), and
  those cost this phase nothing. Widening that coverage and implementing CPU instanced runs
  are two routes to the same 72.9%, and this document does not choose between them.

## Spread, and what these numbers do not say

Run-to-run spread within an arm is **4.8%** on Pass1 (12.833 / 13.446) and **6.4%** on
`shapeDraw` — far tighter than the 21.7-vs-15.1 ms this harness has produced before, because
`land:obj` is a phase rather than a whole frame. The rt=0 and rt=1 arms differ by ~14% on
Pass1; that is the render-thread flip changing main-thread scheduling, not a change in the
work, and the shapeDraw **share** is stable at 75–77% across all four.

- The instrumentation costs what it measures. ~12 clock reads per drawn object inflate the
  totals it reports; read the **shares** and the **counts**, and cross-check against `submit`
  from the same run. It is off unless `WGR_PASS1_STATS` is set, and the batchability census
  sits outside the `submit` timer so it inflates no reported bucket.
- Nanoseconds, not microseconds like `Pass1Stats`: several of these blocks are under 1 us per
  object and microsecond truncation would have reported real work as free.
- One machine, one pose, one mission. `land:obj` is 2–3 ms on the stock CWA islands; nothing
  here re-measures that.
- Not deployed. The brief for this pass forbade `Deploy.ps1`, and the change is inert without
  the env var, so the owner's binary behaves identically — but the owner's game folder is
  still on `3fa5dc97` and does not contain this instrumentation.
