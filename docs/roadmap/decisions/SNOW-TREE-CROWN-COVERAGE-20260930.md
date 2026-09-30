# Individual tree crown snow

Source baseline: 94694933. Implementation is a cosmetic material layer for actual
primary static MapTree/MapSmallTree owners. Installed acceptance is pending;
the root agent owns builds, deployment and the game lock. No new assets, public
buffer layouts, simulation state, save/load fields or network semantics change.

## Source identity and crown envelope

The direct producer requires the current mesh to belong to the actual owner,
with no skinning, CPU pose, dynamic vertex buffer, palette or configured shape
animation. The owner must be primary, static and intact. Only opaque,
depth-writing cutout leaf sections receive the proof. Reflective, glass,
cockpit, monitor and surface-overlay draws are excluded. The retained producer
sets the proof only in BuildGpuInstance for the primary owner, not proxy or
far-authoring DTO constructors; the fragment also requires a cutout section.
Noncutout trunks, bushes, grass and merged forest meshes remain uncoated.
Merged forests need independently bounded per-tree components before admission;
their existing component centroids alone cannot describe a crown envelope.

LODShape::CalculateMinMax combines the loaded model's levels; the published
Min/Max Y therefore remains the same across selected visual LODs. Finite positive
height is required. Reserved conform0.y/z hold model minY/inverseHeight;
conform2.w holds owner proof; conform1.x holds external exposure. These lanes
are unused by rigid/Keep conform modes 0/2. Forest planes (mode1), unknown
conform modes and pinned On geometry are excluded rather than overwriting
their existing plane. ABI sizes are unchanged.

Both vertex paths evaluate the same smooth 45–85% upper-model-height envelope
from the original model position. Wind-displaced positions, camera angles and
the current section bounds never seed it. This is a broad crown approximation,
not imported branch accumulation or a leaf/trunk segmentation mask. Leaf cards
that also depict small branches share that material layer. Solid bark sections
remain protected by the existing alpha classification.

## Physical exposure, bounded work and lifetime

Scene::ObjectForDrawing invokes the optional no-op-default engine hook for
tree candidates only when actual deposit or the effective altitude snowline can
apply, before the Full retained divert. Dry worlds preserve the cached coverage
field read and perform no roof rays. This is a source draw candidate hook, not
GPU visibility readback; the GPU can still reject a candidate later. The direct
leaf path consumes the same renderer-owned proof cache.

An actual query starts at the transformed loaded model upper-center +15cm and
ends 30m vertically above it, radius5cm, ignoring the literal tree owner. Keep
conform adjusts that point by the same SurfaceY/bcSurfaceY rule as drawn
vertices. FireGeometry is queried first, then ViewGeometry on a miss; this
catches roofs such as stock CampEastC whose FireGeometry is explicitly empty.
Only an actual unobstructed result supplies distant exposure. Missing or
pending proof is zero. Discarded/missing sky bakes are never interpreted as open.

The frame budget is eight owner probes (at most16 geometry calls), reset only
when the published main-frame generation changes. Cache capacity is4096.
Proofs bind literal RenderId, shape and the complete transform; they expire
after one second of steady-clock time, including a paused simulation. Expired
positive proof is revoked when a draw candidate cannot obtain the next bounded
probe. Removal and movement erase the lease; capacity eviction revokes its
retained positive before removing it. An unchanged fresh query does not publish
another identical retained instance update. No world registration sweep or
per-frame all-object collision scan is introduced.

This is one coarse external-roof test per crown. It does not resolve partial
roof coverage, roofs more than30m above the sample, per-leaf interception,
neighboring crown interception or all-world deposition history. A loaded crown
top under a view/fire blocker stays dry far away. Within actual zenith-map
coverage the local raw roof result takes precedence, including the cosmetic
feather border. Outside it, only the cached real query supplies exposure.
More than4096 active eligible owners and the eight-per-frame admission budget
can leave pending crowns bare. Those bounds need runtime cost/coverage review,
not a claim that every island tree is accepted.

## Material and controls

