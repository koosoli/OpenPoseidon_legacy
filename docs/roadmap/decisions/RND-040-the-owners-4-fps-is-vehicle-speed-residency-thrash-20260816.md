# RND-040 — The owner's 4 fps is real, is vehicle-speed only, and the harness could never have seen it

**Date:** 2026-08-16
**Branch:** `feat/texture-streaming`
**Status:** **Measured and reproduced.** Mechanism confirmed against code. **Not fixed.**
**Binary:** all figures from the 2026-08-15 23:36 build. No engine change was needed to measure it.
**Related:** [RND-038](RND-038-four-worlds-four-bottlenecks-at-play-resolution-20260815.md), owner feedback §1

## The claim that started it

Every Reforger figure in the previous handover came from a **fixed** freefly pose settled to
`pending=0`, and reported 34-40 fps. The owner, who plays by **moving**, reported **~4 fps** and
was told numbers that did not describe any state he occupies.

**He was right, the measurements were right, and they were measuring different things.**

## The numbers — Everon, 1600×900

Static and moving come from the **same process** and the same fill-in, so no arm is confounded by
a different warm-up.

| arm | frames | median | p95 | p99 | worst | throughput | median *second* |
|---|---:|---:|---:|---:|---:|---:|---:|
| static (in 30 m/s run) | 1806 | 25.0 ms | 27.0 | 28.0 | 232 ms | **40.3 fps** | 40 frames |
| static (in 5 m/s run) | 1793 | 25.0 ms | 27.0 | 29.0 | 42 ms | **40.0 fps** | 40 frames |
| **moving 5 m/s (walk)** | 5527 | 20.0 ms | 26.0 | 28.0 | 435 ms | **46.6 fps** | 49 frames |
| **moving 30 m/s (vehicle)** | 1334 | 24.0 ms | 335 | 643 | 1555 ms | **11.9 fps** | **4 frames** |
| 30 m/s, driver artefact excluded | 1071 | 24.0 ms | 310 | 561 | 1047 ms | **13.1 fps** | **4 frames** |

**The median frame time barely moves.** 25.0 ms static, 24.0 ms at 30 m/s. All of the damage is in
the tail: at 30 m/s, **74% of wall-clock time is spent inside frames slower than 100 ms**, and the
median *second* delivers **4 frames**.

That is not an approximation of the owner's report. It is his number.

## The split is the finding

- **Vehicle/aircraft speed: catastrophic.** 13.1 fps throughput, 66% of seconds at ≤5 fps.
- **Walking speed: fine.** 46.6 fps — *faster than its own static phase*, because motion keeps the
  near set churning rather than holding a maximal resident set in view.

So "the streaming transient is the whole experience" is **half right**. It is the entire experience
in a vehicle and nearly invisible on foot. **If the owner is walking and still sees 4 fps, this is
not his bug and something else is wrong** — that is the one question this record cannot close.

## Why every previous measurement missed it

This is the part worth remembering.

The capture JSON written **during the 30 m/s moving phase** reports `avg_fps 35.06`, frame avg
28.5 ms, GPU frame total 26.75 ms — **indistinguishable from static**. A capture samples the
median, and the median is healthy while the frame budget is being destroyed by a tail.

**Adding camera motion to the existing harness would still have reported ~35 fps.** The hole was
never only "the camera does not move" — it was that a mean or a single-frame sample cannot see a
bimodal distribution. Any future performance claim on this engine needs a distribution, not a mean.

## Mechanism, confirmed

Predicted from code before the runs, and the prediction was right in a falsifiable detail: **spike
size is constant and only frequency scales with speed.** 21 recentres in 112 s at 30 m/s versus
**2** in 119 s at 5 m/s, with spikes ~230-270 ms in both.

The chain:

1. `Landscape::UpdateModernObjectResidency` (`LandSave.cpp:2525`) quantises the window centre to
   `QuantizeCells = 4` and early-outs unless it moves. Residency therefore does nothing until the
   camera crosses a boundary, then does a bulk rebuild — a periodic spike, not a trickle.
2. The resident set is **at its cap** (19959 of 20000) even when stationary, so every recentre must
   **evict before it can admit**.
3. There is no cache. The file says so: *"(there is no cache), so what the camera left behind must
   be rebuilt"*. Measured `created=15958 / released=77643` at 30 m/s against `307 / 10895` at
   5 m/s — destructive eviction, confirmed.
4. Admission cannot stop: `admitBudgetSpent()` returns false until the floor of 128 has been
   *attempted*, so the 4 ms budget is unreachable. The file's own comment: *"580 ms to admit the
   mandatory 128 — 145× over a 4 ms budget — which is why frames sit"*.

### The correlation, and the part that would have been missed

Frame cost by time since the last logged recentre, at 30 m/s:

| since recentre | frames | median | worst | wall |
|---|---:|---:|---:|---:|
| 0–0.5 s | 37 | **239 ms** | 697 | 9.3 s |
| 0.5–1 s | 27 | **252 ms** | 543 | 7.0 s |
| 1–2 s | 54 | **255 ms** | 662 | 11.8 s |
| 2–4 s | 154 | 24 ms | 1555 | 23.1 s |
| 4 s+ | 1062 | 23 ms | 1363 | 61.0 s |

**The recentre log line marks the start of a stall train, not the stall.** 118 frames hold ~250 ms
for a full two seconds afterwards — 28 s of a 112 s window. Only ~33 frames sit *on* a logged
recentre, so the obvious correlation (worst frames land on the log line) accounts for barely a
tenth of the lost time and would have **understated** the mechanism badly.

## What follows for the fix list

The handover's `WGR_OBJECT_STREAM_MIN_BATCH=1` attacks the admission floor. That is the third of
four problems, and on its own it cannot work:

1. **Admission is synchronous.** ODOL decode, LOD conversion, GPU registration and texture
   transcode all happen on the frame. This is the real fix and it is the large one.
2. **Eviction is destructive.** No cache, so backtracking — the single most common camera motion in
   play — is worst-case. A modest LRU of recently-evicted objects would cut the 15,958 creates
   sharply, and it is far cheaper than async loading.
3. **The floor is mandatory** before the time budget is consulted.
4. **The window is reactive.** It recentres only after the camera has *already* crossed, so every
   admit is on the critical path. Prefetching along the velocity vector is what converts a stall
   into background work, and it is the only item here that specifically targets *speed*.

Note that (4) is invisible to a static harness by construction, which is why it has never been on
the list.

## Caveats

- The 30 m/s arm ran with a bug in the measuring driver: `camCommit` over exactly one waypoint
  interval divides by ~0 on a slow frame, and the camera teleported twice, dragging residency to
  the map corner. Excluding those windows moves throughput 11.9 → 13.1 fps and worst 1555 →
  1047 ms; **median and every conclusion are unchanged**. Fixed via `-WaypointLead` (default 4).
  A clean 30 m/s re-run is the one loose end, and it must now be labelled against the new binary.
- Mission time ran at 0.85× wall during the 30 m/s traverse, so that arm covered ~2.9 km rather
  than the commanded 4.5 km. All quoted figures are wall-clock.
- The speed-0 control was killed for a build slot and is **discarded, not quoted**. It is
  redundant: the 5 m/s arm runs the identical phase structure and waypoint-posting code and
  measures *faster* than its own static phase, which is what that control existed to prove.
- Everon only. Chernarus fills for ~253 s and is texture-VRAM bound; the same measurement there
  could look quite different.
