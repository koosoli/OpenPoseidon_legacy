# PHY-040 — Stock corpse articulation with Box3D

**Date:** 2026-09-30. **Source baseline:** `codex/streaming-residency`,
`ed452be8`. **Status:** feasibility assessment; no runtime ragdoll implemented.
**Related:** [original physics proposal](PHY-001-what-to-port-from-the-fork-20260828.md),
[ownership boundary](SIM-804-physics-integration-boundary-20260831.md).

## Decision

Box3D can support an articulated stock soldier corpse. It makes sense to pursue
an opt-in development prototype after the grass and muddy-puddle priorities.
Replacing death animations by default is not a small or low-risk improvement:
the missing work is pose consistency and lifetime ownership as well as joints.
Keep authored deaths, floating bodies and corpse interactions as the default.

The next meaningful implementation is **one existing, settled, stock corpse
with a complete pose override**, limited to local single-player and dry ground.
Implement its joint binding and rig validation as part of that working vertical
slice. A capsule, unconsumed skeleton table, or renderer-only limb palette does
not complete this task and should not be reported as a ragdoll.

## What was actually checked

This assessment reads the named baseline's source and the installed Box3D
headers at `C:/vcpkg/packages/box3d_x64-windows-clang/include/box3d`.
It also reads retail `DTA/Anim.pbo` from
`D:/SteamLibrary/steamapps/common/ARMA Cold War Assault` without extracting or
changing game data. The archive's SHA-256 is
`5fda23e73a3c052f70bab2a4e4831ede6445113049db4b1e8a45ec939fc239aa`.

The installed binary stamp read before inspecting assets was
`codex/streaming-residency b8f61b97 2026-09-30_08:08 deploy-ps1`.
There was no game launch, deploy, rendered comparison or solver measurement in
this assessment. Source and corpus evidence below do not establish visible
ragdoll quality or runtime cost.

### The stock skeleton has 25 names, not a measured 30-bone hierarchy

The archive has 521 entries, including 517 valid `RTM_0101` animations. Of those,
509 have the same 25-name stock person bone set in **three different orderings**
(499, 8 and 2 clips respectively). The other 8 animations belong to other rigs.
The most common order is:

```text
pchodidlo lchodidlo pprsty lprsty lholen pholen pstehno lstehno
pzadek lzadek bricho zebra hrudnik krk prameno lrameno hlava
pbiceps lbiceps ploket lloket roura zbran pruka lruka
```

`zbran` and `roura` are equipment bones. The remaining 23 are anatomical
selections, including fingers/toes, shoulder and torso subdivisions that do not
each need their own dynamic body. Rig binding must resolve **names**, never
assume those RTM file indices are the engine skeleton indices. The loader already
remaps names. `Skeleton` in `RtAnimation.hpp` stores names only, and face animation
adds synthetic bones to the runtime palette. Those synthetic bones must follow
the head; they must not become dynamic ragdoll parts.

A candidate first rig can use pelvis, chest, head, four upper/lower arm parts and
four upper/lower leg parts (11 bodies), with hands/feet as optional additional
parts. This is a proposed grouping, not a validated hierarchy or capsule fit.
There is no measured evidence here that all stock model selections can produce
acceptable capsules without per-model adjustments.

### RTM matrices are skin deformations, not joint frames

`Asset/Formats/RTM/RTMReader.hpp` stores twelve floats as orientation columns and
translation. `AnimationRTPhase::LoadFromRTM` in `RtAnimation.cpp` copies them into
the engine matrix and, for reversed animations, conjugates by the X/Z sign-swap
matrix. `AnimationRT::Point`, `Matrix` and `ApplyMatrices` apply those matrices
directly to original model positions. They are not parent-relative transforms.
The existing `scripts/default-content/generate_jeep_animations.py` independently
uses this convention and documents model-centering differences.

