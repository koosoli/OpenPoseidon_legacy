# ACTIVE HANDOVER — physics, and the fixed step it stands on

**This file is live. Whoever works on this updates it in the same commit as the
code.**

| | |
|---|---|
| **Branch** | `consolidated-v2` — TRUNK. The physics branch was merged 2026-08-28 (fast-forward, 39 commits) and `codex/arch-001-fixed-step` is now history. |
| **Plan of record** | `docs/roadmap/decisions/PHY-001-what-to-port-from-the-fork-20260828.md`, ARCH-001 item **C** |
| **Deployed** | yes, from trunk |

## Done, with numbers

**Fixed step (ARCH-001 C) — COMPLETE.** World clock, weather, wind, AI, vehicles,
visibility AND scripts run at a fixed 1/60 s. Mission timing no longer depends on
frame rate, which is the prerequisite for deterministic multiplayer and replay. Measured across four play sessions: 1.03–1.52
ticks per frame, so the SIMULATION IS THE FASTER CLOCK on this machine and entity
interpolation was cancelled rather than built — it only pays when rendering
outruns simulation.

**PHY-010.** Stock Everon: 103,115 instances seen, 82,381 registered, 151,038
shapes, terrain 3061×3061 at 4.17 m. **0 degenerate components, 0 hull
refusals.** All 32 failing models are roads and ground clutter, which carry no
Geometry LOD by design. Max ConvexComponent points 53 against a cap of 64.

**Cost.** 0.022–0.040 ms mean per step, 2.4–3.2 ms worst, at 82,384 bodies. About
0.03 ms of a 16–20 ms frame: physics does not appear in the budget at this scale.

**PHY-020.** Sphere, capsule, box, cylinder and real MODELS as dynamic probes,
drawn solid, placed on the ground facing the camera, thrown, shot at (impulse at
the hit point, so off-centre hits spin things), pushed by a kinematic player
proxy, and blown by Poseidon's own wind through `b3Shape_ApplyWind`.

**PHY-020 ACCEPTED BY THE OWNER, and on the exact test PHY-001 named.** Balls
spawned inside a house came down the stairs properly. Building interiors and
stairs are the hardest case for a converted collider set -- a hull that is
slightly too large or too small shows there first, as a ball that floats above
the treads or falls through them. This is owner ground truth from play, which
outranks any capture I ran.

**PHY-030, first measurement (owner, live fire).**

    1793 segments | agree hit 72 | agree miss 1698
    legacy hit, physics missed:  0
    physics hit, legacy missed: 23

**Zero legacy-only.** Not one case where the game collided with geometry the
physics world does not have. The 23 the other way are expected: the physics world
holds things the 2001 path cannot see -- the dev probes, which are deliberately
absent from it, and the terrain, which the projectile path tests separately.

Read the 72 HITS, not the 1793 segments: the 1698 mutual misses are air and prove
only that neither system sees anything there. 72 hits is good evidence and not
proof across 82,381 objects. Firing at varied object classes -- trees, rocks,
fences, wrecks -- is what would harden it.

## OPEN AND UNSOLVED: bullets do not go through a wooden gate

Built, wired, switched on in the Physics tab, and it still does not work. The
owner has now reported this twice, and the second report was AFTER the fix I
thought would settle it. Do not assume the remaining cause is the same one.

What is known:

* The material table matched Czech material words (`drevo`, `beton`) in texture
  paths. Stock OFP textures are named after the OBJECT -- `hotel_door1_ca.paa`,
  `dvere_int3.paa` -- so almost nothing matched and everything fell to the
  unknown default, which resists like concrete deliberately.
* PENMISS logging was added for exactly this: every texture with no material logs
  once with its full path. **Nobody has read that log yet.** That is the next
  step and it is cheap -- shoot a gate, grep the log for PENMISS, and the real
  texture names are there to build the table from.

Candidate causes NOT yet excluded, roughly in order:

1. The table still does not match -- read PENMISS first.
2. `MeasureThickness` finds no far face. It casts backwards from 2 m beyond the
   entry; a gate modelled as a single-sided plane has no far face at all, and the
   honest answer for "not found" is "stopped". A whole class of OFP geometry is
   single-sided.
