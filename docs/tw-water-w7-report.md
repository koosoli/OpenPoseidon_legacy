# TW-WATER W7 — performance pass and parity report

Tidewater Native (`WGR_WATER_BACKEND=1`) against Current OP (`WGR_WATER_BACKEND=0`) on the W7
camera paths. Plan: `.tw/probe/plan.md` §7 (performance), §8 (tests), §9 (when may the old path
go), §12 (this template). Updated after W5e, W6j–W6l and W7h.

```
Package: W7   Branch/commit: dec/tidewater-w7h-measurements 145f123c
Deploy stamp: dec/tidewater-w7h-measurements 145f123c 2026-09-28_11:57 deploy-ps1
Status: measured (one machine, one run per path and backend; see "Not done")
Captures: .tw/shots-w7h-{p1,p2,p3}-{b0,b1}, .tw/shots-w7h-p{1,3}-grid{64,96}, .tw/shots-w7h-soak,
          .tw/shots-w7f-{p1,p3}-b0-noplanar
```

## How it was measured

- Harness: `.tw/test-float.ps1` under `scripts/with-game-lock.sh`, `--render wgpu --window --dev
  --width 1280 --height 720`, the same deployed build for both backends; only
  `WGR_WATER_BACKEND` differs (plus the switch named in each experiment).
- Paths (`.tw/float/gen.py`, Everon at 9950/1450): **P1** low pass over open sea to the beach,
  holding on the surf zone; **P2** descent through the waterline looking up, then rising; **P3**
  fly-overs at 150 m and 600 m along the coast. Calm sea, same weather and time of day in both.
- `WGR_FRAME_STATS=1` logs every 5 s: wall-clock and GPU frame p50/p95/p99, the water GPU
  regions, backend allocations and the render worker's CPU phases. The tables give the median over
  the 5 s windows of a run, dropping the first window (start-up: shader and pipeline creation put
  its p99 near 100 ms in both backends). "Worst p99" is the worst window.
- Upscaler, MSAA, view distance (`setViewDistance 2000`) and the LOD settings are the build's
  defaults and are the same in both runs; the LOD governor was not locked.

## Frame time (ms)

| Path | Backend | wall p50 | wall p95 | wall p99 | worst p99 | GPU p50 | GPU p95 | GPU p99 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| P1 | Current OP | 8.28 | 9.25 | 9.77 | 12.63 | 7.48 | 8.43 | 8.98 |
| P1 | Tidewater | **6.85** | **7.96** | **8.54** | 9.34 | 5.69 | 6.63 | 7.12 |
| P2 | Current OP | 7.99 | 8.71 | 9.75 | 11.09 | 6.78 | 7.78 | 8.25 |
| P2 | Tidewater | **6.51** | **7.52** | **7.93** | 8.50 | 5.38 | 6.26 | 6.68 |
| P3 | Current OP | 8.67 | 9.25 | 9.69 | 10.24 | 7.88 | 8.56 | 8.87 |
| P3 | Tidewater | **6.10** | **6.75** | **7.14** | 8.79 | 4.79 | 5.47 | 5.81 |

Tidewater mode's frame p95 is below Current OP's on P1, P2 and P3 (the timing half of §9
criterion 2, on this machine and these runs). The W7f build measured the same within 0.1 ms.

## Water GPU (ms, mean per frame)

Since W7h every Tidewater compute stage has its own region (82 `Tidewater: caustics` and
83 `Tidewater: breakers + spray` are new; before W7h the caustics were untimed and the breakers
sat inside `shore sim`).

| Path | Tidewater spectrum + FFT | shore sim | breakers + spray | caustics | boat wakes | water draw | **Tidewater total** |
| --- | --- | --- | --- | --- | --- | --- | --- |
| P1 | 0.095 | 0.043 | 0.056 | 0.153 | 0.001 | 0.448 | **0.80** |
| P2 | 0.090 | 0.043 | 0.056 | 0.144 | 0.001 | 0.339 | **0.67** |
| P3 | 0.108 | 0.042 | 0.064 | 0.180 | 0.002 | 0.333 | **0.73** |

| Path | Current OP simulation (every other top-level water region) | water draw | **Current OP total** |
| --- | --- | --- | --- |
| P1 | 0.66 | 1.77 | **2.43** |
| P2 | 0.69 | 1.45 | **2.14** |
| P3 | 0.59 | 3.01 | **3.60** |

Not in these regions: Current OP's planar reflection (see the reflection A/B) and Tidewater's
underwater composite (a post pass; untimed). The boat wakes are ~0 here because no boat is near
the camera on P1–P3; on the boat runs (`.tw/shots-bowE-*`) they cost 0.04–0.05 ms.

