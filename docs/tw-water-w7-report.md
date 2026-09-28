# TW-WATER W7 — performance pass and parity report

Tidewater Native (`WGR_WATER_BACKEND=1`) against Current OP (`WGR_WATER_BACKEND=0`) on the W7
camera paths, after the W7c and W7e/W7f fixes. Plan: `.tw/probe/plan.md` §7 (performance), §9
(when may the old path go), §12 (this template).

```
Package: W7   Branch/commit: dec/tidewater-w7f-early-submit-default 5fb37a40
Deploy stamp: dec/tidewater-w7f-early-submit-default 5fb37a40 2026-09-28_10:38 deploy-ps1
Status: measured (one machine, one run per path and backend; see "Not done")
Captures: .tw/shots-w7f-{p1,p2,p3}-{b0,b1}, .tw/shots-w7f-{p1,p3}-b0-noplanar
```

## How it was measured

- Harness: `.tw/test-float.ps1` under `scripts/with-game-lock.sh`, `--render wgpu --window --dev
  --width 1280 --height 720`, the same deployed build for both backends; only
  `WGR_WATER_BACKEND` differs (plus `WGR_ABLATE=planar` for the reflection A/B).
- Paths (`.tw/float/gen.py`, Everon at 9950/1450): **P1** low pass over open sea to the beach,
  holding on the surf zone; **P2** descent through the waterline looking up, then rising; **P3**
  fly-overs at 150 m and 600 m along the coast. Calm sea, same weather and time of day in both.
- `WGR_FRAME_STATS=1` (W7a/W7b) logs every 5 s: wall-clock and GPU frame p50/p95/p99, the water
  GPU regions, backend allocations and the render worker's CPU phases. The tables give the
  median over the 5 s windows of a run, dropping the first window (start-up: shader and
  pipeline creation put its p99 near 100 ms in both backends). "Worst p99" is the worst window.
- Upscaler, MSAA, view distance (`setViewDistance 2000`) and the LOD settings are the build's
  defaults and are the same in both runs; the LOD governor was not locked.

## Frame time (ms)

| Path | Backend | wall p50 | wall p95 | wall p99 | worst p99 | GPU p50 | GPU p95 | GPU p99 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| P1 | Current OP | 8.26 | 9.16 | 9.73 | 12.20 | 7.46 | 8.42 | 8.82 |
| P1 | Tidewater | **6.87** | **7.94** | **8.50** | 8.74 | 5.70 | 6.55 | 7.12 |
| P2 | Current OP | 7.97 | 8.79 | 10.85 | 11.28 | 6.80 | 7.81 | 8.28 |
| P2 | Tidewater | **6.43** | **7.40** | **7.95** | 8.38 | 5.34 | 6.27 | 6.68 |
| P3 | Current OP | 8.69 | 9.33 | 9.94 | 10.51 | 7.91 | 8.59 | 8.98 |
| P3 | Tidewater | **6.10** | **6.80** | **7.24** | 8.89 | 4.78 | 5.49 | 5.78 |

Tidewater mode's frame p95 is below Current OP's on P1, P2 and P3 (§9 criterion 2's timing half,
on this machine and these runs).

## Water GPU (ms, mean per frame)

The frame-stats columns share the water draw region, so they are split here: *simulation* is
the backend's own compute (Current OP: every top-level water region except the draw;
Tidewater: `TW ocean spectrum`, `TW ocean FFT`, `TW shore sim` (which includes the breakers and
their spray), `TW boat wakes`), *draw* is `Water draw` in the frame.

| Path | Current OP simulation | Current OP draw | Current OP total | Tidewater simulation | Tidewater draw | Tidewater total |
| --- | --- | --- | --- | --- | --- | --- |
| P1 | 0.65 | 1.76 | 2.41 | 0.20 | 0.45 | 0.65 |
| P2 | 0.69 | 1.45 | 2.14 | 0.15 | 0.38 | 0.53 |
| P3 | 0.59 | 3.02 | 3.61 | 0.22 | 0.33 | 0.55 |

These are the timed regions only. Tidewater's caustics passes and the bow-spray update offshore
(no shore simulation running) are not in a region, and Current OP's planar reflection is not in
its water regions (see the reflection A/B below for its cost).

## VRAM

Backend allocations (the renderer's allocation tracker, largest window): P1 942 MB Tidewater vs
948 MB Current OP; P2 941 vs 949; P3 938 vs 946. Within §9's "no worse than Current OP + 10 %".
This is the renderer's own accounting, not a driver-reported process figure.

## What changed in W7 to get here

W7a (`WGR_FRAME_STATS`) showed Tidewater's GPU frame 1.5–3 ms shorter than Current OP's, but its
wall-clock p50 on P1/P2 *worse* (P1 ~9.3 vs ~8.6 ms) and recurring ~14 ms p99 frames. W7b split the
render worker's CPU time and the Tidewater `prepare`:

1. **p99 spikes → the shore station re-trace (W7c).** `update_stations` traced the breakers'
   shoreline stations (marching squares, 30 smoothing passes, resampling: ~11 ms) on the render
   thread whenever a simulated region's key (centre, terrain generation, sea level in 0.1 m
   steps) changed, 8–16 times per 5 s on these paths. It now runs on a worker thread, and the
   last 8 traces are cached (up to half the requests were keys that came back). Station update on the render thread: max 11.2 ms → 0.1 ms; Tidewater P1 p99
   14.3 → 10.4 ms.
