# PERF-031 — The cell skip is restored, correctly, and it buys nothing

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-030 (the seven scans; item 2 named as "the one to fix first")

## What changed

`ObjectListFull` keeps a second incremental count, `_nTypeVehicle`, beside `_nNonStatic`,
maintained at the same three sites (`Add`, `Delete`, `Clear`) with the same debug-only O(n)
self-check. `Landscape::PredictCollision` now skips a cell on `GetTypeVehicleCount() <= 0` — which
is exactly the filter its own loop applies two lines later, where `GetNonStaticCount()` was only
ever a proxy for it, and stopped being a fair one when footprint decals started living in cells.

The invariant is "an object's type does not change while it is indexed". A design pass over all 14
`Object::SetType` call sites found exactly one that could break it —
`World::MoveOutAndDelete`'s convert-to-object branch, which changed the type without removing the
object from the landscape index, unlike the two branches above it. That branch is currently dead
(`SetConvertToObject` has no callers), and it now removes and re-adds around the change, so the
invariant is enforced rather than assumed.

## Measured: nothing

Everon `perf_combat`, noon, `POSEIDON_SIM_VEHICLE_COST=1`:

| | before | after |
|---|---:|---:|
| `man` ms/tick, marks alive | 3.59–3.63 | 3.709 |
| `sim:step`, marks alive | 6.1 | 6.15 |
| `sim:step`, no marks | 3.91 | 3.94 |

Inside the run-to-run spread PERF-028 documented for this figure. **`PredictCollision` was not
where the footprint cost lives**, which closes item 2 of PERF-030's map — the one it called "the
one to fix first" on the strength of the defeated early-out.

Why the reasoning was wrong is worth keeping: the skip only fires for a cell with *no* non-static
objects, and on a scene with 193 soldiers walking about, the cells near a soldier already held a
non-static object — the soldier. The marks defeated a skip that mostly was not firing anyway.

## Kept anyway

It ships. It costs four bytes per non-empty cell and one branch per insert/remove, it replaces a
proxy with the real predicate, and it closed a latent index bug on the way. But it is recorded
here as **no measured win**, so nobody later reads the diff and assumes it bought the millisecond.

Full suite after: 3,061 tests, two failures, both pre-existing (the physics worker readback and
the audio suite).

## What is left of PERF-030

Five scans, none of them yet ablated: `AILocker::LockPosition` (50 m + 25, whenever the soldier
advances half a metre), `GroundCollision`, `ObjectCollision`, `SurroundTracker`, `FindHideBehind`.
The next measurement should ablate the largest of them the way `POSEIDON_ABLATE_ROAD_SURFACE` did
— one env-gated arm per scan — rather than reason about which one it is. Three careful readings of
the source have now been wrong, and two designs derived from them bought nothing.
