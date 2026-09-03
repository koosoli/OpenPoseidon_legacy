# REN-GI-006 — The sun-bounce gain is inert where it was expected to matter; the default stays

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Closes:** the second "not done" of REN-GI-003 / REN-GI-002 ("a pose with deep shade beside a
sunlit wall before the gain default is chosen")

## The pose

`gi_shade270.eden`: the same placed farmhouse as REN-GI-005, at 17:00 with the sun low. The
player stands 12 m from it, in shade, with the house's stone foundation wall filling the upper
third of the frame and sunlit grass at the left edge. It is the case the reflective shadow map
of REN-GI-002 exists for — a surface that the sun cannot see, a metre from a surface it lights
hard.

## Measured (Everon, 17:00, 3441×1440, `POSEIDON_VSYNC=0`)

`WGR_GI_RSM_GAIN`, one run each:

| gain | shaded stone wall | sunlit grass |
|---:|---:|---:|
| 0 (bounce off) | 62.44 | 157.88 |
| 1 (default) | 62.45 | 157.92 |
| 4 | 62.46 | 157.97 |
| 12 | **62.51** | 157.95 |

**Twelve times the bounce moves the shaded wall by 0.07 luma — 0.1%.** The crops at gain 0 and
gain 12 are indistinguishable. Since the gain multiplies the bounce term, the gain-0 row *is*
the bounce-off arm, so this is not a scaling question: the term arrives at essentially zero on
this surface.

## What that settles, and what it does not

**The default gain stays at 1.** No value in the tested range changes the picture here, so there
is nothing to tune toward; raising it would only add risk on the poses where the term is not
zero.

It is not broken everywhere: REN-GI-002 measured gain 12 moving the *ground's* mean luma +2.3%
on `perf_town`, over open sunlit terrain. The pattern across the two poses is that the bounce
registers over large sunlit areas seen at a shallow angle, and vanishes on a wall standing next
to them — which is the opposite of where a first bounce is wanted.

So the honest position on the hybrid GI as it stands:

- **Stage 1 + stage 3 (the probe volume and per-probe visibility) are what make interiors
  right.** REN-GI-005 measured the shaded interior wall +35.3% with the sunlit wall unmoved.
- **Stage 2 (the sun proxy) is not yet earning its place in the picture.** Its cost is inside the
  0.19 ms probe update and it does no harm, so it stays on; but it should not be credited with
  the indoor improvement, and the next GI work is a diagnosis of why the proxy's radiance does
  not reach a vertical surface, not a tuning pass on its gain.

`GI: sun proxy` reading n/a in a capture is expected and is not evidence either way: the proxy
re-renders when its view moves, else at most one frame in eight, and the timer reports only a
frame it actually bracketed.
