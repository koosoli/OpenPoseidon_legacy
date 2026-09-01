# PERF-016 — What Arma 3 2.20's OPREP transfers to this engine, and what it does not

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Source:** Bohemia Interactive, *OPREP — Performance optimizations in 2.20*, dev.arma3.com
**Follows:** PERF-012 (overlap ceilings per scene), PERF-015 (inside `AIGroup::Think`), REN-THR-016
(overlap landed and proven frame-equivalent)

The owner brought this article and an analysis of it. The analysis is largely right, and the
parts of it that are right are worth writing down. But it inherits one assumption that this
project has already measured its way out of, and following it in the order proposed would spend
a job system on work that should not be happening at all.

## What transfers, and why

**The article confirms the shape of what just landed.** Bohemia describe rendering frame N in
parallel with simulation of N+1. REN-THR-016 does exactly that, and proves it frame-equivalent:
901 of 901 scene frames identical to the inline path, aligned by publication tick. So this is
not a claim we are copying — it is one we can already check.

**Their AI caveat is the useful half.** They say plainly that Arma AI does not parallelise by
handing each group a thread: FSMs, scripts and shared world-state writes collide. What *is*
separable is target search and parts of pathfinding preparation. That matches what PERF-015
found here from the other direction, by measurement rather than by architecture.

**Their headline metric is the right one, and it is not ours yet.** Bohemia's goal was fewer
spikes and higher minimum framerate, not higher mean. Average framerate rose relatively little.
Every performance number this project has published is a mean, and PERF-012 already notes that
`perf_combat`'s ring mean (49 ms) and its cheapest steady frame (13.9 ms) are a factor of three
apart — which is a spike distribution described as an average. **1% and 0.1% lows belong in the
bench**, and we have a spike instrument nobody has used as one: `FixedStepAccumulator`'s
discarded-step counter (PERF-013) fires precisely on the frames that overrun.

## The correction: rationing comes before parallelism

The proposed order is *build a dependency job system, then move AI target/visibility scanning
onto it as read-only work.* Two things are wrong with that here.

**First, the expensive scan is not read-only.** PERF-015 §4 puts the near-enemy tracking loop at
`AIGroup.cpp:1315–1342`, O(engaged units × targets), and it *writes* — that is what it is for.
It is not a pure computation waiting for a worker; making it one is its own piece of work, and it
is the piece the article warns about rather than the piece it recommends.

**Second, and this is the part that changes the plan: the work is not rationed, and the ration
already exists.** `TrackNearTargets` ends by writing `_trackNearTargetsTime = Glob.time` and
**nothing ever reads it**. Its sibling `_trackTargetsTime` *is* read and does gate its loop. So
the throttle was written, wired up on one side only, and has been inert ever since. Measured on
`perf_combat`: `track` is 2.231 ms of a 2.753 ms AI stage — **68%** — at 457 targets per call and
14,225 pairs per tick.

Reading that timer would remove most of those pairs outright. Moving them to a worker pool
instead keeps every pair and buys back only the wall time of one core. **Deleting work beats
distributing it**, and the deletion is a one-line gate that is already half-implemented.

This is not an argument against the job system. It is an argument about which comes first: fix
the ration, re-measure the AI stage, and let what is *left* decide whether a job system is worth
building. If `track` drops to 0.3 ms, the AI stage stops being the largest cost and the whole
question moves elsewhere.

**Why it is still unfixed:** turning a ration on changes how often the AI re-evaluates targets,
which is behaviour, not performance. It needs the `StateTimeline` oracle and behavioural tests
first. That gate is the actual next task, not the job system.

## Where a job system would and would not pay

PERF-012 measured the overlap ceiling per scene, and the same reasoning bounds any CPU-side
parallelism:

| scene | CPU frame | swap | ceiling on CPU-side work |
|---|---:|---:|---:|
| `perf_abel` | 11.79 ms | 10.74 ms (91%) | **1.3%** |
| `perf_town` | 6.06 ms | 4.93 ms (81%) | **7.6%** |
| `perf_combat` | 49–57 ms | 27 ms (48–55%) | **34–42%** |

Three of four bench scenes are GPU-bound, and `perf_abel` — the scene every historical figure in
this project was taken on — would gain nothing measurable from any of this. A job system is a
`perf_combat`-shaped change. That is a reason to build it against `perf_combat`, and a reason not
to report it on `perf_abel`.

## What goes into the roadmap

As a phase *after* the AI ration work, not instead of it:

1. **Ration `TrackNearTargets`** (behavioural gate first). Re-measure the AI stage.
2. **Add 1% / 0.1% lows and discarded-step counts to the bench**, and stop quoting means alone.
3. **Then** decide on a dependency job system, scoped by what step 1 leaves behind. If it is
   built: Taskflow or enkiTS, which Bohemia name as the alternatives they evaluated — not a
   reimplementation of Enfusion's, which we do not have and cannot borrow.

## What this does not say

- It does not evaluate Taskflow or enkiTS. Neither has been tried here.
- It does not claim the tracking ration is safe to enable. Nobody has run the behavioural tests
  that would say so; that is the whole reason it is still off.
- The 68% figure is one scene, one machine, and PERF-015's own caveats apply — in particular
  that `perf_combat` is the only perf mission with meaningful AI load.
- Bohemia's numbers are theirs, on their engine. Nothing here reproduces them.
