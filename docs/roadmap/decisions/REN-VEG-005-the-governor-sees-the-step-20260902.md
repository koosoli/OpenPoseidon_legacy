# REN-VEG-005 — The governor sees the cost of a step before it takes it

**Date:** 2026-09-02 (night)
**Branch:** `asset-obs-001`
**Follows:** REN-VEG-004 (the retained-tree sawtooth: a blind refine step across a 700× ladder)

## The mechanism

The cull's count pass already walks every surviving instance's LOD ladder once. It now walks
it four more times, for `lod_inv_width` scaled by 0.5, 0.71, 1.41 and 2.0, and sums the
triangles the *same survivors* would draw at each — four words in the stats block, read back
with the rest (`objects.lod_governor.whatif_tris` in the capture JSON). The governor keeps that
ladder together with the multiplier it was measured at, and every iteration — and every path
that recomputes the complexity sum — asks for the retained cost *at the multiplier it is about
to try*, interpolated in log scale through the five known points, instead of assuming last
frame's count.

## Measured (one run per arm, 800×600, WLD-025 pose / Everon `perf_combat`)

| arm | CPU frame | p95 | fps | GPU | tris | LOD hist 0/1/2/3 | invW | iterations |
|---|---:|---:|---:|---:|---:|---|---:|---:|
| Chernarus+ retained trees, before | 15.4 | **46.7** | 65 | 12.7 | 0.5 M | 71/364/378/4580 | 0.093 (clamp) | 5 |
| Chernarus+ retained trees, **what-if** | 20.6 | **30.8** | 49 | 16.2 | 1.5 M | 211/476/674/4164 | 0.056 | 5 |
| Chernarus+ default (CPU trees) | 20.2 | 26.3 | 49 | 20.6 | 2.4 M | 727/607/228/59 | 0.016 | 1 |
| Everon | 25.7 | 28.3 | 39 | 21.6 | 0.4 M | 3257/6259/3619/234 | 0.022 | 2 |

The retained arm's what-if ladder at capture: 9.6 M triangles at half detail, 4.4 M at 0.71×,
0.62 M at 1.41×, 0.38 M at 2×. The loop can now see that one refine step costs 4.4 M
triangles, and it stopped at 0.056 instead of overshooting to the clamp and back: p95 47 → 31 ms,
the five-second sawtooth is gone. The default arms are unchanged and converge in 1–2 iterations;
their ladders read as expected.

## What this decides

- **The governor is now correct for a GPU-retained set.** That was the prerequisite for ever
  putting vegetation on the retained path by default.
- **The default still does not flip.** With an honest estimate, the retained trees land on the
  *same* frame as the CPU-drawn ones (20.6 vs 20.2 ms) at this window on this GPU: the CPU
  millisecond they save (`land:obj` 4 → 1 ms) is spent on the GPU, and the picture is coarser
  (most trees at the card level). The retained path wins only where the frame is CPU-bound —
  a scene with more objects per frame than this GPU-bound pose. `WGR_TREEADV_RETAINED=1`
  stays the lever, now without the sawtooth.

## Not measured

One run per arm. The retained arm at a CPU-bound pose (a town at ground level with the AI
running) is the measurement that would justify a per-world default; a high resolution with DLSS
on, where the GPU headroom differs, is the other.