Consequently a matrix translation is not an elbow, hip or shoulder location.
Capsule centers and joint frames need validated selection/weight geometry and
model-space calibration. Fitting an envelope around weighted vertices does not
by itself locate a biomechanical joint or its hinge axis.

The raw animations also are not uniformly rigid. A read-only wire walk selected
the 141 members whose names contain `smrt` or `mrtv` (a **filename heuristic**,
not a config-resolved list of all death states). It checked 27,900 matrices:

| Measurement | Result |
|---|---:|
| Maximum component of `abs(R^T R - I)` | 0.8624557516 |
| Determinant range | 0.0357898514 to 1.0000474330 |
| Negative determinants | 0 |
| Last-phase matrices checked | 3,525 |
| Last-phase matrices with orthogonality error over 0.001 | 70 |

The final-phase exceptions are `lloket` and `lruka` in 29 clips each, and
`ploket` and `pruka` in 6 clips each. Intermediate deviations occur on every
stock bone, not only equipment: for example `lruka` in `SSMRTB.rtm`, phase 2,
has error 0.490347094 and determinant 0.508953751. Simply converting an RTM
matrix to a quaternion loses deformation and can visibly snap the corpse.
Engine interpolation and additional head/aiming/leg transforms are separate
from these raw-data measurements; the actual final palette must be sampled.

For a no-jump takeover, preserve the initial deformation matrix alongside the
rigid solver frame. One proposed mapping for bone `i` is:

```text
D_i(t) = inverse(O(t)) * B_i(t) * inverse(B_i(0)) * O(0) * D_i(0)
```

Here `O` is the corpse object's world transform, `B_i` is its assigned rigid
body's world frame, and `D_i(0)` is the completed model-space deformation at
takeover. The equation preserves the initial palette exactly at time zero,
including a nonrigid residual. It is a design proposal requiring tests;
unsupported or singular initial matrices must refuse takeover cleanly.

### The solver can do the required joints; the engine seam cannot yet

Installed Box3D 0.1.0 `types.h` and `box3d.h` expose:

- `b3SphericalJointDef`: cone limits, twist limits, springs and torque-limited motors.
- `b3RevoluteJointDef`: hinge angle limits, springs and torque-limited motors.
- `b3JointDef`: two local frames and `collideConnected`.
- Shape `groupIndex` filtering and body/joint destruction APIs.

This supports shoulder/hip cone constraints and elbow/knee hinges in principle.
Joint frames use their local Z axes; selecting an anatomical axis is still rig
work. Motors are not needed for a passive corpse. Joint limits, self-collision
policy and stability under impacts have not been exercised in this assessment.

`PhysicsBackend.hpp` has dynamic body creation and `GetBodyMotion`, but no joint
handle, joint creation/removal, capsule definition with arbitrary local endpoints,
or per-corpse contact-filter vocabulary. Extend the backend-independent seam with
generation-checked handles and bounded definitions when the real consumer lands;
do not leak Box3D types into infantry headers. Destroy joints before bodies,
handle partial creation atomically, and ensure body/world teardown invalidates
owned handles. `SpawnSphereProbe` is a diagnostic spawner, not corpse ownership.

### Corpse pose is consumed in more places than rendering

`Man::Animate` in `SoldierOldSimProxy.cpp` builds and applies the vertex palette,
then runs `BasicAnimation`. The same palette path serves graphical and special
LODs, with additional view/head/weapon logic. `Man::AnimatePoint` and both
`AnimateMatrix` overloads in `SoldierOld.cpp` separately evaluate animation
weights for points, proxies and memory positions. A render-only replacement
would leave those consumers and special collision geometry in the authored pose.

All consumers need one committed corpse-pose source. Preserve weighted-vertex
semantics, face-parent behavior, equipment transforms and per-object palettes;
never write a ragdoll pose into the shared animation asset or shared bind mesh.
Update root bounds and movement notifications when bodies spread or move, or
visibility/shadow caches and corpse selection can become stale.