2. **p50 deficit → a GPU bubble at submit (W7e, default since W7f).** Current OP splits its frame
   after the planar reflection and submits that part early, so the GPU works while the rest of the
   frame is recorded and submitted. A Tidewater frame has no planar pass and was one command
   buffer: the GPU waited for the whole ~2.9 ms `queue.submit`. Tidewater frames now submit the
   early work (water simulation, culls, far field, GI) at the same kind of point. A/B on the same
   build: P1 wall p50 9.2 → 6.9 ms, P3 7.7 → 6.2 ms, GPU time unchanged, no validation errors,
   captures unchanged (`.tw/shots-w7e-*`). `WGR_TW_EARLY_SUBMIT=0` restores one submission.

The remaining `prepare` maximum (~4 ms once per 5 s window, mean 0.03 ms) is not the station
trace and was not attributed further.

## Experiments (§7)

1. **Reflection (done):** Current OP with the planar reflection vs without (`WGR_ABLATE=planar`).

   | Path | Current OP GPU p50 | without planar | Tidewater GPU p50 |
   | --- | --- | --- | --- |
   | P1 | 7.46 | 5.22 | 5.70 |
   | P3 | 7.91 | 5.50 | 4.78 |

   The planar pass is ~2.2–2.4 ms of GPU, most of the GPU gap. Without it Current OP's GPU frame
   is 0.5 ms *shorter* than Tidewater's on P1 and 0.7 ms longer on P3. Its wall-clock does not
   improve (P1 p50 8.04 ms): with the planar pass gone its frame is one command buffer and it
   gets the same submit bubble Tidewater had before W7e.
2. **Mesh density (not done):** needs a build with a different `cdlod::GRID`; no switch exists.
3. **FFT, 4. Shore per-region cost, 5. Spray budget / overdraw, 6. Underwater resolution
   (not done).** The breakers and spray share `TW shore sim` and the caustics have no GPU region,
   so experiments 4–6 need their own regions first.

## Captures (for the owner's eyes)

`.tw/shots-w7f-*` (and the side-by-side sheet delivered with this report). What I see, to be
confirmed:

- **P1:** Tidewater's water is brighter and more turquoise over the shallows; Current OP's is
  darker and greyer. Breaker lips are small on this calm sea in both.
- **P2:** Tidewater splits the waterline at the lens (sky above, water below) and, from 3 m
  down looking up at ~13°, shows the underside of the surface with bright Snell-window patches
  and no island: at that elevation the surface reflects totally, so this matches the physics.
  Current OP shows the island through the surface from below.
- **P3:** since W6i.7 fixed the view-space sign, Tidewater's refraction is valid, and from 150–
  600 m the sea bed shows through the water as dark streaks. Current OP shows an opaque blue
  sea. Whether that sea bed look is wanted is the owner's call.

## Deviations from Tidewater and why

- The shore stations are traced off the render thread with a small cache (W7c): Tidewater builds
  them once, when its breakers are created, for its one fixed beach; OP's simulated regions follow
  the camera along the coast, so the trace recurred during play.
- The early submit (W7e/W7f) is an OP frame-structure change, not a Tidewater one: it matches
  what Current OP's planar split already does.
- Everything else as recorded per package (W2–W6 commit messages and file headers).

## Not done / not verified

- One run per path and backend on one machine and resolution (1280×720); no day/night/storm
  pass, no second coast (§8 manual captures), no LOD-governor lock.
- Backend-switch soak (§8: 20+ switches with VRAM and handles back to baseline): the harness
  sets the backend per session through the environment; a runtime switch needs the Water tab.
- One-off costs (backend creation, ShoreField precompute, region activation), triangles and
  draw calls: not in the frame statistics yet.
- Experiments 2–6 above.
- The early submit assumes no pass in the early command buffer reads an upload made later in
  the frame; captures and validation show no difference, but a one-frame lag of something
  culled or simulated early would only show in motion.
- §9 criteria 1 (the owner prefers it), 3 (nothing regresses: rivers, lakes, server,
  multiplayer, save/load) and 4 (every Tidewater dev-tools parameter has an effect) are not
  assessed here.

## Next hypothesis

Tidewater is not slower than Current OP on these paths. The largest remaining CPU item in both
backends is `queue.submit` (~2 ms Tidewater, ~2.3 ms Current OP): wgpu's per-submit resource
tracking over the frame's command buffers. Splitting the late part of the frame (shadows, sky,
scene) into further early submits would hide more of it; the same split would also give Current
OP back its wall-clock when the planar reflection is off.
