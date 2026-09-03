# PERF-028 — Three hypotheses for PERF-027, all measured, all wrong; the mechanism is still open

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-027 (footprints double the cost of a soldier)

PERF-027 established the fact: with ~2,200 footprint marks alive, the same 193 soldiers cost
roughly twice as much to simulate. This decision records what that is **not**, so the next session
starts from three closed doors instead of the same three guesses.

## Eliminated 1 — it is not the marks' own simulation

By-kind accounting: `other` is **0.213 ms over 2,482 calls**, 0.086 µs each. The marks tick
almost for free. The cost is inside `man`.

## Eliminated 2 — it is not creating them

The discriminator is a third arm of `POSEIDON_MARK_TTL`. At **1 second** the spawn rate is
unchanged (every footfall still makes a mark) while the live population falls tenfold:

| TTL | marks alive (`other` calls/tick) | `man` ms/tick |
|---:|---:|---:|
| 10 s (shipped) | 2,237 | 3.389 |
| **1 s** | **124** | **2.332** |
| 0 (none spawned) | 78 | 2.381 |

One second and zero cost the same. So it tracks the **live population**, not the spawn rate, and
entity creation is not the mechanism.

## Eliminated 3 — it is not `Landscape::RoadSurfaceY`

That function looked like the answer on inspection: every soldier calls it several times a tick to
find the ground under its feet, and it walks **every object in the neighbouring landscape cells**
looking for a roadway level — cells that are now full of that soldier's own footprints.

`POSEIDON_ABLATE_ROAD_SURFACE=1` (added here, off by default) skips the scan entirely and returns
the bare terrain height. It is a measurement arm, not an option: it takes roads out from under
everyone's feet.

| | `man` ms/tick | `sim:step` |
|---|---:|---:|
| road scan on | 3.477 | 6.12 |
| road scan **off** | 3.858 | 6.25 |

Removing the whole scan does not make it faster. The scan is not where the time goes.

## What the next session needs to know about the noise

`man` is not a quiet number on this scene: across runs it read 3.39, 3.48, 3.86 and 4.77 ms with
marks present, and 2.24 / 2.38 without. The **separation** is solid — the lowest marked run is
still above the highest unmarked one — but any single-run difference under about 1 ms on this
figure is noise. PERF-027's "2.1×" is the ratio of two single runs and should be read as "roughly
double", not as a measurement.

## Where to look next

Something in a soldier's tick scales with the number of entities standing near it, and it is
neither the entity list walk (`slow.moveout` is 0.14 ms) nor the road scan. Candidates not yet
tested: the collision/intersection path that queries nearby objects for movement, and the sound
or animation code that resolves what is underfoot. The way to settle it is a sampling profiler on
one marked and one unmarked run, not another reading of the source — this decision is the record
of three careful readings of the source that were each wrong.
