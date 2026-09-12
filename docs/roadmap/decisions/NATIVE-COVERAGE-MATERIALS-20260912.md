# Native coverage, materials and receiver cost

Continuation baseline: asset-obs-001 01e7ce95, identical to safety/consolidated-v2.
Installed engine was 5f1538dd (mission-only changes since then). Work in asset-obs;
foreign Cargo.lock and scratch directories unchanged. Historical measurements
in earlier decisions are not evidence of completed far coverage.

## Confirmed coverage gap

Forest-resume-baseline, Measure-NativeReforger, installed 5f1538dd:
1,296,350 native stream placements / 1,773 models, 1024x1024 land cells at 12.5 m.
Resident 19,999 / budget 20,000; effective desired radius 45 cells, about 563 m.
No pending preparation remains at capture. This is a residency limit, not proof
that a distance LOD is missing from an already resident model. Increasing the
view-distance setting alone cannot populate this absent geometry.

LandLoadEnfusion builds complete XOB LOD ladders, but LandSave's desired set is
distance-sorted and count-budgeted. The generic far renderer exists, yet only
LoadOprwModern publishes its proxy set, not native Enfusion. Its untextured cards
and prisms are not equivalent to the requested authored low-quality mesh LODs.
Do not claim a count-budget increase or grey proxies solve native far quality.
Required next integration: visual-only coarse authored geometry admitted through
the existing worker/model cache and retained renderer, explicit exclusion when
the normal instance is resident, model-shared materials, bounded publication and
world-generation retirement. Do not instantiate a million simulation Objects.

## Ground normals: integrated, broader visual acceptance open

65738f02 connects native NHO maps to TerrainWgpu's existing tile_normal slot.
The September 10 DDS tests already established RG normal packing; the terrain
code still returned a flat normal and its comment incorrectly called packing
unknown. Raw RG sampling, world-space authored ScaleUV, mips and existing terrain
lighting/snow are reused. Height and AO are NOT interpreted as albedo or normal.
No geometry, collision, shader ABI size or original asset changes.

WGR_ENFUSION_GROUND_NORMALS=0 restores the previous path at next world load.
Normals fade with the authored detail band and are not fetched beyond it.
Sources share a per-world cache, capped at 512px like the existing detail layer.
Five targeted terrain tests pass, build passes; installed same-build off/on
Forest runs both exit cleanly and bind 0/22 versus 22/22 NHO palette entries.
Evidence: build/native-reforger-perf/Forest-ground-normal-{off,on}.
Both have 22,360 registered, 6,463 main instances, 15,756,906 main triangles.
Tracked allocation 1603.896 -> 1629.230 MiB (+25.333 MiB, not whole-driver VRAM).
Endpoint terrain colour 0.89 -> 0.90 ms; GPU total 31.523 -> 32.338 ms.
One ordered pair: no established FPS/load-time benefit or causal slowdown.
Forest images inspected, no obvious corruption. Close-ground light-direction,
snow, grazing slope and moving-camera comparisons remain necessary. This does
not resolve tree contrast or far-object coverage.

## Receiver filter experiment: not enabled by default

WGR_COALESCED_PCF=1 composes the shared shadow receiver with a sixteen-fetch
version of the existing nine bilinear tent taps (formerly 36 depth loads).
Only the exact one-texel filter and unclamped receiver gradients qualify;
other widths, non-square maps and steep gradients keep the original kernel.
No cascade resolution, caster geometry or filter footprint reduction. Scalar
oracle covers texture edges, fractional UVs and slopes; shader composition covers
direct, retained, terrain and grass receivers. Both tests pass. Real-device and
installed A/B timing/image acceptance pending; do not advertise an FPS gain.

Installed 4ad82a79, Forest-pcf-off/on: both lifecycle checks pass, same geometry
counts and native 1920x1080/MSAA4 profile. GPU endpoints 34.295 / 33.732 ms;
solid colour 1.88 / 1.66 ms but cutout colour 3.19 / 3.61 ms. Mixed single-pair
result, not an accepted improvement. Images have no obvious new shadow artefact;
wind/cloud time prevents interpreting the full image difference as filter error.
Keep this experiment OFF, and distinguish receiver work from cascade-3 casting.

## Native leaf cavity: opt-in material experiment