The coat uses frame.snow_surface actual deposit and smooth world-altitude
snowline ramp. Zero depth/exposure, excluded sections and lower crown points are
exact zero-coat cases. Leaf alpha discard, A2C, geometry, wind displacement and
prepass normals are unchanged. Snow blends toward the existing powder mean
(linear0.70/0.75/0.80), with no procedural grain or powder-normal perturbation
on moving/subpixel cards. Specular and near-field leaf SSS/ambient boost fade
with coat coverage; the original leaf response returns continuously as it thins.

WGR_TREE_SNOW=0 disables the crown coat and its CPU probe work without changing
GSnow deposit, terrain snow or ordinary object coating. WGR_OBJECT_SNOW=0 also
disables it because the shared raised-surface depth lane is disabled; this is a
broader control. WGR_SNOW_POWDER=0 affects terrain/solid-object detail; crown
cards already use the mean. WGR_TREE_SNOW_TRACE=1 logs up to32 actual model
bounds and512 proof rows, with owner birth, actual model path, direct/retained
path, exposed boolean, world top and remaining frame budget. Trace is a fixture
aid, not performance-comparable telemetry.

## Checks and installed fixture gates

`scripts/Test-TreeSnowSurfacePolicy.ps1` compiles the actual production policy
header and checks finite bounds, every independent owner/leaf gate, one reset
per frame, exhausted budget, changed birth/model/transform, one-second expiry,
and the actual Fire→View transport with literal self-ignore/origin/end.
It passes under clang++ with warnings treated as errors.

Rust tests are authored for actual GPU functions, not a CPU imitation:
tree_crown_actual_gpu_height_depth_shelter_and_ablation checks height/zero
proof/depth/shelter/partial coverage and the compile-definition control.
The existing actual-object-snow GPU probe additionally exercises tree local
roof precedence, exposed local maps, unknown far refusal, proven far positive,
feather rejection and actual smooth world-altitude snowline. Shared shader
composition and source contracts cover both consumers and unchanged alpha,
wind and prepass normals. Root must compile/run these before acceptance.

For a reproducible native starting view, Measure-NativeReforger's already
defined Forest pose is4697.60/3998.02/50.23, heading220.6, elevation-5.0;
ForestHigh uses the same XZ/heading at450m/elevation-25.0. They are candidate
views, not evidence of a specific accepted species. Start with fixed exposure1,
native1920×1080/MSAA4, temporal/DLSS/jitter off, fixed date/time and wind0.
Use actual depth0.18, remove the falling-rate variable, and disable the altitude
line for the deposit comparison. Capture fresh serial dry/treeON/deposit-treeON/
deposit-treeOFF arms through the owned installed process/profile/provenance
pattern; verify actual dev_snow state and the same loaded receiver/proof identity.

Select one actual exposed proof row, bind its literal model and top coordinates,
and use nearby/150–400m/return cameras aimed at that crown. Require a crown
positive at each distance, unchanged trunk/alpha-hole masks, coherent LOD
transition and no glitter. Force WGR_GPU_DRIVEN=0 in a fresh matching arm to
exercise the direct producer and require a direct proof for that actual model;
an unsupported dynamic source is a refusal, not a passing direct test. Then
restore normal wind and collect a short motion sequence at the same sim times
in ON/OFF arms; do not classify moving-weather images as performance samples.

A real covered control needs a loaded tree whose upper-center lies under actual
Fire/View roof geometry within30m. Bind that actual model/proof identity and
require exposed=false plus no distant coat. A small canopy over the tree root
is not that control. Added/deleted roof and moved/deleted tree controls should
hold the camera beyond the zenith map and wait >1s to exercise lease renewal
and revocation, without changing eligibility or supplying fake exposure.
Include snow-off/reset and a pure altitude-line arm around actual crown Y.

Every installed run needs matching EXE/DLL provenance, owned normal exit,
validation-clean logs and before/after binary hashes. The existing roof/terrain
snow fixture remains a separate regression gate; this change does not accept
far roofs, roads, terrain powder quality or footprint behavior by inference.
