# PACE-001 — Community stutter at a high fps cap (Wetzer's report), measured

**Date:** 2026-09-12. **Branch:** `perf/frame-pacing` (from trunk tip `5bd4f8f6`).
**Status:** mechanism measured on this machine; REN-INTERP-001 built and deployed on the
branch; its acceptance A/B is recorded in "REN-INTERP-001 measurements" below (nothing above
that section claims it passed). NOT confirmed on the tester's machine; NOT on trunk.

## The report

Tester Wetzer: Ryzen 9 5950X, RTX 3080 Ti, 1440p, 120 Hz monitor. In-game cap 120, VSync
off, windowed, "native" resolution, MSAA 4x, no upscaler, Ultra tiers, shadows off,
particles/clouds high.

* cap 120: visible stutter, very uneven frame-time curve; one screenshot shows ~47 fps at
  95 % GPU load with the overlay reading 8.3 ms.
* cap 30: smooth, even frame-time curve, ~60 % GPU load.
* FSR 1 Quality: ~59-60 fps, smooth, ~51 % GPU load.
* DLSS "does not work" (nature of the failure unknown).

Builds: the tester first named the 2026-08-22 build and later a newer Discord build. Which
screenshot came from which build is NOT known. The screenshots are not a controlled A/B.

## What was installed and measured here

| | |
|---|---|
| Machine | i7-11700F, RTX 3070 (driver 32.0.15.9579), 3440x1440 @ 144 Hz, Windows 11 26200 |
| Installed before this session | `asset-obs-001 61bbb0c6` (DLSS build, `nvngx_dlss.dll` present) |
| Measured binary | `perf/frame-pacing f9234310` — trunk tip `5bd4f8f6` + the trace commit, deployed via `Deploy.ps1`, stamp in every run's output |
| Scene | `tests/perf/missions/perf_field.eden` (Everon field), scripted camera at eye level: 30 s static, then 60 s traverse at 5 m/s (`scripts/pace-series.ps1` → `farfield-movecam.ps1`) |
| Settings | scratch user dir (`POSEIDON_USER_DIR`) with the owner's `graphics.cfg` pinned to vsync=0, msaaSamples=4, dlssMode=0, shadowQuality=0; window 2560x1440 via `--window --width --height`; cap via `POSEIDON_FPS_CAP`; VSync via `POSEIDON_VSYNC=0` |

This is NOT the tester's machine. The 3070 at 2560x1440 MSAA 4 on this scene lands in the same
regime as his report (GPU frame ~21 ms, ~46 fps at cap 120), which is why it is a usable
stand-in for the mechanism, not a confirmation of his case.

## Code archaeology: what each build had

| mechanism | 2026-08-22 build (`52dfbceb`) | current trunk |
|---|---|---|
| fps cap | `Sleep(1000/cap - elapsedMs)` in whole ms, no `timeBeginPeriod` anywhere in the tree | `PaceToFps`: microsecond period, high-resolution waitable timer (`SleepUs`), `timeBeginPeriod(1)` while a cap is active (since `5f7c2bda` 08-24, `c274badc` 09-01) |
| fixed-step simulation | none (variable deltaT per frame) | 1/60 s ticks, merged 08-28, no render interpolation |
| render thread / overlap | none | on by default since `5a8c07ea` 09-01; producer may run one frame ahead |
| present | wgpu `Immediate` for VSync off | same |

So the three hypotheses split by build:

* **A (limiter)** can only explain the 08-22 build's stutter. In that build `Sleep(n)` for n
  in 1..8 ms takes 15.3-16.7 ms on this Windows 11 box when nothing in the process has
  raised the timer resolution (measured with a standalone probe, 60 samples each: `Sleep(1)`
  median 15.7 ms, `Sleep(8)` median 15.3 ms; with `timeBeginPeriod(1)`: 1.5 / 8.7 ms; the
  current high-resolution timer: 1.6 / 8.35 ms). At cap 120 that pacer computed an 8 ms
  budget and padded every frame that finished early by ~15.6 ms, and padded nothing when the
  frame was late — a bimodal ~8 / ~24 ms stream. The current pacer does not have this
  defect: in the trace its sleep overshoot is 0.25-0.33 ms median.
