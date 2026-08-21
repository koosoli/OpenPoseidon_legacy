# HANDOVER — smoke containment: rooms and portals (DESIGN ONLY, no code)

Written 2026-08-18. Canonical copy of the plan; it previously existed only as a published
artifact, which a session starting cold would never find.

## 0. BRANCH — READ FIRST

**Branch `feat/wgpu-decal-billboards`, remote `safety`.**

```bash
git fetch safety
git worktree add .claude/worktrees/<name> -b <yourbranch> safety/feat/wgpu-decal-billboards
```

All smoke/fire/grass work is ONLY there (it contains `feat/texture-streaming` as an
ancestor). Branching from `new-renderer-infrastructure` or
`codex/wgpu-performance-modernisation` silently loses it. Never edit the repo-root
checkout. `.agents/README.md` Rule 0: read `DEPLOYED-FROM.txt` before deploying, write it
after, copy BOTH binaries.

## 1. Why the current model cannot do it

Collision is **reactive**: a sphere swept against fire geometry, pushed back on contact.
`contain` (2e9e875a) sweeps every tick and never lets a particle cross a surface it came
from behind, plus the drawn puff is clamped to its distance from the contact plane. Good,
never complete: a puff is 3-5 m, a wall 0.3 m, a soft billboard has no hard edge, and some
fire geometry has gaps.

Needed instead: **proactive**. Not "stop when you hit a wall" but "you are in room A; the
only ways out are portal 1 and portal 2". Then a particle cannot leave except through an
opening, by construction, and the wall never has to be hit.

## 2. The finding that makes it tractable

Every enterable building already stores its doors — it is how the AI walks in.

| data | where | gives |
|---|---|---|
| doors | `BuildingType::_exits`, named points `In1..InN` in the Paths LOD (`House.cpp:63`) | world-space door positions |
| rooms (implicit) | `_positions` (`Pos1..PosN`) + `_connections` (`House.cpp:76,90`) | rooms = connected clusters; doors where a cluster meets an `In` node |
| point-in-building | `Landscape::IsInside` (`Collisions.cpp:1932`) | already used by the AI |
| walls | Fire Geometry LOD (`ObjIntersectFire`) | the solid shell |
| windows (implicit) | Fire Geometry vs **View Geometry** (`ObjIntersectView`) | passes fire but blocked by view = glass; both pass = an opening |
| floors | Roadway LOD, `Landscape::RoadSurfaceY` | storey floors |
| door graph | `IPaths` (`House.hpp:104`) via `Object::GetIPaths()` | `FindNearestExit`, `SearchPath` |

Not stored: explicit room volumes and window lists. Both derivable.

## 3. Design

Sim-side `BuildingInterior`, cached per **BuildingType** (shared by every instance;
placement transforms the lookup), answered through `ISmokeWorldQuery`:

* `RoomOf(pos)` -> room id or outside
* `Portals(roomId)` -> {centre, normal, radius, kind: door|window|breach}
* `ContainedStep(from, to, roomId)` -> where the particle may actually reach

Rooms: flood-fill the Paths graph; a component not separated by an `In` node is a room.
Volume: voxelise the bbox at 0.5 m against fire geometry, label each cell by the nearest
room node reachable without crossing solid (~15k cells for a 20x12x8 m house, one byte
each). Portals: doors are the `In` nodes projected onto the shell; windows from a one-time
ray scan out of each room centroid, clustering rays that leave fire geometry into open air
between floor and ceiling and are not blocked by view geometry; breaches re-run the scan on
destruction.

**Cost:** all geometry work per-building, once. Per particle it is a voxel lookup plus a
portal test. Outdoors costs nothing — `RoomOf` misses on the first cell.

## 4. Phases (each ends with something visible)

1. **Room ids from the Paths graph** — no voxels; use for the shelter test + logging. Half a
   day. Log room counts per building type on Everon/Chernarus/Stratis; expect 1-6.
2. **Room volumes** — voxelise + label + cache. One day. **Build the dev overlay that draws
   room labels as coloured points and KEEP it — most valuable tool in the plan.**
3. **Portals** — doors (named, trivial) then the fire/view window scan. One day.
4. **Containment step** — `ContainedStep` replaces the sweep in-room; a particle leaves only
   via a portal, taking its normal plus a pressure boost so smoke pours from a doorway.
   Draw radius clamps to the room inscribed radius. One day. This is where it shows.
5. **Room pressure** (optional) — per-room fill; above a threshold new particles bias toward
   the nearest portal. Half a day. Makes a burning house pour from every window.

Verification: overlay; `POSEIDON_TEST_SMOKE_HOUSE=1` already spawns inside the nearest
enterable building (add the room id to its log line), photograph from outside at 30 s —
no smoke except at portals; counter for particles-that-crossed-a-wall (must be zero).

**Risk:** only as good as the fire geometry. Buildings with gappy fire LODs will leak, now
as a design-time fact. Phase 2 logs any room whose labelling reaches "outside" without
passing a portal — which doubles as a fire-geometry QA tool the map team does not have.

**Also buys:** a real "which room am I in" for AI indoor awareness and audio occlusion, and
room-fill as a gameplay quantity (FX-130 coarse server-authoritative smoke without any
volumetric rendering).

**Not in scope:** volumetric grid smoke; inter-room pressure beyond phase 5; vehicle interiors.

Total ~4 days for phases 1-4.

## 5. RAIN — the same query, build it once

Roadmap `WTR-245` (Precipitation: occluded rain, wet surfaces) is REQUESTED, NOT STARTED
and wants a real particle rain system that does not fall indoors. That is the **same
question**: is this point under cover, and where are the openings.

* **Today, free:** `ISmokeWorldQuery::IsSheltered(pos, probeHeight)` already exists and is
  exactly a roof test. Rain can use it now with none of the work above.
* **Proper:** `RoomOf(pos)` beats an upward ray — it does not read a bridge or tree canopy as
  a building, and it knows the storey.
* **Rain through a hole in the roof or an open window** is portals again, used in the other
  direction: rain enters where smoke leaves.

Build it once behind the query boundary (promote to a shared `IWorldShelterQuery` if rain
lands first). **Do not let rain grow a private copy of this.**

## 6. Traps

* `Object::Intersect(beg, end, radius, ...)` **ignores radius** — zero-width ray. Build a
  sphere sweep from a ray bundle or edges leak.
* `CollisionInfo::pos`/`dirOut` are in the hit object MODEL space.
* On the beg/end path `dirOut` is the penetration chord, not a normal (the engine says so).
  Use `CollisionInfo::surfaceNormal`.
* `SmokeSourceVehicle` detonates on its first tick unless `_explosionTime` is pushed out.
* Verify a render path by what reaches the screen, never by counting calls into it — the
  `DrawDecal` gap cost hours because the submission counters looked healthy.