3. The legacy path deletes the round before `TryPenetrate` is reached on some
   branch -- there are three `ObjectCollision` call sites in Shots.cpp and only
   one is wired.
4. World colliders were never registered in that session, so there is nothing to
   measure a thickness against.

Cause 2 is the one I would look at first after the log, because it would be
invisible from outside and would make penetration fail for exactly the doors,
fences and gates people try first.

## What is open

1. **The ray audit has no data.** It reports 0 segments — nothing fires in a
   free-fly capture and there is no headless way to make the AI shoot. Tick
   "Compare physics rays" in the Physics tab, empty a magazine into a wall, read
   `legacy-only`. That counter is the only thing that can say whether 82,381
   objects converted to the right SHAPE, as opposed to merely converting.
2. ~~Fixed-step scripts~~ **DONE 2026-08-29.** ON by default after the owner
   played with it and reported no difference. `POSEIDON_FIXEDSTEP_SCRIPTS=0`
   still turns it off: one playtest is evidence, not proof, and mission pacing is
   the kind of thing that surfaces in an odd mission weeks later.
   **ARCH-001 item C is now complete.**
3. **Cosmetic, owner has deprioritised all three:** solid probes render white and
   black because `SetConstantColor` does not reach the shader; the cylinder wall
   has gaps; a config-spawned jeep draws white boxes, established to be its
   unresolved crew proxies.
4. **PHY-040 (ragdoll)** needs a skeleton hierarchy that does not exist: RTM
   stores a matrix per NAMED bone but as a flat list with no parent/child
   relation. ~30 bones with parent, joint type and limits. No help from the fork —
   checked: its `CreateD6Joint` is never called and its skinned model carries no
   hierarchy either.
5. **PHY-050 (real vehicle ownership)** remains the unanswered question. The
   separate-instance probe sidesteps it; it does not solve it.

## Material bullet-passthrough — worth taking, but it is NOT a penetration model

The fork has `intersect::Material` with a per-material **boolean**
`bulletPassthrough`, an impact decal per material, and a transparency class,
driven from config:

    class glass { bulletDecal="BulletHoleGlass"; bulletPassthrough=1; transparency="alpha"; };

`Shot::ProcessCollision` loops: on hitting a passthrough material it records the
section handle, skips it and re-tests from the hit point, leaving that material's
decal. There is no energy loss, no thickness, no calibre dependence -- the author's
own comment in that loop reads `// TODO add penetratino depth`.

So it is "bullets ignore glass and leave a glass hole", not "different materials
resist differently".

**What is worth taking is the SHAPE, not the data model:** the ignore-section loop
is exactly right, and small. **We are positioned to do better than the original.**
PHY-030 already casts a physics ray over each projectile segment; entry and exit
points against the same collider give a THICKNESS, and thickness plus the drag
model already in `Ballistics.cpp` gives an energy loss. That is a real penetration
model rather than a boolean, and the fork does not have it.

*Licensing was raised and settled: the owner confirmed on 2026-08-29 that we have
permission to take from that tree. One line, so nobody re-opens it.*

## Traps worth not rediscovering

* `Landscape::GetHeight(z, x)` is NOT the world surface — it read −59 where
  `SurfaceY` gave 269. Build height fields from `SurfaceY`, which takes world
  coordinates.
* Debug lines must be submitted INSIDE `Scene::BeginObjects`/`EndObjects`, or
  they are silently discarded and the thing simply never appears.
* `GetCheat2ToDo` gates on a BOUND ACTION, not a fixed key. Dev hotkeys read
  ImGui's key state instead.
* Object pipelines cull with `front_face = Cw`. Winding is per generated mesh —
  flipping all of them fixed the cylinder and broke the box.
* `b3Quat` is `{ b3Vec3 v; float s; }`; `b3Matrix3` is three column vectors.
* `b3CreateHullShape` COPIES the hull (measured); `b3CreateHeightFieldShape` holds
  a REFERENCE, so the world must be destroyed before the field.
* `tests/unit/engine/Poseidon/CMakeLists.txt` lists sources explicitly; a new test
  file is silently absent until added there.
* Turn grass off (`WGR_GRASS=0`) for probe captures. A 20 cm ball is invisible in
  it, which cost several runs.

*Last updated: 2026-08-28, after the merge to trunk and PHY-030.*
