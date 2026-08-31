# SIM-803 — One AI ordering ranked by heap address, and its comparator had a cycle (2026-08-31)

Roadmap **Phase 8.3, "Stable iteration and reductions"**. Branch `sim-803-stable-iteration`,
based on `safety/asset-obs-001`.
**Related:** [SIM-801](SIM-801-worker-fp-environment-20260831.md) (worker FP environment),
Phase 8.2 (RNG ownership — the positional/sequential split in `engine/Random/randomGen.hpp`).

## The finding

Phase 8.3 says a gameplay-observable winner must not depend on hash/container iteration order,
pointer addresses, unordered float reductions, or job completion order. A survey of every
best-so-far loop, sort and reduction in `AI/`, `World/Detection/`, `World/Simulation/`,
`World/Entities/` and the SQF nearest-object commands found **the good news first**: this engine
is almost entirely index-ordered. `AutoArray`, `RefArray`, `OLinkArray` and the fixed
`_units[MAX_UNITS_PER_GROUP]` array are what AI iterates; the handful of `std::unordered_map`s
that exist in these trees are lookup caches that are never iterated for a decision. There is no
hash-order problem to fix here.

There were three real defects, and they were in the sorts, not the scans.

**1. `AICenterImpl.cpp` ranked guarded vehicles by heap address.** `CmpGuardedVehicles` ended:

```cpp
return info1->_idExact - info2->_idExact;   // EntityAI* minus EntityAI*
```

`_idExact` is `LLink<EntityAI>`, which has `operator Type*()`, so this is a pointer subtraction.
`AICenter::UpdateGuarding` step 5 hands the sorted list to guarding groups in order, so this
decided **which group is sent to guard which vehicle** — from the allocator. The tie branch is
not exotic: the key above it is `_type->GetCost()`, a flat per-type constant, so any two vehicles
of the same type tie exactly, bit for bit, every time.

**2. That same expression is not even a valid ordering.** The subtraction is a 64-bit `ptrdiff_t`
narrowed to the `int` return type. Three objects at increasing addresses spanning slightly over
2 GiB produce A < B, B < C, **C < A** — a cycle, because only the A-to-C difference loses its
sign to the truncation. Measured in `test_stable_ordering.cpp`:

| pair | true separation | `(int)(ptrdiff_t)` | comparator claims |
|---|---:|---:|---|
| A → B | +1.5 GiB | −1610612736 | A before B |
| B → C | +1.5 GiB | −1610612736 | B before C |
| A → C | +3.0 GiB | **+1073741824** | **C before A** |

A 64-bit process with a large heap reaches those separations without trying.

**3. `AIGroup::CompUnits` claimed `a > b` and `b > a` for the same pair.** It opened with
`if (!u1) return 1;` before testing `u2`, so **two empty slots compared as 1 in both directions**.
`_units` is a fixed 12-slot array that is normally mostly empty, so the two-nulls pair is the
common case, not an edge case. `SortUnits` feeds this to `QSort` and its result decides formation
slots and leadership succession.

**4. `AIGroup::CreateTargetList` sorted the group target list with no tie-break at all.**
`BetterTarget` returned `sign(t1->subjectiveCost - t0->subjectiveCost)` — 0 on a tie — into
`QSort`. `QSort` (`Foundation/Algorithms/Qsort.hpp`) is an unstable quicksort, and its ≤8-element
`ShortSort` path is a **selection sort**, also unstable; there is no array size at which ties are
preserved. `AIGroup::GetSubjectiveCost` collapses to the type's flat `GetCost()` for any target
that cannot currently fire back, so identical unit types tie exactly every think cycle, and
`AssignTargets` / `SelectInterestingTarget` walk this array in order to pick what to engage.

Unlike (1)–(3), (4) is *reproducible today* — it is deterministic as long as the array arrives in
a deterministic order. It is on this list because that is precisely the assumption Phase 8 removes
when detection starts producing targets from worker jobs that complete in scheduler order.

## The audit table

Order column: **index** = iterating an index-ordered array, stable across runs today.
Observable = does a player-visible outcome depend on which of two tied candidates wins.

