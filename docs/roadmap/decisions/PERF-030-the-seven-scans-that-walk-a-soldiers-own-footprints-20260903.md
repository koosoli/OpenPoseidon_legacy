# PERF-030 — The seven cell scans that walk a soldier's own footprints

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-027 → PERF-029 (the effect, three eliminations, and the even split)

## Why PERF-029's "no single query" was half wrong

PERF-029 measured the extra cost split evenly between `man.pilot` (+0.62 ms) and `man.base`
(+0.61) and concluded there is no single query. The split is right; the conclusion was too strong.
There is no single query because **there are seven**, and they sit on both sides of that split.

The enabling fact, which nothing in this project had written down: footprint `Mark`s go into the
**landscape cell index**, not merely a side list. `SoldierOldMove.cpp:236` → `World::AddAnimal` →
`VehiclesDistributed::Add` → `WorldSetup.cpp:707` → `Landscape::AddObject` → `_objects(x,z)`.
`ObjectListFull` is a flat `SmallArray<Ref<Object>>` with **no static/dynamic partition**, so
every cell scan below walks them.

## The map

Per-tick unless noted. "Reject" is what each mark costs before the loop moves on.

| # | scan | file:line (loop) | when | reject cost per mark |
|---|---|---|---|---|
| 1 | `AILocker::LockPosition` | `AIUnit.cpp:190` | whenever the soldier advances 0.5 m; **50 m + 25 m** → up to 4×4 cells | 1 virtual (`GetIPaths`) |
| 2 | `Landscape::PredictCollision` | `Collisions.cpp:1398` | every tick while moving | 1 inline `GetType()` — **but see below** |
| 3 | `Landscape::GroundCollision` | `Collisions.cpp:704` | every tick while animating (`SoldierOldSim.cpp:531`) | shape deref + `FindRoadwayLevel()` |
| 4 | `Landscape::ObjectCollision` (frame) | `Collisions.cpp:169` | every tick, moving non-steady soldier (`SoldierOldSim.cpp:726`) | up to 4 inline `GetType()` |
| 5 | `SurroundTracker::Update` | `EnvTracker.cpp:35` | on the sensor cadence, every 5 m moved; 50 m + 25 | 4 `GetType()` + `Static()` |
| 6 | `EntityAI::FindHideBehind` | `TargetFire.cpp:785` | non-leaders, once per 5 s | 4 `GetType()` |
| 7 | `Landscape::CheckVisibility` | `Collisions.cpp:1500` | only with a live fire target, cached 0.3 s | shape deref + **virtual** occlusion + `GetType()` |
| — | `CheckNearFriendlies` | `VehicleAICombat.cpp:1497` | firing only | distance² + **`dyn_cast`** |

Items 1 and 3–4 alone put scans in both halves of the tick, which is the even split, measured.

**Item 2 is the one to fix first.** It skips a cell entirely when `GetNonStaticCount() <= 0`.
Marks are non-static, so a cell that gained footprints **loses that skip and is then walked in
full, static objects included** — on Everon that is trees and bushes by the hundred. It is the only
site where marks cause previously-skipped work rather than extra iterations.

In every case the object pointer, its vtable and often its `LODShape` are pulled into cache per
mark. The cost is cache misses across the cell array, not the compares.

## What was tried and measured here

`Mark` has asked for a coarse simulation rate since 2001 — `SetSimulationPrecision(0.5)`,
commented "twice per second is enough" — and **the engine has been ignoring it**:
`Entity::SimulateOptimized` throttles, and then the `SimulateRest` pass runs every tick on the
leftover time, so a mark's `Simulate` ran 60 times a second regardless.

`POSEIDON_REST_HONOURS_PRECISION=1` makes `SimulateRest` honour a coarse (≥ 0.25 s) declared
precision. Measured: the marks' own cost halves (`other` 0.124 → 0.064–0.081 ms/tick) — the
throttle works — and **the soldiers do not get any faster** (`man` 3.59 vs 3.58, `sim:step`
unchanged).

That is a useful negative: it confirms the cost is on the **scanning** side, not in the marks
ticking. It also decides the default — **off**. It buys no frame time, and stepping a ten-second
fade at 0.5 s is a look risk. A look risk with no measured win does not ship.

## Next

Restore item 2's cell skip. The obvious implementation — cache a per-cell count of `TypeVehicle`
objects — has a hazard worth stating: `Object::SetType` is a plain inline setter with 14 call
sites, several of which change an object's type **while it is in a cell**, so a cached
type-dependent count can silently go stale and a skipped cell means a missed collision
prediction. Either hook `SetType` or find a partition that does not depend on a mutable field.
