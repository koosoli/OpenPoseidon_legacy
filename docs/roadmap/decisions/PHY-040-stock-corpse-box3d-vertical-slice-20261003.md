# PHY-040: explicit stock corpse Box3D vertical slice

Source baseline: `codex/streaming-residency` `94f261c5`. This candidate is an
off-by-default developer action. It is not the default death behavior, a network
feature, or accepted general corpse contact quality. Root owns build, installation
and actual-game verification; a passing source compile does not close those gates.

## Actual consumer and physics connection

`triPhysicsShowcase "corpse-capture"` retains the actual authored affine phases.
`"corpse-prepare"` brings up the existing physics corpus if no world exists; an
existing world is preserved, and missing terrain is explicitly refused.
`"corpse-articulate"` fits and creates eleven dynamic Box3D convex bodies and ten
limited spherical joints. Pelvis, chest, head, upper arms, forearms/hands, thighs
and shins/feet are distinct bodies. Cone and twist limits are centered on the
captured pose. This first slice uses spherical limits also at elbows/knees;
anatomical hinge-axis calibration remains a promotion gate. Internal collisions
are disabled for the single admitted corpse. No motor, wind or probe-water
controller acts on these bodies.

Solver readback creates the model-space rigid delta
`inverse(O) * B(t) * inverse(B(0)) * O(0)` for each assigned anatomical group.
It left-multiplies each ORIGINAL previous/next phase matrix, preserving authored
scale/shear and the existing per-phase/per-weight operation order. Deltas are
never accumulated. Exact initial solver frames take the identity path, preserving
bitwise capture arithmetic. Render palettes, CPU skinning, Memory points and
weighted proxy/special-LOD matrices consume the same owned phase data. The existing
face palette construction reads the moved head parent; reserved synthetic matrix
tails also receive the head delta. Equipment follows the chest/right forearm.

While the lease is active, Man's authored ground-alignment simulation yields
ownership and its shadow pose remains animated. The object's root is retained.
Individual body-center travel is bounded to 0.5 m, and all initial collider
vertices must be within 2.5 m of the retained root. This is deliberately bounded
settled-corpse articulation, not arbitrary root locomotion or bodies thrown across
the map. Distant culling, cast shadows and complete hit geometry still require
installed visual acceptance. Actual final palettes project every admitted LOD
vertex each tick to produce corpse-owned animated/draw bounds, including the
dead face tail; a point beyond 3.5 m or a nonfinite palette retires the rig.

## Admission evidence and limits

Admission requires a local single-player dead, settled, visible, unattached corpse
with the actual `data3d\mc vojakw2.p3d` model, all 25 stock names and eight reserved
face names in its 33-bone palette, unit-scale world frame and identity additional
head/gun/leg corrections. Other models and correction states remain authored.

The installed read-only receipt
`final-pose-installed-20261001-052726-193320` contains genuine final matrices,
source-selection paired anchors and individually weighted Fire hulls. It establishes
head clearance -46.967 mm, chest -72.723 mm, right upper arm -74.661 mm,
shoulder endpoint separation 52.137/57.388 mm and left hip separation 78.774 mm.
Its overall PNG-baseline campaign failed; these complete actual getter receipts
are measurements, not a completed contact/visual acceptance campaign.

Consequently raw Fire hulls are NOT used unchanged. The candidate insets each
anatomical Fire component by 0.40 around its actual animated centroid, combines
feet with their shin, and fits the pelvis from actual animated graphical pelvis
members, excluding proxy helpers. This is an explicit conservative collider
calibration; it does not claim to enclose the complete visible skin. Every fitted
vertex must have at least 3 mm clearance from both actual road/terrain support
and an actual registered Box3D terrain ray. The two support heights must agree
within 25 mm. Missing, penetrating, nonfinite, overlarge or unsupported fits
refuse the entire action. Visible skin/contact burial from the authored pose is
not repaired or accepted by shrinking the solver collider.
The existing corpse-ignored vertical center-ray census must also show no external
object hits. That bounded rejection is not a convex overlap proof; building-floor
and wall-contact admission remain separate work.

Every joint uses measured shared-selection endpoints (maximum allowed separation
0.12 m), taking a common world midpoint for both local anchors, so initial paired
anchor mismatch cannot induce an immediate constraint correction. Joint Z axes
come from that geometry pivot toward the measured child hull centroid. The spine
uses the two actual fitted torso centroids. These are prototype geometry frames,
not measured human anatomical hinge frames or fabricated RTM translations.

## Ownership and cleanup

The existing single-owner 30-second phase lease owns every body and joint.
Partial creation destroys joints before bodies. A physics-world lifetime epoch
guards cleanup/readback across backend destroy/recreate; stale handles cannot
remove a new world's reused bodies. Probe clearing never owns articulated bodies.
Actual backend counts separately report articulated bodies/joints.

Clear, hide, delete/destruction, revival, attachment, model/move/clock changes,
expiry, invalid readback, excessive travel, entering retained rain water, save
serialization and explicit authored animation clear the rig and return to the
preserved authored state. No backend handle or new pose field is saved/networked.
Returning to authored state can visibly reposition articulated limbs; this is
the implemented experimental retirement policy, not accepted seamless freezing.
Water/soft-ground behavior and remote authority are not implemented.

## Required executable checks

Native tests added to the existing targets check nonidentity affine deltas,
multiweight points, proxy matrices, synthetic tails, nonaccumulation, identity
restore and atomic invalid-delta refusal. Real Box3D tests apply asymmetric impulses,
measure the two actual local joint pivots after stepping, and exercise probe clear,
attached-body removal, generation reuse, stale-joint removal and world recreation.

Before handoff, an isolated focused executable passes **314 assertions in nine
cases**. It compiles the candidate backend/facade and production RtAnimation plus
the two affected test sources, linking unchanged support libraries from the root
build. Six affected production translation units also pass Clang syntax checks.
The optional runner parses, accepts the actual Box3D name with spaces, and rejects
owner changes, partial handle counts, negative clearance and genuine admission
refusal in bounded parser-policy checks. These are source/focused results; the
full committed game/unit targets and installed smoke remain root's gate.

Root builds the committed tree with:

```powershell
cmake --build build/win-x64-clang-rwdi --target PoseidonGame PoseidonTests
build/win-x64-clang-rwdi/tests/unit/engine/Poseidon/PoseidonTests.exe "[corpse-pose],[articulation]"
```

The locked installed runner is:

```powershell
scripts/Test-CorpsePoseProof.ps1 -ArticulationSmoke -Label stock-box3d-articulation
```

The optional path verifies actual backend 0 -> 11/10 counts, consumed bitwise
zero-motion equivalence, a real chest impulse reaching actual Man points and
clear/expiry/hide/delete zero-handle cleanup. It captures before, initial and moving
views with actual clocks and installed pair hashes. It deliberately separates
this solver campaign from the older read-only no-solver diagnostic's exact paused
PNG baseline requirement, without relaxing either default proof.

Installed review must still inspect body/head/shoulder support, visible motion,
weapon attachment, shadows, near/far culling and collision queries. Slopes, steps,
building floors, save/reload, revival, mission restart and other stock rigs remain
open until actual separate receipts establish them. No default enablement is
authorized by source-only results.
