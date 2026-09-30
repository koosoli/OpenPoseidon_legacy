# Individual tree crown snow

Source baseline: 94694933. Implementation is a cosmetic material layer for actual
primary static MapTree/MapSmallTree owners. Installed acceptance is pending;
the root agent owns builds, deployment and the game lock. No new assets, public
buffer layouts, simulation state, save/load fields or network semantics change.

Owner appearance rejection (2026-09-30 22:54): the actual Everon screenshot
shows ugly uniform-white crossed-card wedges. Previous crown/trunk/alpha
controls establish bounded coverage only and do not satisfy visual quality.
The pending textured-material correction and remaining all-forest/top-view
coverage are recorded in [SNOW-TEXTURED-MATERIAL-20260930.md](SNOW-TEXTURED-MATERIAL-20260930.md).

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

## Standalone installed capture runner

`scripts/Test-TreeSnowCrowns.ps1` automates a bounded deposited-snow capture.
Its default `-World Auto` tries the stock CWA/Everon candidate first. Only an
explicit fixture-unavailable result before binding a tree permits a fresh native
Eden fallback. Runtime errors, malformed telemetry, unsupported commands or an
already selected tree that fails a control abort the campaign. Use an explicit
world to require that world; `-Model` requires its literal full loaded model path.

```bash
LOCK_OWNER="stock tree snow crowns" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-TreeSnowCrowns.ps1 -World LegacyCwa
LOCK_OWNER="native tree snow crowns" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-TreeSnowCrowns.ps1 -World Native
LOCK_OWNER="direct tree snow crowns" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-TreeSnowCrowns.ps1 -World LegacyCwa -Direct
```

`pwsh -NoProfile -File scripts/Test-TreeSnowCrowns.ps1 -SelfTest` needs no lock,
game, GPU or installed files. Its 27 pure checks cover the exact production
trace grammar, paths with spaces/raw backslashes, literal SQF quoting, finite
bounds and coordinates, owner IDs, budget limits, covered proof, deposited
state/upload and invariant-culture camera calculations. The full runner has
also passed PowerShell AST parsing; no installed run is claimed here.

Each campaign holds the shared lock and uses fresh private profiles and owned
processes for `WGR_TREE_SNOW=1` and `0`. It reads DEPLOYED-FROM first, records
the EXE/DLL hashes, sizes and timestamps, and rejects final pair/stamp drift.
It requires normal exit code0, Shutdown complete and validation-clean logs.
Only its own process can be stopped during failed cleanup. Result JSON,
command/response JSONL, engine logs, provenance and PNG hashes remain under
`build/tree-snow/`.

The target must have a real exposed owner proof, a loaded-model bounds row and
a matching actual SQF nearest object's debug model and XZ. The proof's upper
center is used only to find and aim at that actual tree; no geometry is spawned.
The upright, recentered fixture requires origin XZ within25cm of that center.
Native geometry with an offset center can therefore refuse instead of silently
binding a different tree. The OFF process must bind the same model, bounds and
actual ASL within2cm, and must issue no crown roof probes. RenderId births are
recorded separately; cross-process RenderId equality is not a valid identity test.

At fixed simulation time100, date1985-06-21 16:00, exposure1, wind/rain/cloud0,
snowline off and temporal/jitter/grass off, the runner enables actual depth0.18
without falling snow. Cameras aim at the bound crown at30m,150m,400m, from
above and on return. Terrain height is queried for each derived perspective
camera rather than guessed. It also captures the disabled state and restored
near/far state, checking stored depth/chunks throughout. Current target ASL and
paused time are checked for every screenshot. The trace is bounded to32 models
and512 proof rows, so it establishes the initial admitted positive, not a fresh
lease at every capture; no shelter/lifetime acceptance is inferred from it.

The central60% image metric requires a nonzero ON/OFF difference at30/150/400m
to reject a completely unused coat. Return, restore and top-view metrics are
retained for inspection. That broad region includes terrain and other objects;
it is not a crown, trunk or alpha-hole segmentation mask and is not a visual
threshold for quality. Completion status is `captured-not-visually-accepted`.
Review those masks, distant whiteness, LOD continuity and temporal glitter before
accepting appearance. Real covered/moved/deleted controls, wind sequences and
altitude-only snow remain the separate gates described above.

## Installed original Everon crown inspection

Matching06a7c41e `build/tree-snow/stock-crowns-bound-20260930-220101-2790bb`
completes both ON/OFF source, state, actual identity, capture and lifecycle
controls. Actual target is `5242: str habr`, loaded `data3d\\str habr.p3d`,
ASL `[4324.11,4341.45,62.3367]`; ON has a real retained exposed upper-crown
owner query and OFF binds the same model/position without crown probes. Both
exit normally0, with Shutdown complete and unchanged installed pair. Captured
native renderer diagnostics report current physical-map ready/submitted state.
Root inspects30m and400m ON/OFF: upper individual crowns visibly whiten, lower
foliage stays coloured, lower trunks stay brown and cutout holes remain. This
accepts bounded still-image crown visibility on this model; broad central image
metrics alone are not its proof. Temporal wind/LOD transitions, covered/moved
controls and merged forest crowns remain open. Camera aim uses loaded model
height rather than per-instance scaled extent, so centre-pixel assumptions
must still be checked against the actual silhouette.

The earlier `stock-crowns-current-20260930-215220-a3eae1` remains failed on a
live-log sharing conflict; the following `stock-crowns-sharedlog-20260930-215657-603df7`
remains failed because it passed ASL into terrain-relative nearestObject and
therefore could not bind a target. Both owned games shut down normally. The
runner now reads complete log lines with explicit shared read access, pins and
restores the physical-map setting, preserves stdout/stderr, and searches the
proven XZ at terrain-relative0 before requiring actual model/ASL identity.
All27 source/helper checks pass; these repairs do not weaken admission gates.