| # | Site | Iterates | Order today | Tie possible | Verdict |
|---|---|---|---|---|---|
| 1 | `AICenterImpl.cpp:295` `CmpGuardedVehicles` | `AutoArray<const AITargetInfo*>` | index, **sorted by pointer** | constant (flat type cost) | **FIXED** |
| 2 | `AIGroup.cpp:1077` `CompUnits` | `_units[12]` | index | constant (empty slots) | **FIXED** — inconsistent comparator |
| 3 | `AIGroupImpl.cpp:411` `BetterTarget` | `TargetList` (`RefArray<Target>`) | index, **unstable sort** | constant (flat type cost) | **FIXED** |
| 4 | `AIGroupImpl.cpp:1089/1115` `AssignTargets` | `_units[12]` | index | plausible (identical units, equal range) | stable, first-wins by unit ID — left |
| 5 | `AIUnitImpl.cpp:1204` `SelectInterestingTarget` | `TargetList` | index | plausible | stable once #3 is fixed — left |
| 6 | `AIGroup.cpp:1028/1037` `LeaderCandidate` | `UnitWithID(1..12)` | index | likely (all-private group, equal XP) | stable, first-wins by unit ID — left |
| 7 | `AISubgroup.cpp:1332/1341` subgroup leader | `GetUnit(0..NUnits)` | index | likely | stable, first-wins — left |
| 8 | `AIGroup.cpp:1144` `SortUnits` | `_units[12]` | index | — | already ends in `u1->ID() - u2->ID()`; the model tie-break |
| 9 | `AICenter.cpp:781` `CmpMPTableRows` | `_tableMP` | index | likely (equal kills) | already ends in `stricmp(player)`; the model tie-break |
| 10 | `AICenterImpl.cpp:407/438` guard assignment | `_guardingGroups` | index | plausible (equal distance) | stable, first-wins — left |
| 11 | `AICenterStats.cpp:1520` support assignment | `_supportGroups` / `_supportTargets` | index | plausible | stable, first-wins — left |
| 12 | `AIGroupImplHealth.cpp:809` supply unit | `OLinkArray<AIUnit>` | index | plausible | stable, first-wins — left |
| 13 | `AISubgroup.cpp:2526` `FindNearestSafer` | hand-unrolled ring scan | fixed traversal | **very likely** (flat terrain, equal exposure) | stable, first-wins by ring position — left |
| 14 | `AIRadioImpl.cpp:2086` `FindNearestMoveObject` | landscape cell grid → `ObjectList` | index (x then z) | plausible | stable **given** cell contents — see caveat |
| 15 | `GameStateExtUi.cpp:509` SQF `nearestObject` | landscape cell grid → `ObjectList` | index | plausible | stable given cell contents — see caveat |
| 16 | `GameStateExtUi.cpp:407` SQF `nearestBuilding` | `GWorld->GetBuilding(i)` | index | plausible | stable, first-wins — left |
| 17 | `VehicleAICombat.cpp:594/618`, `VehicleAIPilot.cpp:1211` magazine picks | `GetMagazine(0..N)` | index | likely (identical magazines) | stable, first-wins by slot — left |
| 18 | `VehicleAI.cpp:1685` `CmpMagazines` | `conflict` array | index, unstable sort | possible | left — rearm bookkeeping, and the result is de-duplicated immediately after |
| 19 | `TargetFire.cpp:1045/1268` `BestFireResult`/`WhatFireResult` | `NMagazineSlots()` | index | likely (equal surplus) | stable, first-wins by slot — left |
| 20 | `SoldierOldSimProxy.cpp:1117` `CompBlendAnim` | config-derived array | index, load-time | possible | left — load-time, int key from config |
| 21 | `ArcadeTemplateFind.cpp:454` group join | `groups` | index | plausible | **`<=`, last-wins** — editor-only, left |
| 22 | `AIGroup.cpp:1006` vehicle insert | vehicle list | index | plausible | **`>=`, later-wins** — left, ordering is cosmetic |
| 23 | `ArcadeTemplate.cpp:1551` `SelectLeader` | `gInfo.units` | index | likely (equal rank) | editor-only, left |
| 24 | `AICenterImpl.cpp:675` `FindStartPosition` | 100 RNG candidates | RNG stream | unlikely (float cost) | left — determinism here is Phase 8.2's, not 8.3's |

### The two caveats worth writing down (#14, #15)