## Mesh (Tidewater)

The prepare line reports the last frame's CDLOD selection: P1 128 nodes / 262,144 triangles,
P2 132 / 270,336, P3 59 / 120,832, grid 32 per node, 2 simulated shore regions.

## VRAM

Backend allocations (the renderer's allocation tracker, largest window): P1 944 MB Tidewater vs
950 MB Current OP; P2 941 vs 949; P3 938 vs 944. Within §9's "no worse than Current OP + 10 %".
This is the renderer's own accounting, not a driver-reported process figure.

## What changed in W7 to get here

W7a (`WGR_FRAME_STATS`) showed Tidewater's GPU frame 1.5–3 ms shorter than Current OP's, but its
wall-clock p50 on P1/P2 *worse* (P1 ~9.3 vs ~8.6 ms) and recurring ~14 ms p99 frames. W7b split the
render worker's CPU time and the Tidewater `prepare`:

1. **p99 spikes → the shore station re-trace (W7c).** `update_stations` traced the breakers'
   shoreline stations (marching squares, 30 smoothing passes, resampling: ~11 ms) on the render
   thread whenever a simulated region's key (centre, terrain generation, sea level in 0.1 m
   steps) changed, 8–16 times per 5 s on these paths. It now runs on a worker thread, and the
   last 8 traces are cached (up to half the requests were keys that came back). Station update on
   the render thread: max 11.2 ms → 0.1 ms; Tidewater P1 p99 14.3 → 10.4 ms.
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

1. **Reflection (done, W7f build):** Current OP with the planar reflection vs without
   (`WGR_ABLATE=planar`).

   | Path | Current OP GPU p50 | without planar | Tidewater GPU p50 |
   | --- | --- | --- | --- |
   | P1 | 7.46 | 5.22 | 5.70 |
   | P3 | 7.91 | 5.50 | 4.78 |

   The planar pass is ~2.2–2.4 ms of GPU, most of the GPU gap. Without it Current OP's GPU frame
   is 0.5 ms *shorter* than Tidewater's on P1 and 0.7 ms longer on P3. Its wall-clock does not
   improve (P1 p50 8.04 ms): with the planar pass gone its frame is one command buffer and it
   gets the same submit bubble Tidewater had before W7e.
2. **Mesh density (done, W7h, `WGR_TW_GRID`):**

   | Grid per node | P1 triangles | P1 water draw | P1 wall p50 / GPU p50 | P3 wall p50 / GPU p50 |
   | --- | --- | --- | --- | --- |
   | 32 (Tidewater's) | 262,144 | 0.45 ms | 6.85 / 5.69 | 6.10 / 4.79 |
   | 64 | 1,048,576 | 0.74 ms | 6.87 / 5.72 | – |
   | 96 | 2,359,296 | 1.11 ms | 7.08 / 5.95 | 6.15 / 5.31 |

   At 96 the draw costs 2.5× as much and the frame +0.2–0.5 ms of GPU. P1 at 15 s and the P3
   horizon at 45 s show no visible difference between 32 and 96 in stills
   (`.tw/shots-w7h-*-grid96`); motion (shimmer at the horizon) was not judged. 32 stays.
3. **FFT layout (not done):** there is no switch for OP's FFT layout inside Tidewater mode;
   Tidewater's spectrum + FFT costs 0.09–0.11 ms, which caps what the experiment could win.
4. **Shore per region (done):** 2 simulated regions: `shore sim` 0.042 ms (≈0.02 per region),
   breakers + spray 0.056–0.064 ms. Region warm-up is 3 s of weight ramp, no GPU spike; the
   ShoreField and the stations are computed on worker threads (untimed, off the frame).
5. **Spray (partly):** breaker and boat spray share `breakers + spray` (0.06 ms on P1–P3, where
   no boat is near). Overdraw was not measured.
6. **Underwater resolution (not done):** Tidewater's underwater composite runs at full
   resolution in OP (Tidewater: half-resolution shafts + TAA) and has no GPU region yet.

## Backend switch soak (§8, done, W7h)

`WGR_WATER_SOAK=60,24` on P1: 24 switches, one every 60 frames, ending on Current OP
(`.tw/shots-w7h-soak`, the `W7 switch` log lines).

- Switch cost: Current OP → Tidewater 324–332 ms, Tidewater → Current OP 91–95 ms (one frame each,
  plan §4.2 accepts a one-frame hitch).
- Backend allocations: 927.6 MB in Tidewater mode and 970.3 MB in Current OP mode on every
  switch, with one +4.3 MB step for both at switch 9 (with +3 buffers, +15 views, +19 bind groups
  and +6 pipelines on *both* sides afterwards: something outside the water loaded, not a
  per-switch leak); flat for the other 23 switches.
- Live buffers, texture views, bind groups, pipelines and shader modules return to the same
  count for each backend every time.
- The backend's *texture count* (wgpu-hal's counter) drifts by −44 per round trip (−34 → −525)
  while texture memory stays flat: the counter decrements for textures it never counted (it goes
  negative from the first switch). Memory says there is no leak; the counter itself was not
  investigated further.