WGR_NATIVE_LEAF_CAVITY=1 consumes preserved NTC alpha for foliage ambient only,
from the existing normal sample, in direct and retained paths. No albedo contrast
curve, direct-sun change, new texture or geometry. Only BC7 `_ntc` sources qualify:
BC5 lacks auxiliary alpha and the decoded DDS path currently overwrites it with
normal R. Those deliberately retain the old appearance until auxiliary channel
preservation is separately validated. Flag words are reused without ABI growth;
direct bit 1024 avoids the existing 256/512 depth-bias bits.
Shader composition and direct-flag regression tests pass, full build passes.
Installed image/timing acceptance pending. This is not a completed tree fix.

Forest-cavity-off passed. Forest-cavity-on saved its image and metrics, then hung
in shutdown and failed the 300 s lifecycle watchdog; do not accept its timings.
The dump places main in ObjectStreamPreparer destructor/join and a conversion
worker in GTexMaterialBank.New -> AssetCache.Insert -> unordered_map insertion,
called from ShapeAdapter.cpp's MLOD FindSections material recovery. That path
violates the explicit tables-supplied/no-bank-calls contract. This is a CPU bank
ownership defect, not evidence of a GPU shader hang. The visual effect in the
saved forest image is modest; cavity remains OFF pending proper acceptance.

Correction: BuildAdapterBankTables resolves missing-section material paths on
the main thread into retained mlodSurfMats rows. Worker recovery only reads those
rows; the synchronous no-tables path retains its old bank lookup. No second bank,
global lock, shader downgrade or removed authored material. Full build passes;
native repeated-load/shutdown verification is next. Dedicated MLOD concurrency
unit coverage is still needed in addition to the existing ODOL-oriented tests.

3392c9b2 installed: bank-fixed-cavity-on repeats 1/2 and bank-fixed-cavity-off
repeat 1 all pass capture and clean shutdown. Same 22,360 registered, 6,463 main
instances and 15,756,906 main triangles; visible native materials remain bound.
GPU endpoints on 33.189/35.511 ms, off 33.517 ms: no established performance gain
or material-cost regression. This verifies the reproduced workload, not every
possible concurrent load/exit interleaving. Cavity remains an opt-in experiment.

Important profiling caveat: the controlled benchmark pins governor range to 1,
so min=max and all 6,463 survivors here select LOD 0. This is not evidence that
normal gameplay never selects a cheaper LOD. A normal-range-16 capture is a
separate representative run, NOT a same-quality performance comparison.
Normal-range run passes: 282 instances at LOD0, 6,129 at LOD1; 7,560,031 main
triangles, GPU endpoint 24.044 ms. Confirms existing LOD selection works under
the normal governor. Neither the lower geometry nor its speed is a new feature.

Native-bank unit regression now executes the real MLOD conversion on a worker
with dummy texture and an explicitly prepared material. It checks exact pointer
identity and an intentionally missing table entry (must not fall back to the
global bank): 7 assertions pass. Existing non-degenerate ODOL fixture passes 71
assertions. PoseidonTests and shader-composition builds pass.

Follow-up channel semantics: Bohemia's primary texture reference explicitly
defines NTC cavity as affecting diffuse AND ambient, unlike NMO AO:
https://community.bistudio.com/wiki/Arma_Reforger%3ATextures
The first ambient-only experiment was conservative, not full native semantics.
The opt-in now also scales foliage sun/local diffuse and local ambient, without
darkening emissive/specular or changing other materials. New installed A/B pending.

d7268a32 installed diffuse-cavity off/on both pass lifecycle. Identical 22,360
registered / 6,463 main / 15,756,906 triangles, native 1080p MSAA4, pinned governor.
Endpoints 34.613/34.878 ms are within the prior run-to-run spread, not a proven
cost or benefit. Inspected images still show the broad pale forest appearance:
the cavity change is not a sufficient solution. Keep it OFF rather than claim
the user's contrast request complete. No further parameter sweeps on this path.
Ground normal mapping remains ON; the MLOD worker bank-boundary fix is ON.
Far coverage still requires the authored visual-only integration described above.

## Disk housekeeping interruption

User's can-this-be-deleted contained the still-required .tmp-dlss-sdk. Restored
that SDK to the main checkout's expected path; build/deploy need it. Verified 51
a2-addons PBOs against inspiration/officialarma with SHA256 before deleting that
duplicate subtree only. Much of its 6.178 GiB was hardlinked, so logical size is
not recovered capacity. Old images, scripts and sources remain. Requested build
relocation to D was blocked by execution policy before any copy/delete; current
build directory remains on C and no junction was created.
Later free-space check: C 7.18 GiB, D 366.69 GiB. No immediate build migration
needed; remaining can-this-be-deleted content is about 1.122 GiB of historical
test material and has not been deleted. Do not equate the original logical
duplicate size with an independently measured physical-space saving.
