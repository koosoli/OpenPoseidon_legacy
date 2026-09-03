# PERF-029 — The footprint cost is spread evenly across a soldier's whole tick

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-027 (the effect), PERF-028 (three mechanisms eliminated)

## The bisection

PERF-028 ended with "a sampling profiler, not a fourth reading of the source". This is the
bisection instead, which is cheaper and answers the same question: `Soldier::Simulate` is
bracketed in two halves — `man.pilot` (the Keyboard/AI/Fake/Disabled pilot branch) and `man.base`
(everything under `base::Simulate`, i.e. Person, Man, EntityAI, Entity). Both rows are no-ops
unless `POSEIDON_SIM_VEHICLE_COST=1`, and they nest inside `SimulateTimed`'s `man` row, so they
must add up to it — and they do.

## Measured (Everon `perf_combat`, noon, two runs per arm)

| | `man.pilot` | `man.base` | `man` total |
|---|---:|---:|---:|
| marks (TTL 10 s) | 2.055 / 1.811 | 1.511 / 1.775 | 3.582 / 3.603 |
| no marks (TTL 0) | 1.356 / 1.264 | 1.009 / 1.043 | 2.380 / 2.322 |
| **difference** | **+0.62** | **+0.61** | +1.24 |

**The extra cost is split evenly down the middle.** The pilot branch and the base simulation chain
have almost nothing in common — one is decision-making and input, the other is animation, sound,
damage and transform bookkeeping — and footprints make both of them about 50% more expensive by
the same absolute amount.

## What that rules out, and what is left

It rules out the whole class of explanations PERF-028 was still hunting: **there is no single
query.** A neighbour scan, a target list, a collision walk — any one of those lives on one side of
the split, and no such thing could raise the other side by the same 0.6 ms.

What is left is systemic: something that makes *all* of the soldier's code slower when 2,500 extra
entities are being touched every tick. The candidate is **memory** — `SimulateOnly` walks the
whole `_vehicles` array and calls a virtual on each of 2,500 marks, so every soldier's working set
is evicted between its own ticks. That is consistent with the marks' own body being nearly free
(0.086 µs each) while their *presence* costs a millisecond somewhere else, and with `static`
entities moving in the same direction but less (0.021 → 0.025 ms) since their tick touches less.

It is a hypothesis, and this decision does not claim it as measured.

## The step that follows either way

**Touch the marks less often.** A footprint needs its alpha faded over ten seconds and needs to
expire; neither needs 60 updates a second. Iterating them at a lower rate cuts the touching
directly, and it removes their own 0.21 ms as a bonus. It requires knowing which list entries are
marks without asking each one — i.e. a separate list — which is the design question the next
session should start from, not another measurement.