`Man::Simulate` in `SoldierOldSim.cpp` currently zeros dead-body velocities,
aligns orientation with the road/terrain normal, resolves ground penetration,
and suspends a steady corpse after five seconds. It also drives gradual hiding
and eventual deletion. The same file contains the existing `BodyWaterSupport`
path and prevents floating corpses entering the static corpse/shadow cache.
That simulation must yield ownership for the opted-in corpse, while hiding,
deletion and revival still cleanly release its bodies and joints. Running both
root correction and articulated motion would make the solvers fight.

`Man::Serialize` and `IndicesUpdatePositionMan` in `SoldierOldAI.cpp` preserve
authored animation state and normal entity identity; they carry no limb-pose
snapshot. `LooseObjects.cpp` is a useful ownership precedent but owns one rigid
`Thing`, not a skeletal corpse. Its environment setup must not be mistaken for
an already complete ragdoll world-registration/lifetime service.

## Revised implementation gates

These refine PHY-040 without adding an unfinished dependency to the status ledger.

| Gate | Required result |
|---|---|
| Stock rig admission | Measured weight/selection coverage, valid model centering, finite capsule fits and joint frames on ordinary western, eastern, resistance and civilian models. Reject missing/ambiguous mappings with a reason; exclude equipment and synthetic face bones. |
| Pose seam | One corpse palette feeds render, memory/proxies and special collision LODs. A no-motion takeover reproduces the completed authored pose within a stated tolerance, including multi-weight vertices and nonrigid initial transforms. |
| Actual articulated prototype | Name-bound multi-part stock corpse with real constrained limbs, instantiated only by an off-by-default development action. Isolated local single-player, settled body, dry ground, supported rig and valid static/terrain collision world. A new or failed admission keeps the stock behavior. |
| Ownership and teardown | Bound corpse count, bounded simulation time, sleep/freeze policy, atomic creation failure, clear switch, hide/delete/revive/unload cleanup, terrain/static registration and no stale handles. Legacy ground correction does not run against an owned corpse. |
| Save/load and identity | Saving an active prototype has an explicit implemented policy: serialize stable model-space pose plus rig identity, or retire back to the preserved authored state before serialization. Never persist backend handles. Old saves and online sessions retain stock behavior; online gate stays hard-disabled until authority and pose replication exist. |
| Visual and contact acceptance | Installed-game captures on flat ground, a slope, steps and building floors; impulse/blast, hide/delete, reload and mission restart; near and distant views; GPU and CPU skinning, shadows, weapon attachment and corpse selection/hit geometry. Water corpses and vehicle/ladder-attached bodies remain on the existing path. |
| Promotion | Only after the prototype meets the above, measure mass-corpse budgets and decide default behavior. Preserve authored death transitions and add other model generations only through independently validated rig mappings. |

A supported foreign soldier must not be identified solely by matching one or two
bone names. A1/A2/A3/bridged models retain authored behavior until their full rig,
coordinate convention and selections pass independent admission. This is a safe
fallback, not a prohibition on later cross-generation support.

## Corpus measurement method

The read-only Python walk reused `cstring` from
`scripts/Inspect-RigidOdol7Candidate.py` for the PBO table, then read each
uncompressed RTM member in memory. For every RTM it verified signature,
header counts and exact decoded byte size:

```text
28 + (boneCount * 32) + phaseCount * (4 + boneCount * (32 + 12 * 4))
```

It grouped complete ordered header bone-name tuples. For the filename-matched
death subset it verified every repeated phase bone name agrees with the header,
checked all twelve floats are finite, and unpacked columns `m[0:3]`, `m[3:6]`,
`m[6:9]`. It computed `max(abs(dot(column_i, column_j) - (i == j)))` and the
3x3 determinant for each matrix, separately accumulating final-phase counts.
There were no signature/size/name/finite-value failures in that measured set.
No model capsule fit, imported geometry, config death-state traversal or runtime
physics was claimed. No code, game data or installed binary was changed.