Both nearest-object scans walk `GLandscape->GetObjects(z, x)` and take the first strictly-closer
hit, so they are deterministic **given the contents and order of each landscape cell's
`ObjectList`**. That is a Terrain-side property (`engine/Poseidon/World/Terrain/`), which this
change deliberately does not touch. If object streaming ever reorders a cell's list on insert or
eviction, these two become order-dependent with no warning and no assertion — the SQF one is
directly scriptable by mission makers. Recorded, not fixed.

## Float reductions

Every float reduction found in these trees sums over an index-ordered array with a fixed count:
`AIGroupImpl.cpp:1309-1323` (group damage-per-minute), `AIGroupImplHealth.cpp:1171-1184` (group
cost), `:1516-1531` (group strength), `AISubgroup.cpp:2388`, `ArcadeTemplate.cpp` centroids.
**None of them is order-unstable today**, so per the brief none was changed — reassociating a sum
that is already deterministic buys nothing and changes gameplay numbers. They become a problem
only if the units they sum over are ever accumulated from parallel jobs, at which point the fix is
to reduce in unit-ID order, not to add a compensated sum.

## What changed

| File | Change |
|---|---|
| `AI/AICenterImpl.cpp` | `CmpGuardedVehicles` → `CompareGuardedVehicleRank` (pure, no pointer term) + `GuardedVehicleBefore`; `QSort` → `std::stable_sort` |
| `AI/AICenter.hpp` | declares both, with the reasoning |
| `AI/AIGroup.cpp` | `CompUnits`: two null slots now compare equal |
| `AI/AIGroupImpl.cpp` | `BetterTarget` + `QSort` → `_targetList.SortBySubjectiveCost()` |
| `AI/VehicleAI.hpp` | declares `TargetList::SortBySubjectiveCost`, with the reasoning |
| `World/Detection/Target.cpp` | implements it as a `std::stable_sort` |
| `tests/.../AI/test_stable_ordering.cpp` | new; 215 assertions in 4 cases |

The tie-break in both fixed sorts is **the element's index in the ordered array it came from** —
an identity that already exists at both sites, obtained by making the sort stable rather than by
adding state. That was a deliberate choice over the two alternatives:

- **`Object::ID()` is not usable.** `WorldImpl.cpp:1772+` assigns `-1` to every dynamically
  created vehicle, so it is not unique for AI entities.
- **`NetworkId` is not usable.** It is only ever assigned by the network layer
  (`NetworkClientActions.cpp:150`) and by serialization, so in single-player it is null for
  everything.

No algorithm was rewritten. Both sorts produce the same ranking they always did; only the
resolution of ties changed.

## What was deliberately NOT done

- **The 21 "stable, first-wins" sites were left alone.** They iterate index-ordered arrays with
  strict comparisons, which is already a deterministic tie-break — the lowest index wins. Adding
  an explicit ID comparison to each would be churn that changes no behaviour and creates 21 merge
  conflicts for the next person in this tree.
- **#21 and #22 (`<=` / `>=`, last-wins) were left.** Both are deterministic; one is mission-editor
  code and the other only affects the display order of a vehicle list. Unstable-*looking* is not
  the same as unstable, and the brief asked for honesty about that.
- **No float reduction was reassociated.** See above.
- **`QSort` was not replaced globally.** Only the two comparators that were actually ambiguous were
  touched. `QSort` remains correct wherever the comparator is a total order — `CompUnits` and
  `CmpMPTableRows` are the two in this tree that already were, and they are cited above as the
  pattern to copy.
- **Nothing in `World/Terrain/`** — the two nearest-object caveats are recorded, not fixed, because
  that tree is owned elsewhere right now.
- **No `CompUnits` unit test.** Constructing an `AIUnit` with a `Person` and a rank needs a live
  world and game data; the fix is a three-line null-symmetry correction reviewed by inspection. The
  antisymmetry property it now satisfies *is* asserted exhaustively for
  `CompareGuardedVehicleRank`, which is the same class of bug in a testable place.
- **The game was not launched.** Another agent holds the single allowed instance. Verification here
  is build + unit tests only, and no behavioural capture of AI target selection was taken. Anyone
  wanting one should run a guarded-vehicle mission twice and diff the assignments.

## Evidence

Build: `win-x64-clang-rwdi`, targets `PoseidonGame` + `PoseidonTests`, clean.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| baseline (`safety/asset-obs-001`, `aa2eea3e`) | 2964 | 234,902 | 0 |
| after this change | 2968 | 235,117 | 0 |

Difference is exactly the 4 new cases / 215 new assertions.
