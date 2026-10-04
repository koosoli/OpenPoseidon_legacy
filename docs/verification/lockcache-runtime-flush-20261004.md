# Live AI locks during terrain cache flush

Base: `793d7b96`. This is a bounded source/native investigation of the
`Cannot unlock field that was not locked` warning at `LockCache.cpp:279`.
It does not establish the separate corpse ragdoll failure's cause.

## Defect and repair

`AILocker` acquires one `LockField` reference per field, shares that reference
between normal and temporary movement locks, and releases it after both cell
lock sets are removed. That normal sequence is balanced.

`Landscape::FlushCache` unconditionally replaced the lock cache, destroying
fields while live lockers retained raw pointers to them. Runtime crater
creation (`RocketTerrainImpact` / `PaintTerrain`), cave editing and terrain
restore call this function with units alive. A later locker could read freed
field coordinates and then release them against the replacement empty cache,
which is the exact warning path. This is a concrete lifetime defect; the
reported stress mission's individual warning occurrences were not replayed.

The repair retains nonempty AI occupancy during cache flushing. Operational,
terrain segment and shadow caches still refresh. Once all field owners release
their references, the empty lock cache can be recreated, including during
normal world loading/resizing after units have been removed.

## Native verification

The existing Landscape test file now contains
`Terrain cache flush preserves live AI lock references` (`[lock-cache]`).
It acquires 63 references to one field plus a neighboring field in the same
coarse bucket, performs four flushes, verifies both pointer identities and
cell occupancy, then releases all 64 references and verifies the cache is
empty. It also flushes again after all owners have released.

- Before repair: failure at the first flush's cache identity check;
  66 assertions passed before the expected failed assertion.
- After repair: all 153 assertions passed.
- Related `[ObjectID]` and `[heightmap-view]` tests, together with the new
  regression: all 1,200 assertions passed in five cases.
- `git diff --check`: passed.

These results came from private native executables in this worktree's ignored
`build/lockcache-probe` directory. Only the modified test translation unit and
fixed `Landscape.cpp` were freshly compiled; other dependencies came from
the root agent's warm Windows build cache. The pre-repair executable used
the existing production Landscape implementation. Parent cache outputs were
read only, and no installed game was started, stopped or modified.

Installed-game acceptance remains with the parent agent: create a rocket
crater or edit terrain with AI present, let units move/unlock, and check that
the warning no longer occurs. The unrelated existing four-bit cell lock
saturation and corpse ragdoll admission budget were outside this repair.
