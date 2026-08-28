# ACTIVE HANDOVER — physics, and the fixed step it stands on

**This file is live. Whoever works on this updates it in the same commit as the
code.**

| | |
|---|---|
| **Branch** | `consolidated-v2` — TRUNK. The physics branch was merged 2026-08-28 (fast-forward, 39 commits) and `codex/arch-001-fixed-step` is now history. |
| **Plan of record** | `docs/roadmap/decisions/PHY-001-what-to-port-from-the-fork-20260828.md`, ARCH-001 item **C** |
| **Deployed** | yes, from trunk |

## Done, with numbers

**Fixed step (ARCH-001 C).** World clock, weather, wind, AI, vehicles and
visibility run at a fixed 1/60 s. Measured across four play sessions: 1.03–1.52
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

**PHY-030.** Every projectile segment is asked of both collision systems and the
disagreements are counted. Replaces nothing.

## What is open

1. **The ray audit has no data.** It reports 0 segments — nothing fires in a
   free-fly capture and there is no headless way to make the AI shoot. Tick
   "Compare physics rays" in the Physics tab, empty a magazine into a wall, read
   `legacy-only`. That counter is the only thing that can say whether 82,381
   objects converted to the right SHAPE, as opposed to merely converting.
2. **Fixed-step scripts are implemented and OFF** (`POSEIDON_FIXEDSTEP_SCRIPTS`,
   or the Fixed Step tab). The last ARCH-001 item. Needs campaign missions with
   cutscenes played, not code review — see §3 of the old handover text below for
   why a naive move breaks them.
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
