# REN-VEG-003 — The LOD governor works again: a 40 fps set point, the target-point drive, and the GPU in its estimate

**Date:** 2026-09-02
**Branch:** `asset-obs-001`
**Follows:** REN-VEG-002 (the governor cannot see the retained set and targets 15 fps)

## Three changes, one loop

`Scene::AdjustComplexity()` steers `_lodInvWidth` — the detail multiplier both the CPU draw
path and the GPU-driven cull read — so the estimated frame lands in a target band. Three things
were wrong with it on a modern machine, and each is now changed:

1. **The set point.** `frameRate` defaulted to 15 (band 10–20 fps, 50–100 ms) since 2001, and
   every `UserInfo.cfg` carries that value because the game wrote it. It now defaults to **40**
   (band 27–53 fps); a stored 15 is read as the legacy default, any other stored value is the
   player's. `WGR_LOD_TARGET_FPS` overrides for captures. The classic Options slider still sets it.
2. **The error metric.** The target-point form (`WGR_LOD_GOVERNOR_TARGET`, previously opt-in,
   +36% on Everon in the 2026-08-15 handover) is on. The distance form measured its error with a
   normal built from the range, so the range acted as a gain and moved the fixed point
   (`Scene.cpp:619` describes it). Now the range is the clamp it is documented to be.
3. **The GPU term.** The previous frame's retained main-view triangle count is added to the
   estimate — on every path that recomputes `complexity`. The first version (REN-VEG-002) added it
   inside the loop only; the clamp and oscillation-restore paths recomputed a CPU-only sum, that
   sum went into the four-frame history, every later estimate read three to five times too slow,
   and the loop pinned at its coarsest bound whatever the set point. That is why all the `gov*`
   arms below show `invW 0.093` and all the `hist*` arms do not.

## Measured (one run per arm, 800×600; `objects.lod_inv_width` is now in the capture JSON)

| arm | CPU frame | fps | GPU | `land:obj` | tris | LOD hist 0/1/2/3 | invW |
|---|---:|---:|---:|---:|---:|---|---:|
| Chernarus+ default, old governor | 31.1 | 32 | 30.3 | 7.65 | 3.8 M | 1513/109/0/0 | — |
| Chernarus+ default, term but broken history (60) | 17.7 | 57 | 12.9 | 5.37 | 0.3 M | 67/343/304/394 | 0.092 (pinned) |
| **Chernarus+ default, fixed (60)** | **20.5** | **49** | 20.0 | 4.04 | 2.8 M | 851/668/102/0 | 0.014 |
| Everon, old governor | 29.4 | 34 | 28.3 | 2.72 | 2.3 M | 4745/8325/950/150 | — |
| Everon, fixed, set point 60 | 24.9 | 40 | 21.6 | 1.41 | 0.3 M | 2591/6144/3544/277 | 0.028 |
| **Everon, fixed, set point 40** | **24.3** | **41** | 21.7 | — | 0.4 M | 3269/6259/3619/234 | 0.022 |
| Chernarus+ retained trees, fixed, 60 or 40 | 15.4 | 65 | 13.3 | 1.18 | 0.5 M | 71/364/378/**4580** | 0.093 (pinned) |

- **Chernarus+ default:** 31 → 20.5 ms with the near trees at level 0–1 and the picture close to
  the old one (`.tmp-veg/aerial_old_hist.png`, middle). The governor found an operating point,
  not a bound.
- **Everon:** 29 → 24 ms, one level coarser on the mid ground (`everon_old_hist.png`). 60 vs 40
  changes the frame by 0.6 ms and the detail by one level: the frame there is grass and sky,
  so coarsening objects buys nothing. **40 ships.**
- **Retained trees still pin the loop** at any set point. The DayZ tree ladder is 4k / 2k / 1k /
  **6** triangles at resolutions 1, 2, 3, 4: there is no state between "30 M triangles" and
  "cards", the loop overshoots to the cards, and the oscillation guard keeps it there. That is
  the ladder's problem, not the governor's, and it is the next one on this line. The bail stays
  on by default.

## What this changes for the player

Every world: the LOD governor now does its job at a modern frame rate. Objects get coarser when
the frame is slow and finer when it is fast, inside a 27–53 fps band. A player who set the
classic frame-rate slider keeps their value; everyone else moves from a 2001 default nobody
chose. `WGR_LOD_GOVERNOR_TARGET=0`, `WGR_LOD_GOVERNOR_RETAINED=0` and `WGR_LOD_TARGET_FPS=15`
together restore the old behaviour exactly.

## Not measured

One run per arm and one pose per world; DLSS off; 800×600. Overcast and night were not run. The
retained-tree ladder needs either a mid LOD (a vegetation LOD bias at registration that maps the
4-step ladder onto the governor's range) or a smoother loop; that is REN-VEG-004.