* **B (render thread / GPU back-pressure)** — under the current build at cap 120 GPU-bound
  the main-thread period is EVEN (below). The back-pressure lands in the worker's
  `get_current_texture` (acquire median 16-17 ms, blocked on 100 % of frames), which the
  producer sees as its slot wait (median 18 ms). There is no burst/stall pattern and the
  limiter sleeps on 1 % of frames. On the CPU/GPU side B is not a cause on the current
  build; whether the presentation path shows those frames evenly was not measured (no
  PresentMon here) and stays open.
* **C (fixed-step motion judder)** — cannot have caused anything on the 08-22 build (no
  fixed step there), but it IS present on the current build and matches the tester's
  30-smooth / 60-smooth / 47-stutters pattern exactly: 47 fps is 1.28 ticks per frame, so
  consecutive frames advance the world by 16.7 or 33.3 ms of simulation time while the
  screen shows them 21 ms apart. At cap 30 every frame is exactly 2 ticks; at ~60 fps every
  frame is 1 tick; both are smooth. Measured on the shakedown (cap 120, 46 fps): ticks per
  frame 1.29, histogram {1: 965, 2: 392}, and the tick count CHANGES between 58-63 % of
  consecutive frames.

## Trace, series, numbers

Instrumentation: `POSEIDON_FRAME_TRACE=<prefix>` (commit `f9234310`) — per frame on the
main thread (start/end, sim deltaT, ticks, alpha, profiler phases, pacer decision and actual
sleep), on the producer (InitDraw wait, lazy waits, slot wait, lockstep wait) and on the
worker (acquire, submit, present, GPU total). `scripts/pace-analyse.py` cuts a run by the
movecam log's phase frame numbers and reports period median/p95/p99, frame-to-frame jitter
(mean |Δperiod| / median), alternation, pacer stats, waits, ticks-per-frame, and — from the
second trace commit on — camera displacement per frame (the motion the player sees).

RESULTS TABLE: see the section "Series A" appended below once the runs finished.

## DLSS on the tester's machine

The two archives that were handed out for testing (`OpenPoseidon DLSS BUILD ready for testing
-92716b96.7z`, `OpenPoseidon DLSS-standardonat67procent.7z`, both 2026-08-30) contain ONLY
`OpenPoseidon.exe` and `wgpu_renderer.dll`. The NGX snippet `nvngx_dlss.dll` is not in them.
`Deploy.ps1` copies it next to the exe on the owner's machine (from `.tmp-dlss-sdk`), which is
why DLSS works here and in every local capture. Without it NGX init fails and the engine
reports `nvngx_dlss.dll not found next to the executable (...); copy it beside OpenPoseidon.exe`
(`dlss_status.rs`) and falls back to native — which is exactly "DLSS does not work" from the
outside. This is the most likely cause of the tester's DLSS failure; it is not proven because
his log was never seen. What to ask him for: the `DLSS status:` line from his log, and whether
`nvngx_dlss.dll` sits next to his exe. Any future test archive must include the snippet (and
the release-note line: DLSS needs Vulkan + an RTX GPU + that DLL).

MSAA 4 is not a DLSS gate in this code (the DLSS route is decided by build feature, WGR_DLSS,
adapter backend/vendor, NGX init, and the temporal path being on).

## Display cadence, for the write-up to the tester

On a fixed-refresh 120 Hz display with VSync off in windowed mode, frames are composited by
DWM at 120 Hz: a 47 fps stream lands on alternating 2- and 3-refresh intervals (16.7 / 25 ms)
whatever the engine does. Caps that divide the refresh rate (60, 40, 30 on 120 Hz) give an
even display cadence when the GPU can sustain them; 47 fps cannot be shown evenly on that
monitor without VRR. This is independent of the engine-side judder above and is worth one
sentence in the reply to him.

## The fix: REN-INTERP-001 render interpolation

