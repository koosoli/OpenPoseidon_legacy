# PHY-040 — Stock US corpse: authored parts and moving-pose gate

Source review: `3353770c`, 2026-10-01. Source-only diagnostic; no new game,
solver, deployment or articulated-corpse acceptance. Authored deaths remain the
default. The accepted [affine hold](PHY-040-corpse-affine-pose-proof-20260930.md)
proved an unchanged pose, not a moving final palette.

## Better body geometry exists

The new read-only `scripts/physics/inspect_stock_corpse_parts.py` reuses the
bounded, checksum-checked ODOL7 parser. Its exact western stock member is
`mc vojakw2.p3d`; stored SHA-256 is
`f3ca907846587abc8e787fbfeca3d9a2ffe16f4f2025fda67ffbae66c977155c`.
[Measured evidence](../evidence/phy-040-stock-wb-authored-parts-20261001.json)
records bounds, memberships and both source hashes. Coordinates are before
engine recentering and animation. No component is silently admitted as a joint,
contact fit, convexity proof, inertia or runtime body.

Fire Geometry LOD 10 contains the following authored parts. Each anatomical
part has 24 vertices, exclusively and fully weighted to its listed bone.

| Component | Bone | Candidate body |
|---|---|---|
| 01, 02 | pchodidlo, lchodidlo | respective shin + foot |
| 03 | zebra | combined torso |
| 04 | hlava | head |
| 05, 06 | lbiceps, lloket | left upper arm, forearm |
| 07, 08 | pbiceps, ploket | right upper arm, forearm |
| 09, 10 | lstehno, lholen | left thigh, shin + foot |
| 11, 12 | pstehno, pholen | right thigh, shin + foot |
| 13, 14 | zbran, roura | equipment; not anatomical collision bodies |

Component 13 has 24 vertices; 14 has 20. Geometry/View Geometry LOD 6 have
only nine components and omit arm hulls. They cannot alone supply a complete
articulated body. Fire Geometry supplies twelve anatomical hull candidates;
merging each foot with its shin gives **ten** bodies, rather than a fabricated
single capsule. Combined torso removes the unresolved `bricho/zebra` spine
anchor. This is a proposed grouping, not a claim that source weights prescribe
that anatomy.

All 25 phase names must be resolved by name, not clip order: 23 anatomical
names plus `zbran` and `roura`. Torso candidates include `pzadek/lzadek/bricho`,
`zebra/hrudnik/krk/prameno/lrameno`; hands follow their forearms and toes follow
their shins. Equipment needs an explicit captured attachment policy; assigning
it to the nearest arm or dropping it arbitrarily is not admission. Synthetic
face bones must continue to ride the final head transform. Runtime weights and
face rebinding remain authoritative, not this raw membership diagnostic.

The earlier nine boundary centroids supply candidate head, shoulder, elbow,
hip and knee anchors. No calibrated anatomical hinge/cone axes or limits exist.
That alone does not forbid a bounded opt-in **axis-free spherical-joint proof**:
Box3D supports spherical joints. It does forbid inventing anatomical limits or
advertising the resulting unconstrained joints as a calibrated human rig.
Actual anchor placement, initial contacts and complete pose consumption are
still required before creating bodies. Memory LOD points are aiming, camera,
footstep and equipment helpers; their names do not supply elbow/knee frames.

## The base hold is not a complete moving-pose seam

Current `Man::PrepareBasePose` supplies held primary/secondary phase pairs.
`Man::Animate` then applies head, aiming and graphical-leg corrections, writes
synthetic face bones and applies the matrices. `AnimatePoint` separately applies
aiming and leg transforms. `AnimateMatrix` orthogonalizes its weighted result,
then its selection overload applies aiming/head transforms. Those consumers
have deliberately different legacy arithmetic.

`AnimationRT::CombineTransform` applies a correction separately to each bone
matrix; `TransformPoint` applies each influence's correction to the *already
blended point*. With different influences and correction factors these are not
equivalent. Thus merely inserting rigid deltas into held phase matrices would
put corrections on the wrong side and can move special geometry/memory/proxies
differently from the visible mesh. A mathematically similar helper test cannot
establish that the production consumers agree.

Death simulation sets `landLegsFactor=0` before its dead branch, but only writes
the leg matrix when `change` is true. Gun/head identities are recalculated from
manual aim, aim angles and recoil, not explicitly guaranteed by corpse hold
eligibility. Capture must observe the actual corrections; a settled corpse
must not be assumed to have identity post-transforms.

The next production proof should remain one local dry stock corpse, no solver:

1. Record complete final graphical/special/memory/proxy outputs and actual
   correction state from the existing consumers, with all runtime bones covered.
2. Prove identity retains their original arithmetic exactly. Then exercise a
   common nonidentity rigid delta after the final consumer operations, including
   synthetic face, attachments, special LODs and bounds/shadow invalidation.
3. Before independent limb deltas, resolve the mixed-weight correction
   discrepancy above. A per-consumer preserved affine residual is a possible
   design, but must be measured against actual production outputs and checked
   for continuous no-jump behavior as delta tends to identity. Switching from
   legacy point arithmetic to graphical LBS only when motion starts can jump.
4. Report actual posed hull extents, finite/invertible body frames, world-space
   paired anchor error and initial terrain/object penetration before admission.
   Transform authored hull vertices through their full initial affine pose;
   quaternion conversion of a nonrigid RTM matrix is invalid.

Proposed ownership for that proof: `CorpsePose.hpp`, corpse-only functions in
`SoldierOld.cpp/.hpp`, `SoldierOldSimProxy.cpp::Animate/BasicAnimation`, and
production animation/pose tests. Solver work is separate: `PhysicsTypes.hpp`,
`PhysicsBackend.hpp`, `PhysicsWorld.cpp/.hpp`, `Box3DBackend.cpp`, plus a scoped
corpse controller and the death simulation ownership branch. No such production
edits are included in this commit.

## Ownership and lifetime before solver activation

The present hold is limited to 30 simulation seconds and one weak global owner;
it validates model/skeleton/weights/animations/world, and clears on expiry,
hide, delete, serialization and animation changes. It requires
`ShadowPoseFrozen`; moving bodies invalidate that condition. A solver needs
its own lease and explicit suspension/cache invalidation, not reuse of the
static-corpse eligibility test. Legacy dead simulation currently zeros speed
and angular momentum and aligns the root to the ground; it must not compete
with an opted-in physics owner.

Create all bodies/joints atomically with rollback, generation-checked handles
and body self-collision policy. Retire constraints before bodies on refusal,
hide/delete/revival/world change/save/network transition/backend shutdown.
No physics handle or limb pose is added to existing save/network schemas.
Unsupported corpses fall back to authored deaths. Collision registration,
terrain/road support and contact penetration must be actual source queries,
not a camera-height floor or an assumed collider. Preserve root ownership until
a separate world-motion gate is validated.

## Checks and remaining acceptance

Three pure diagnostic tests pass: full-weight single membership, mixed/partial
membership rejection and empty/nonfinite refusal. One bounded stock member was
read without extraction or modification. The prior no-motion installed proof
remains the runtime evidence; there is **no nonidentity or joint-body runtime
proof yet**. Full stock animation scans, GPU tests and game launches were not
performed during the parent's runtime campaign.

Reproduce this new evidence with:

```powershell
python scripts/physics/test_stock_corpse_parts.py
python scripts/physics/inspect_stock_corpse_parts.py --archive 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault/DTA/Data3D.pbo' --out docs/roadmap/evidence/phy-040-stock-wb-authored-parts-20261001.json
```
