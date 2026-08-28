# PHY-001 — What to port from the `inspiration/otherfork` physics work, and in what order

**Date:** 2026-08-28
**Branch:** `consolidated-v2`
**Status:** proposal, nothing implemented
**Supersedes nothing. Depends on:** ARCH-001 item **C** (fixed-step accumulator)

This is a scope document. It exists because a survey of `inspiration/otherfork`
turned up several things that *look* portable, and only some of them are. Every
number below was measured in this repository or in that tree on the date above;
where something is reasoned rather than measured it says so.

---

## 0. The prerequisite, and it is not optional

ARCH-001 §1.5 found that **there is no fixed timestep anywhere in the engine**.
`World::Simulate(float deltaT, bool&)` takes a variable frame delta and
`GameLoop.cpp:60` welds the simulation and render ticks together. Its §6 then
schedules the fixed-step accumulator **before** any physics library, with the
reasoning that otherwise the physics gets written against a variable delta and has
to be redone.

That warning has already come true once, in work shipped this session:

| what | step | reproducible? |
|---|---|---|
| `Ballistics::SimulateAtDistance` (the AI's aim solution) | fixed, `DefaultStep = 1/500 s` | yes |
| `ShotShell::Simulate(float deltaT, …)` (the projectile in flight) | frame delta | **no** |

So the shot the AI *predicts* is deterministic and the shot that actually *flies*
is not. Nobody has reported a symptom, and at typical frame times the divergence is
small — but it is exactly the failure ARCH-001 named, and a rigid-body solver
stepped the same way behaves far worse than a ballistic integrator does.

**Nothing in this document should start before the accumulator lands.** That is
ARCH-001's call, not this document's, and it was made before any of this work
was proposed.

---

## 1. What was measured

### 1.1 The fork's physics layer is ~85 % engine-agnostic

`fp/physics/` is 1,666 lines across seven files. Lines touching the PhysX API at
all:

| file | lines | PhysX-touching |
|---|---|---|
| `World.hpp` / `World.cpp` | 1,217 | 18–21 % |
| `Shape.cpp` | 139 | 12 % |
| `Terrain.cpp` | 82 | 13 % |
| `SimpleManager.cpp` | 88 | 11 % |
| `CartridgeManager.cpp` | 117 | 11 % |

The rest is structure: a manager per object class, lifetime, transform in/out,
debug draw. That part is library-independent. The genuinely PhysX-specific surface
is roughly 250–300 lines — shape creation, body creation, step, query.

### 1.2 Our colliders are already decomposed, and that is the expensive part

The fork's `Shape.cpp` consumes an already-prepared `PhysicsShapeInfo` carrying a
convex/triangle-mesh split; that preparation happens in their model loader, outside
the 1,666 lines. Producing the equivalent would normally be *our* largest cost,
because convex decomposition of arbitrary meshes is the hard problem in this area.

It does not apply to us. `Shape.hpp:682` already defines `class ConvexComponents`,
populated from the Geometry LOD via `RecalculateGeomComponents()`. This is not
luck: OFP models have been authored this way since 2001 — the artist builds the
Geometry LOD out of named convex parts (`Component01`, `Component02`, …) and the
engine reads them as such. We additionally carry separate `GeometryLevel`,
`FireGeometryLevel` and the several view geometries.

The bridge is therefore `ConvexComponents` → convex hull shape, and heightmap →
heightfield. Both are directly supported by every candidate library.

### 1.3 The fork did not solve the reconciliation problem — it avoided it

This is the finding that most changes the plan, and it corrects an earlier
assumption in this session that their integration was further along.

What is live in their tree:

* `SimpleManager` is a **member** of many object classes (`house.hpp:167`,
  `detector.hpp:246`, `objectPlain.hpp:67`, three times in `objectClasses.hpp`).
* Its data flow is **inward**: `SetTransform()` → `setGlobalPose()`. The 2001
  simulation stays the authority on position; PhysX is told where things are.
* PhysX is then **queried**: `physics::g_world.RayCast(...)` at
  `aiSubgroupFSM.cpp:2052` and `:2933`, for AI line of sight.

What is not live:

* `Man::UpdatePhysicsCapsule()` is an **empty function body**, called from five
  sites.
* The ragdoll bone push is commented out: `// TODO_soldierPhysicsManager->SetBoneTransforms(_boneTransforms);`
* `CartridgeManager` — the one class with real owned dynamics
  (`SetTransformAndVelocity`, `AddDeltaTime`, `OnContactModify`) — is **never
  instantiated anywhere in the tree**.

So their answer to "who owns the position when two simulations disagree" is: don't
let the question arise. Physics is a read-only query service. The directory is
named `FP_269_vk_WIP` and that is a fair description.

This is still useful. It means step 1 below has a working precedent, and steps 4
and 5 have none — which is worth knowing before estimating them.

---

## 2. Library choice: Box3D, with Jolt as the named fallback

Both `joltphysics 5.5.0` and `physx 5.5.0` are present in the local vcpkg, as are
`box2d 3.1.1` and `box3d 0.1.0`. Adding any of them is one line in `vcpkg.json`.

PhysX is out: Jolt and Box3D are both better fits and licensing does not
discriminate between any of them (Jolt MIT, Box3D MIT, PhysX 5 BSD-3 — all
compatible with GPL-3).

### Why Box3D

**Its documented feature set covers every step this document actually schedules.**
Convex hulls, capsules, spheres, triangle meshes, **height fields**, CCD, a
character mover, ray/shape casts and overlap queries, sensors,
revolute/prismatic/distance/motor/weld/**wheel** joints with limits, motors,
springs and friction, multithreading, SIMD, island sleep, and stated cross-platform
determinism. PHY-010 through PHY-040 need nothing beyond that list.

**It is small enough to read end to end**, which Jolt is not. When a rigid body does
something inexplicable — and it will — being able to read the solver is worth more
than most feature comparisons. Erin Catto's solver work is what most engines
imitate.

### What that costs, stated plainly

* The vcpkg port is **0.1.0**, the repository shows roughly **40 commits** on
  `main`, and there is **no stated API stability guarantee** and no
  production-readiness claim.
* **No tracked-vehicle constraint is documented.** Wheel joints yes, tracks no.
* "It will be expanded anyway" is a reasonable expectation but a bet on someone
  else's timeline. It is a reason not to *rule out* Box3D; it is not a reason to
  choose it.
* **Being smaller is not measured to be cheaper at runtime.** Jolt is heavily
  optimised and has shipped in AAA titles; Box3D has not been benchmarked by us or,
  as far as this document knows, by anyone at scale. Choose it for readability and
  size, not for performance claims nobody has verified.

### Why the cost is acceptable

The tracked-vehicle gap bites only at **PHY-050**, which §3 deliberately does not
schedule and explicitly refuses to estimate. Choosing a library today on the
strength of a step we have declined to plan is the wrong way round.

And §1.1 measured the thing that bounds the rest of the risk: only **250–300 lines**
of any of these integrations is library-specific. Behind the interface in §2.1, a
later move to Jolt is a file, not a project. A bounded risk taken for a real benefit
is a reasonable trade; an unbounded one would not be.

### The revisit points, so this is a decision and not a drift

1. **At PHY-020.** The test-body step is where Box3D first has to behave. If
   stacking, sleeping or CCD misbehave on real building geometry, switch then — the
   binding is at its smallest and nothing depends on it yet.
2. **At PHY-050.** If vehicle dynamics are scheduled and Box3D still has no tracked
   controller, re-evaluate. By then the binding is proven and its replacement is
   scoped.
3. **On an API break that costs more than a day.** Pre-1.0 means this can happen;
   the point is to notice it as a signal rather than absorb it silently.

### 2.1 The interface is a requirement, not a nicety

Whatever we bind to, the engine must not see it. The surface is small — create body,
build shape from a Geometry LOD, step, query — and §1.1 shows that this is where
the library-specific code actually lives. Keeping it behind our own header is what
makes the library question a file rather than a project.

---

## 3. The plan

Each step states what would make it stop.

### PHY-010 — Bind Box3D, nothing writes back

`ConvexComponents` → convex hull shapes; heightmap → heightfield; a manager per
object class following the fork's structure. Transforms flow **in** only. No
gameplay code reads anything.

*Abort if:* the collider set for stock CWA buildings cannot be produced without
per-model special cases. That would mean §1.2's premise is wrong and the whole
estimate collapses.

### PHY-020 — Dev-panel test bodies

Spawn a sphere / box / capsule from the camera, with mass and throw direction;
a counter and a clear-all; the fork's debug lines.

This is not a toy. It is the **proof for PHY-010**: a ball that rolls down the
hotel stairs instead of falling through them demonstrates that the collider
generation works, on exactly the geometry that has historically given us trouble.
Nothing depends on these bodies, so a wrong result costs a screenshot.

*Abort if:* bodies behave plausibly only on flat ground. Then the Geometry LOD
mapping is wrong and PHY-030 onward would be built on sand.

### PHY-030 — Ray queries as a second opinion

Run ray/visibility queries against the new colliders **alongside** the existing
2001 path and log the disagreements. Do not replace anything.

Two payoffs even if it never becomes authoritative: it is a real accuracy check on
the collider set, and it is the tool that would have measured the buried-buildings
placement problem on Everon instead of flying to it.

*Abort if:* disagreement is high and the new path is not obviously the better one.

### PHY-040 — Skeleton table, derived capsules, ragdoll behind a dev switch

**No new art is required.** RTM stores a 3×4 matrix per **named bone** per phase
(`RTMReader.hpp:30`), so our soldiers are already skeletal; for A2/A3 rigs
`synthesizeBoneSelections` fills the selections from `vertexBoneRefs`.

What is missing is a **table, not a model**: RTM keeps bones as a flat list with no
parent/child relation, and no skeleton hierarchy exists in our code. A ragdoll joint
needs one. That is roughly 30 bones with parent, joint type and angle limits —
lookup work, not research, because the OFP bone names are fixed and known. The
per-bone collision capsules can be **derived automatically** by fitting a capsule to
the vertices weighted to each bone.

Two honest limits:

* **Cross-generation.** Our own measurement records that A2/A3 rigs lose a pivot, a
  name and a subtree in translation. A hierarchy authored for CWA soldiers will not
  automatically hold for bridged foreign models. Start with stock soldiers.
* **This is visible gameplay, not decoration.** OFP death animations are part of the
  game's identity. A ragdoll that takes over on death can twitch, sink into terrain
  or slide down stairs, and everyone sees it. It stays a dev-panel switch, applied
  to a body that is already down, with the authored death animation as the default,
  until it is convincing.

*Abort if:* capsule fitting needs hand-tuning per model. At that point it is art
work after all, and the estimate was wrong.

### PHY-050 — Vehicle dynamics

Deliberately unscheduled. There is **no precedent in the fork** — this is where
their work stops. It would need its own design pass and should not be estimated
from anything in this document.

---

## 4. Separate track: collision code worth taking on its own

`fp/intersect/` is 4,960 lines. Its external dependencies are narrow: their math
library, a handful of `fp/std/` container headers, and — only in the skinned path,
**two includes** — `fp/simple_renderer_vk/ModelDataAnimation.hpp`.

**Do not port it whole.** Their `World.hpp` (935 lines) is a segment grid with DDA
traversal and a `Visibility()` returning a *float* — a geometric query library. Our
counterpart, `WorldImpl.cpp:1023`, is mostly **perception rules**: it checks object
types, group membership and whether the unit is still in the landscape, returns `1`
outright in several branches, and delegates the geometry to
`_sensorList->GetVisibility()`. They do not solve the same problem, so this would be
a substructure replacement rather than a swap.

Two pieces are worth taking by themselves:

* **INT-010 — `GjkEpa.hpp` + `EpaConvexHullBuilder.hpp` (1,129 lines).** Convex
  penetration with depth and normal. A self-contained algorithm with no world model,
  depending only on vector maths. It lands directly on `ConvexComponents` and
  underpins everything above. The cleanest port in the whole fork.
* **INT-020 — `SkinnedShape.cpp` (475 lines).** Hit evaluation on animated meshes —
  which bone, which material. This is the real gameplay gain over the 2001 hit
  model. Needs the bone-interface design work, and shares that work with PHY-040.

~1,600 lines instead of 4,960, without adopting their world model.

---

## 5. Explicitly not porting

| item | why not |
|---|---|
| **VR / OpenXR** | Their VR layer is ~904 lines of eye masks and types — scaffolding, and inside *their* Vulkan renderer, so nothing transfers to wgpu. The blocker is budget, not effort: our own measurements show stock worlds are GPU-bound, grass alone is a third of the frame, and LOD detail is already framerate-governed. Stereo at 90 Hz is roughly six times the present pixel budget. This is the one item where we can show we do not meet the precondition. |
| **Meshlet rendering** | Real `GL_EXT_mesh_shader` task/mesh shaders with cluster culling, 577 lines. But wgpu exposes no mesh shaders, so there is no route; and we are fill- and alpha-test-bound (grass), not vertex-bound, so it would not touch our actual cost. |
| **Land editor** | Named optimistically. Its verbs are object placement and posing (`add_island_object`, `add_soldier`, `set_soldier_bone_transform`, ray picking). It **loads** heights and has no terrain-sculpting verb and no save path. It cannot write `.wrp`. |
| **`fp/intersect` as a whole** | §4 — different problem shape from ours. Take the two pieces. |
| **PhysX** | §2 — Box3D instead, with Jolt as the named fallback. |

| **Their soldier physics** | §1.3 — it does not exist; the function body is empty. |

---

## 6. What this document does not do

It does not add ledger entries. `status-ledger.yaml` is the roadmap of record and
its gate has a strict regenerate → rehash → validate order; adding tickets for work
nobody has started would put unfinished dependencies into that graph for no benefit.
Ledger entries belong with the first commit of PHY-010, not before it.

It also does not estimate PHY-050, and it does not claim the fork validates steps 4
and 5 — because, per §1.3, it does not.
