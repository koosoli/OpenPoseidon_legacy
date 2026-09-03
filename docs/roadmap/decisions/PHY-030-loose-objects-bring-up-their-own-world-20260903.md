# PHY-030 — Loose objects bring up their own physics world; the switch now does something

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Asked for by:** the owner — props the player can bump, knock over or drive around, "at least a
few static objects to test".

## What was already there, and why it did nothing

Nearly all of it existed:

- `LooseObjects::Step` is called from `Thing::Simulate` and hands a prop to the solver, reading
  its pose back — the one place in the engine that reads out of physics.
- The player's kinematic capsule already follows the player **every physics tick**, from
  `World.cpp` (`UpdatePhysicsPlayerProxy`, on by default), so shoving was wired.
- `tests/perf/missions/phys_things.noe` already places six props — two barrels, a barrel stack,
  two pallets and a table.
- A dev-panel checkbox and `POSEIDON_LOOSE_OBJECTS` already existed.

And with all of it switched on, **nothing happened.** `Step` bailed twice, silently: first on
`!world->IsCreated()`, then on `!stats.terrainRegistered`. Nothing in the loose-object path ever
created a physics world or gave it a ground — both were side effects of the dev **probe** tool.
The feature's only working configuration was another feature's setup, and the symptom was a
barrel sitting there behaving like 2001.

## What changed

`TerrainBroughtUp()` in `LooseObjects.cpp`: on first use with the feature on, create the backend
and register the landscape as a height field, subdivided 4× (12.5 m spacing — the same reason
`PhysicsCorpus` subdivides: a straight chord across the 50 m landscape grid cuts a hillside's
corner by metres and an object sinks into a slope it should rest on). Tried once and remembered,
so a refusal cannot re-scan the island every tick.

`phys_things.noe` now faces azimuth 45 instead of 320. The props are 4–8 m north-east of the
player and the shipped heading pointed away from them, so the test mission's own screenshot
showed empty grass.

## Measured (Everon, `phys_things.noe`, noon)

```
LOOSEOBJ: terrain 1021x1021 at 12.50 m -> registered
LOOSEOBJ: ... barel1 at [2406.00 129.50 9744.00] moving 0.16 m/s, awake
... six objects, one line each
Sim stage ms/tick: ... physics=0.005
```

Six props owned by the solver at **0.005 ms per tick**, resting upright on the ground in the
capture — not sunk, not floating.

## Known rough edge, not fixed here

**They never sleep.** Four seconds after settling the objects still report 0.10–0.21 m/s and
`awake`, and drift about 2 cm from where they were placed. It is invisible at this scale but it
is a solver-tuning question (sleep thresholds against a 30 kg body at 12.5 m terrain spacing),
and it is the first thing to look at before this is considered for shipping.

## Still off by default

Unchanged, and for the reasons the original header gives: multiplayer ownership and what a moved
barrel does to a saved game are both open. This decision makes the switch **work**; it does not
argue that it should be on.

**To try it:** dev panel → Physics → the loose-objects checkbox, then walk into the props in
`phys_things.noe`. `POSEIDON_LOOSE_OBJECTS=1` does the same from a capture run.
