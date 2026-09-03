# PERF-027 — Footprints do not cost time by existing; they make every soldier twice as expensive

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-023 (the Vehicles stage by kind), PERF-026 (a CPU draw call costs 0.2 µs)

## Where this started

`sim:step` is 6.1 ms of a 20.9 ms CPU frame on Everon `perf_combat`, and the Vehicles stage owns
4.1 ms of it — all of it in the `slow` sub-stage, of which the by-kind accounting could only
explain about half.

`POSEIDON_MARK_TTL=0` (PERF-026's ablation) moves **`sim:step` 6.07 → 3.91 ms**. So the
footprints, which PERF-026 proved cost 0.19 ms of draw recording, cost **2.2 ms of simulation**.
Ten times more, in the other direction, in a place nobody had looked.

## Two wrong guesses, both measured and dropped

1. **"The instrument is the cost."** `SimulateTimed` runs two clock reads and a six-step
   `IsKindOf` classification after *every* entity's `Simulate`, on every tick, in every build,
   with no gate at all. On a scene with a few thousand marks that is a few thousand type-hierarchy
   walks a tick. Measured: **0.24 ms** (`sim:step` 6.29 on, 6.05 off). Real, worth gating, not the
   answer.
2. **"The marks are in `_animals` and `MoveOutAndDelete` walks them four times."** Three new
   sub-stage brackets say otherwise: `slow.animals` is 0.18 ms and `slow.moveout` 0.14. Marks live
   in `_vehicles`.

## The measurement that settles it

Everon `perf_combat`, noon, one run per arm, accounting on:

| | marks (TTL 10 s) | no marks (TTL 0) |
|---|---:|---:|
| `slow.vehicles` | **4.78 ms** | **2.25 ms** |
| `slow.animals` | 0.18 | 0.005 |
| `slow.moveout` | 0.14 | 0.003 |
| by kind: `other` (the marks themselves) | 0.213 ms over **2,482 calls** | 0.008 over 56 |
| by kind: **`man`** | **4.771 ms over 193 calls** | **2.236 ms over 193 calls** |

**Read the last row twice.** The same 193 soldiers, simulated the same number of times, cost
**24.7 µs each with footprints present and 11.6 µs without.** The marks' own simulation is
0.086 µs each — they are nearly free to tick. What they do is make *somebody else's* work twice
as expensive.

That also corrects PERF-023's headline: "18 µs per soldier per tick" was measured with footprints
in the world, so it was never a property of the soldier.

## What it means

A soldier's tick contains a query whose cost scales with the number of entities near it — a
neighbourhood or landscape-grid scan — and 2,500 decal marks are sitting in the list it walks.
They are `Offset::Decal`: they do not collide, cannot be targeted and cannot be shot. They have no
business in a soldier's neighbour scan.

The lever is therefore **not** fewer footprints and **not** shorter-lived ones — both change what
the player sees, for a cost that is not theirs. It is to keep marks out of whatever structure
`Person::Simulate` queries. Naming that query is the next step and is not done here.

Note the frame did not move on this scene (21.3 vs 21.2): `perf_combat` is GPU-bound and `swap`
absorbs it, exactly as in PERF-026. The prize is real on a CPU-bound pose, which is where this
must be re-measured before anything is claimed for it.

## Shipped here

- `POSEIDON_SIM_VEHICLE_COST=1` gates the by-kind accounting, **off by default** — it used to be
  unconditional, and it costs 0.24 ms. The classification is also memoised per `EntityType` now.
- Three sub-stage rows: `slow.moveout`, `slow.vehicles`, `slow.animals`.
