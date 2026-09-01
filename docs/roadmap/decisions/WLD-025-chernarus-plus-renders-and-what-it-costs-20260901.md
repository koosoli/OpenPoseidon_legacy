# WLD-025 — Chernarus+ renders, and the cost is not where the streamer is

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**World:** `packages/dayz-compat/world/chernarusplus/chernarusplus.wrp`, DayZ addons
**Follows:** the object preload work of the same day, and the owner's note that this map
"never ran conclusively"

The owner suggested this map as a stress case because it has far more objects than anything
in the bench set and had never produced a usable run. It produces one now.

## What it does

Full render at the owner's pose (`5704.58 2523.37`, altitude 120, hour 12): textured terrain,
apartment blocks, autumn foliage, roads, coastline, and the `perf_combat` mission's AI running
and talking on top of it.

| view | frame mean | p95 | fps | `land:obj` | `swap` | `sim:step` |
|---|---:|---:|---:|---:|---:|---:|
| az 260.9 | 40.79 ms | 45.84 | 24.5 | **20.05** | 11.22 | 5.19 |
| az 80.9 (denser) | 34.33 ms | 37.78 | 29.1 | **13.10** | 11.08 | 4.99 |

## The number that matters

**`land:obj` is 38–49% of the CPU frame**, against 2–3 ms on the CWA islands. It is the
per-frame object list build, and it is paid *every frame*, in steady state, long after the
streamer has gone quiet.

That is worth stating plainly because it contradicts the guess this run was made to test. The
hypothesis was that the map never ran because the streamer thrashes at its budget ceiling. It
does not: after 276 residency updates the streamer admits **5 objects per update** and is
essentially idle. The fill converges. What costs is the standing work of having 20,000 objects
resident, not the work of getting them there.

So the preload mode landed the same day helps the *fill* and does nothing for this: 34.33 ms
without it against 34.57 with, measured past the fill. That is the correct result, not a
disappointment — the two are different problems and it was a guess that put them together.

## The proportion behind the map

**2,971,218 object placements against a resident budget of 20,000.** Never more than **0.67%**
of the world's objects exist at once. The budget is not tightly set; it is smaller than the map
by a factor of 150, and the map still looks like the picture. Whether 20,000 is the right
number is a separate question this does not answer — but any future work on residency budgets
should start from that ratio rather than from the CWA islands, where the whole world fits.

## Two process notes, both mine

**The first three runs were worthless and looked plausible.** `--test-world` without
`--addon-root` gives white terrain and no objects, because the engine cannot find the foreign
world's models or ground textures. Water and sky render normally, so the frame looks like a
rendering regression rather than a missing flag. The recipe requires both; I did not follow it,
and the owner spotted the picture before I had drawn any conclusion from the numbers.

**The preload mode's safety bound is now observed rather than argued.** Its commit says
explicitly that the 64-update bound was correct by construction and code review only, because a
1500-frame run on Stratis reaches 7–8 updates and never exercises it. Chernarus+ reaches 228,
and the log says what it should:

```
Modern object preload: ended after 64 updates (update bound reached); per-update caps back on
```

## What this does not say

- One machine, one pose pair, one run per arm. The `land:obj` share is large enough that
  run-to-run spread does not threaten the conclusion; nothing finer should be read off it.
- It does not establish that this map is *playable* — 24–29 fps at 1280×720 with DLSS off is a
  measurement, not a verdict.
- It says nothing about why `land:obj` costs what it does. That is the next question and it is
  not answered here.
