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