## Parity work since the first report

- **W5e — the diver's torch.** Tidewater's torch beam in-scatter under water, from OP's own spot
  light at the eye (the dev head torch; any spot within 2 m of the camera), with OP's cone and
  fall-off. At night under water the beam now reads as lit water in front of the diver; by day
  and without a torch nothing changes (`.tw/shots-w5e-*`). `WGR_TW_TORCH_BEAM` scales it.
- **W6j–W6l — bow spray.** The Russian patrol boat (BoatE) and the PBR were logged to emit
  identically; both disappeared in white when the bow settled back in at ~17 m/s. The spray's cap,
  ligaments, white water and mist were cut (`WGR_TW_BOAT_SPRAY_TUNE`, arm C), checked on both
  boats in calm and rough seas (`.tw/shots-w6l-*`).
- **Underwater "cloud blobs".** From 4 m down looking up (`.tw/shots-uw-day-b1` t11 / t15) the
  white shapes with grey undersides are the sky's clouds seen through Snell's window: the
  water material samples the sky panorama, clouds included, along the refracted ray, exactly as
  Tidewater's `skyRadianceWithClouds(Td)` does. Outside the window the surface shows total
  internal reflection. This is Tidewater's model and physically expected; whether they should be
  softened is the owner's call.

## Captures (for the owner's eyes)

- **P1:** Tidewater's water is brighter and more turquoise over the shallows; Current OP's is
  darker and greyer. Breaker lips are small on this calm sea in both.
- **P2:** Tidewater splits the waterline at the lens and, from 3 m down looking up at ~13°, shows
  the underside of the surface with total internal reflection and no island (at that elevation
  the surface reflects totally); Current OP shows the island through the surface from below.
- **P3:** since W6i.7 fixed the view-space sign, Tidewater's refraction is valid, and from 150–
  600 m the sea bed shows through the water as dark streaks. Current OP shows an opaque blue sea.
- **Under water, day and night** (`.tw/shots-uw-*`, `.tw/shots-w5e-*`): Tidewater's medium is
  a clear blue with caustics and the torch beam; Current OP's is a flat teal fog, grey at night.

## Deviations from Tidewater and why

- The shore stations are traced off the render thread with a small cache (W7c): Tidewater builds
  them once, when its breakers are created, for its one fixed beach; OP's simulated regions follow
  the camera along the coast, so the trace recurred during play.
- The early submit (W7e/W7f) is an OP frame-structure change, not a Tidewater one: it matches
  what Current OP's planar split already does.
- The torch (W5e) is OP's head torch / spot lights, not Tidewater's flashlight object; its cone and
  fall-off are OP's so the beam matches what the torch lights on surfaces. No marine snow.
- The bow spray is below Tidewater's level (W6l) for patrol boats at 13–17 m/s.
- Everything else as recorded per package (W2–W6 commit messages and file headers).

## Not done / not verified

- One run per path and backend on one machine and resolution (1280×720); no storm pass, no second
  coast, no DLSS/FSR runs (§8 manual captures), no LOD-governor lock.
- Experiments 3 and 6, overdraw for 5, and GPU regions for the underwater composite.
- The early submit assumes no pass in the early command buffer reads an upload made later in
  the frame; captures and validation show no difference, but a one-frame lag of something
  culled or simulated early would only show in motion.
- Mesh density was judged in stills only.
- §9 criteria 1 (the owner prefers it), 3 (nothing regresses: rivers, lakes, server,
  multiplayer, save/load) and 4 (every Tidewater dev-tools parameter has an effect) are not
  assessed here.

## Next hypothesis

Tidewater is not slower than Current OP on these paths. The largest remaining CPU item in both
backends is `queue.submit` (~2 ms Tidewater, ~2.3 ms Current OP): wgpu's per-submit resource
tracking over the frame's command buffers. Splitting the late part of the frame (shadows, sky,
scene) into further early submits would hide more of it; the same split would also give Current
OP back its wall-clock when the planar reflection is off.