Moving entities (the world's `_vehicles`, `_animals`, `_fastVehicles` lists) remember their
transform before EACH fixed-step tick (`Object::CaptureRenderPrev`, called from the tick loop
in `World::Simulate`). The draw traversal (`SceneDraw.cpp` DrawSortObject and DrawExShadow)
draws each such object at `prev + alpha * (cur - prev)` where alpha is the accumulator's
leftover fraction published after the tick loop (`Object::SetRenderInterpAlpha`), and the
camera attached to a vehicle takes the same interpolated frame (`World.cpp`,
`cameraVehicle->RenderTransform()`), so the camera never sits on a different time than the
body it rides. Static objects are never captured and draw at their own frame at the cost of
one branch. Nothing is written back into the simulation; AI, physics, network, saves and
scripts see only tick state. Teleports/spawns/setPos snap (jump guard: 40 m per tick). A
paused world draws its current state (alpha forced to 1). `POSEIDON_RENDER_INTERP=0` turns it
off.

Cost: the displayed state lags the newest tick by (1 - alpha) ticks, at most 16.7 ms, on
average ~8 ms. That is the standard price of interpolation and it is documented here rather
than hidden; extrapolation would avoid it at the cost of overshoot on every collision.

Not covered by this slice (documented, measurable later): skeletal animation phase (advances
per tick; limbs still step), cloudlet particles, vehicle-attached lights (headlights read the
tick transform), sounds (positions read tick state — inaudible at these deltas).

Acceptance metric: camera displacement per frame in the trace (`cam_dx_m`) on a vehicle-ride
mission with the player in the cargo of an AI-driven jeep (`.tmp-pace/missions/ride.Intro`):
relative jitter of dx per frame before vs after, at caps where ticks per frame is fractional
(40, 50, 90 as well as 120).

RESULTS: see "REN-INTERP-001 measurements" appended below.

## Series A — perf_field, 2560x1440, MSAA 4, VSync off, upscaler off, shadows off, binary `f9234310`

| cap | phase | fps | period med / p95 / p99 ms | jitter | within +-10 % | frames slept | ticks/frame | tick-count changes | acquire med ms | GPU med ms |
|---|---|---|---|---|---|---|---|---|---|---|
| 30 | static | 29.6 | 33.7 / 34.0 / 34.1 | 1 % | 100 % | 100 % | 2.03 | 6 % | 0.0 | 20.9 |
| 30 | moving | 29.6 | 33.7 / 34.0 / 34.2 | 1 % | 100 % | 100 % | 2.03 | 6 % | 0.0 | 20.0 |
| 60 | static | 44.4 | 22.3 / 25.9 / 36.0 | 11 % | 78 % | 7 % | 1.35 | 66 % | 17.1 | 22.3 |
| 60 | moving | 47.6 | 21.0 / 23.4 / 30.1 | 7 % | 80 % | 8 % | 1.26 | 51 % | 16.2 | 21.0 |
| 120 | static | 46.4 | 21.5 / 23.4 / 24.8 | 5 % | 93 % | 0 % | 1.29 | 58 % | 17.3 | 21.4 |
| 120 | moving | 48.5 | 20.6 / 23.1 / 25.7 | 7 % | 83 % | 1 % | 1.24 | 48 % | 15.6 | 20.5 |
| none | static | 44.8 | 22.3 / 24.5 / 25.8 | 5 % | 91 % | 0 % | 1.34 | 68 % | 17.4 | 22.3 |
| none | moving | 46.6 | 21.6 / 24.4 / 26.2 | 7 % | 77 % | 0 % | 1.29 | 58 % | 16.7 | 21.4 |

Reading it:

* **The CPU-side frame period is even at every cap** (display timing was not captured, see
  "Measurement hygiene"). Jitter (mean frame-to-frame change of the period)
  is 1 % at cap 30 and 5-11 % GPU-bound; there is no short/long alternation (alternation
  index 0.14-0.25, where a strict 8/34 ms stream would read ~1.0). The limiter is not the
  bottleneck when the GPU is: it slept on 0-8 % of frames at caps 60/120 and never
  uncapped. The one 11 % row (cap 60 static) overlapped a compile I started by mistake and
  aborted; its p99 of 36 ms is that, not the game.
* **The GPU is the clock.** GPU frame total 20-22 ms in every GPU-bound arm; the worker
  blocks 15.6-17.5 ms per frame in `get_current_texture` waiting for a swapchain image,
  and that wait reaches the main thread as the producer's slot wait. Cap 30 removes the
  back-pressure entirely (acquire 0.04 ms, GPU ~62 % busy = 21/33.7 — the tester's "~60 %
  GPU at cap 30").
* **The cap does not reach its number:** cap 30 delivers 29.6 fps (33.7 ms instead of 33.3).
  The pacer sleeps from the frame's own start to `period`, then the inter-frame work (input,
  events) adds ~0.4 ms on top. Small, and a deadline-based pacer (next deadline = previous
  deadline + period) would remove it; noted, not changed here.
* **The motion judder is in the tick column.** GPU-bound at 44-48 fps the world advances
  1.24-1.35 ticks per frame and the tick count changes between 48-68 % of consecutive
  frames: every other frame or so shows twice the motion of its neighbour while the frames
  themselves are 21 ms apart. At cap 30 it is 2.03 ticks per frame with 6 % changes: smooth.
  This is what the tester describes, on the build that has a fixed step.

## Measurement hygiene (checked after the review)

* **Auto-screenshot stalls are outside every measured window.** Series A: the screenshot
  frame (5614 / 8827 / 9031 / 8843) came after the last moving-phase frame (4355 / 6740 /
  6898 / 6698) in all four arms. `pace-analyse.py` now warns when a screenshot frame falls
  inside a phase.
* **Present/display timing was NOT captured.** PresentMon is not installed on this
  machine (searched PATH, Program Files, the user profile) and installing software was
  out of scope. The trace ends at `frame.present()` returning (0.08 ms median under
  Immediate — the call does not wait). What the display did with those frames is
  therefore inferred, not measured: with windowed Immediate the DWM samples the latest
  frame at each refresh, and an even 21 ms CPU/GPU cadence on a 144 Hz (here) or 120 Hz
  (tester) panel maps onto 3/3/3/4-refresh or 2/3/2/3-refresh intervals respectively.
  "Even CPU period" is thus a necessary, not a sufficient, condition for even display
  output. This stays open until someone runs PresentMon alongside the trace.
* **Acquire blocking locates the back-pressure; it does not by itself prove a pure GPU
  compute limit.** The GPU timestamp total (20-22 ms per frame, `WGR_GPU_TIMER_FRAME_TOTAL`,
  i.e. the submitted work) matches the frame period within a millisecond in every
  GPU-bound arm, which is the direct evidence that the GPU work itself fills the frame;
  a presentation-path stall would show as acquire time NOT covered by GPU work.
* **The first ride A/B (11:17) measured nothing:** the AI driver never moved (doMove target
  off the Intro island; `speed ffvJeep` = 0 for the whole "moving" phase). The mission now
  shuttles along the lane the dev mission uses and logs the jeep speed on every FT line;
  the analyser reports camera translation AND turn (dyaw) per frame. Those runs are
  discarded.

## Separate verdicts (each stands or falls on its own evidence)

1. **Motion smoothing (REN-INTERP-001):** verdict pending the ride A/B with the fixed
   mission — see "REN-INTERP-001 measurements". The 1/2-tick alternation itself is
   expected to REMAIN after the fix (it is how a 60 Hz simulation meets a 47 fps frame);
   the acceptance number is the rendered camera's per-frame displacement and turn jitter,
   not the tick histogram.
2. **Frame pacing:** the current limiter is not a cause of uneven frame delivery
   (Series A). No pacing change was made; the 0.4 ms cap overshoot is noted, not fixed.
3. **Community case reproduced and fixed:** NOT claimed. The mechanism reproduced here on
   another machine/scene matches the report's pattern on the fixed-step build; the 08-22
   build's cause is a different, already-removed defect; the tester's re-stutter at an
   unchanged 30 fps cap under higher GPU load (from his video) is a separate acceptance
   point — the interpolation would cover it if the GPU frame exceeds 33 ms (2.4 ticks per
   frame at 25 fps), but that case was not run here.
4. **Additional fps gain:** none claimed, none measured. Interpolation costs a copy of
   one matrix per moving entity per tick and a lerp per drawn moving object.

## Cost of the slice

`_renderPrev` (a `Matrix4`, 64 bytes) plus a flag live in every `Object`, moving or not, so
the flag check in the draw path costs no lookup. On stock Everon (82,381 registered objects,
PHY-010 census) that is ~5.5 MB of process memory, ~8 MB with padding, for a process that
holds several GB of geometry and textures; a side table keyed by entity would save it at the
price of a hash probe per drawn object, which the `_gpuCoverage` history in Object.hpp
records as measured frame cost. Not measured as a frame-time change here; the per-tick
capture is a 64-byte copy per moving entity (hundreds, not tens of thousands).

## REN-INTERP-001 measurements — ride A/B, same build `64f08cfc`, 2026-09-12 11:37-11:52

`tests/perf/missions/pace_ride.Intro` (Intro island, light scene): the player in the cargo of an
AI-driven jeep shuttling a 240 m lane with a U-turn at each end (speed median 45-47 km/h),
20 s static, 40 s inside view, then `switchCamera "EXTERNAL"` for 40 s (first 3 s after the
switch trimmed). Same scratch settings as Series A. Interpolation switched by
`POSEIDON_RENDER_INTERP` only. Artefacts: `.tmp-movecam/out/ride{OFF,ON}-cap{120,50}/`
(run-01.log, *.trace.{main,producer,worker}.csv, *.pace.json, *.pace.png).

| cap | view | fps off / on | cam dx per frame med / p95 m, off -> on | dx jitter off -> on | dyaw jitter off -> on | ticks/frame off / on | tick-count changes off / on |
|---|---|---|---|---|---|---|---|
| 120 | inside (passenger) | 46.3 / 47.3 | 0.22 / 0.44 -> 0.27 / 0.32 | **51 % -> 6 %** | 81 % -> 46 % | 1.29 / 1.27 | 58 % / 54 % |
| 120 | external | 70.4 / 72.7 | 0.19 / 0.23 -> 0.20 / 0.23 | **36 % -> 33 %** | 156 % -> 159 % | 0.85 / 0.83 | 39 % / 39 % |
| 50 | inside (passenger) | 46.7 / 46.7 | 0.22 / 0.44 -> 0.26 / 0.32 | **49 % -> 5 %** | 86 % -> 49 % | 1.29 / 1.28 | 56 % / 56 % |
| 50 | external | 48.9 / 48.9 | 0.21 / 0.44 -> 0.21 / 0.44 | **38 % -> 37 %** | 153 % -> 168 % | 1.23 / 1.23 | 45 % / 45 % |

## REN-INTERP-001 measurements — re-run on build `3f64710a` (RenderDelta through the hierarchy), 11:58-12:06

Same mission, settings and harness; artefacts in `.tmp-movecam/out/ride4{OFF,ON}-cap50/`.
Unit tests: `PoseidonTests "[interp]"` — 4 cases, 29 assertions, green.

| cap | view | fps off / on | cam dx per frame med / p95 m, off -> on | dx jitter off -> on | dyaw jitter off -> on | ticks/frame off / on |
|---|---|---|---|---|---|---|
| 50 | inside (passenger) | 48.1 / 46.3 | 0.21 / 0.44 -> 0.27 / 0.31 | **43 % -> 6 %** | **83 % -> 48 %** | 1.25 / 1.30 |
| 50 | external | 49.0 / 48.9 | 0.21 / 0.43 -> 0.25 / 0.28 | **38 % -> 3 %** | **152 % -> 33 %** | 1.22 / 1.23 |

**Motion smoothing verdict: PASSED on this machine** for the passenger's inside and outside
views, straight legs and U-turns, at a cap where frames are a fractional number of ticks.
Both views now show one band of per-frame displacement instead of two. Frame period, fps
and the tick alternation are unchanged, as expected.

Still open, unchanged by this result: display timing (no PresentMon), the tester's own
machine and full case (including his re-stutter at cap 30 under heavier GPU load), and
any fps gain (none, none claimed). The interpolation is ON by default on this branch and
`POSEIDON_RENDER_INTERP=0` restores the previous behaviour; a first-person soldier on foot
and vehicles seen from another vehicle use the same path but were not measured separately.

## Follow-up 12:16 — helicopter cockpit flicker on 3f64710a (owner report)

Cause in code, not a guess: `SceneShadowPass.cpp` submitted every dynamic shadow caster
with `obj->Transform()` (tick time) while the colour pass draws the body at its
interpolated frame. A vehicle's self-shadow — the fuselage over its own cockpit — therefore
sat up to a tick of travel away from the receiving geometry, by a different amount every
frame. The acceptance series never saw it because it ran with shadows OFF (the tester's
setting). Fixed in `ac89cf30`: shadow-map casters, both proxy-caster sites and the proxy
shadow volumes take `RenderTransform()` / the drawn parent frame. Reproduction mission
`tests/perf/missions/pace_heli.Intro` (player = gunner of an AI-flown Cobra, shadows on):
flies at 80 km/h median, no crash, camera dx jitter 9 %. Whether the flicker is gone is a
visual check by the owner; rotor-blade shadows sweeping the cockpit are legitimate and
remain.

Lesson for the next slice of this kind: every pass that positions a moving object must
take the SAME frame — colour, shadow casters, shadow volumes, motion vectors (already the
drawn matrix via `prev_worlds`), and still not covered: vehicle-attached lights, cloudlets,
skeletal animation phase.

Field note, same time: the tester writes that the stutter appears "as soon as the engine
no longer reaches the 60 fps limit (38 fps now), then it gets abruptly bad" — on HIS build,
which does not carry this branch. 38 fps is 1.58 ticks per frame; that is the mechanism
this branch addresses, observed independently.
