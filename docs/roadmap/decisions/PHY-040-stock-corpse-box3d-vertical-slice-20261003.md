# PHY-040: explicit stock corpse Box3D vertical slice

Source baseline: `codex/streaming-residency` `94f261c5`. This candidate is an
off-by-default developer action. It is not the default death behavior, a network
feature, or accepted general corpse contact quality. Root owns build, installation
and actual-game verification; a passing source compile does not close those gates.

## Actual consumer and physics connection

`triPhysicsShowcase "corpse-capture"` retains the actual authored affine phases.
`"corpse-prepare"` preserves an existing world and its terrain. If no world exists,
it requires the actual local held corpse and registers a bounded 129x129 height
field sampled from real `SurfaceY` at 0.25 m spacing around that owner (32 m square).
The field is clipped to the actual landscape domain; invalid bounds/heights or
missing terrain are explicitly refused. This is local prototype support, not a
worldwide contact claim. Its unchanged hull admission still compares every fitted
vertex with both actual roadway support and the registered solver terrain ray.
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

The first installed articulated campaign on combined source `254` failed before
creating any bodies: `stock-box3d-articulation-20261003-180700-891415/engine.log`
ends in `std::_Allocate_manually_vector_aligned` from `RunPhysicsCorpus` immediately
after backend creation. That map-wide debug corpus uses twelve subdivisions of
the object grid. The actual native boot receipt at engine.log:5890 records land
1024x1024 at 12.50 m and terrain 2048x2048 at 6.25 m. The corpus formula consequently
requests 602,898,916 bytes (12277 squared times four), compared with 37,478,884
bytes for the classic 256 object grid. `LandGrid` is a dynamic landscape getter,
so its spacing follows the actual 12.50 m object grid; the fault is allocation
scale, not a hardcoded 50 m spacing.
`MemHeap::Alloc` refuses a single block >=256 MiB, while the replaced global
`operator new` forwards a null failure into STL's aligned-vector bookkeeping.
This source combination explains the allocation-site crash on a larger grid;
the general null-returning global-new contract is a separate allocator defect.
Preparation no longer calls that corpus: its fixed input storage is 66,564 bytes,
and backend copying/quantization sees only that same bounded real surface.
The new preparation log records actual source terrain/object grid dimensions and
spacing, so the next installed receipt can substantiate the failing-map scale.
No contact threshold was relaxed. Installed rerun remains required.

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

The bounded preparation rerun `bounded-stock-box3d-articulation-20261003-182318-a5152c`
exited normally but refused the contact fit before creating bodies. The precise
installed receipt `precise-stock-corpse-fit-20261003-184317-108b15/engine.log`
then established the lowest vertex: part 4 `ploket` (right forearm), at world
(6530.84277, 160.979828, 6467.77881), actual road/terrain Y 160.98262 and solver
Y 160.98259. The measured -2.792 mm clearance is a real collider fit issue;
the two floor providers agree within 30 micrometers. All 296 fitted vertices
were measured, with maximum root distance 1.096 m.

The next candidate calibrates only each inset collision proxy's vertical local
offset. If its measured minimum clearance is below 3 mm, it requests enough
offset for 3.25 mm, including a 0.25 mm floating-point rounding reserve. An
offset above 10 mm is refused; the original every-vertex 3 mm support, 25 mm
provider agreement, finite and spatial bounds checks remain mandatory after
calibration. The original body origin, dimensions, authored skin and shared
joint anchors remain unchanged. For this measured forearm the requested offset
is 6.042 mm. This still does not correct visible authored skin burial.

The direct C witness `tests/experiments/box3d_contact_proxy.c` measures local
mass-center Y 0.00604235847 after custom 2 kg mass assignment while actual world
transform origin remains exactly unchanged. The actual backend native case
`[corpse-contact]` passes 69 assertions at the installed receipt's world location:
every fitted vertex clears the real elevated solver terrain, body-origin
readback and axes are exact, nonrigid affine weighted points/proxies/palette
retain zero-motion equality, two real constrained offset pieces remain finite
with paired joint pivots within 25 mm after 120 solver steps, and all ownership
is retired. These source/private native checks do not substitute for the next
installed admission and consumed-pose test; that result is still pending.

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
