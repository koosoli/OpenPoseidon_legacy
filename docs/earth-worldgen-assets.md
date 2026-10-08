# Actual CWR-Worldgen asset placements in Earth

This optional path reads an actual local CWR-Worldgen build rather than selecting
new proxy models from OSM tags. It references that build's RVW4 P3D placements:
stock trees/forest blocks, stock or generated roads, and stock or generated
buildings. Models, textures and retained LODs come through the existing installed
asset bank. No game assets or generated output are bundled in this repository.
This is a bounded presentation integration, pending installed visual acceptance.

Source inspected: CWR-Worldgen
`b46124fc9c810773b5402633168af4829240f143`, especially `wrp.py`,
`model.py`, `osm.py:BboxProjection`, `asset_mapping.py` and `milestone9.py`.
The upstream source is GPL-3.0-or-later; this adapter independently implements
format interoperability and does not copy its generator, styles or textures.

## Source and activation contract

Set `POSEIDON_EARTH_WORLDGEN_MANIFEST` to the generated output's `manifest.json`
or a prepared region index before starting an Earth-enabled game. Restart to
load changed outputs or index metadata.

The adapter reads these existing Worldgen manifest fields:

- `world.name`, `world.cells`, `world.cell_size`;
- `world.bbox`: `[south, west, north, east]`;
- top-level `height_scale`.

The current packaged source is `source/<world.name>/<world.name>.wrp` below the
manifest directory. Older flat `source/<world.name>.wrp` outputs also work.
The safe world name cannot traverse paths. The file must be uncompressed RVW4:
`4WVR`, two little-endian int32 dimensions, two int16 grids, 512 texture paths
of 32 bytes, then 128-byte object records (`12 float32`, int32 ID, 76-byte model
path) and the empty final record. The adapter reads only the elevation and object
data. It requires ASCII relative `.p3d` paths and finite positive affine bases;
duplicate IDs, malformed lengths, invalid bounds and unsafe paths reject the
source with a diagnostic. A non-crossing geographic rectangle within ±84° is
required, matching the upstream bbox contract.

The game's normal mod arguments must also mount the generated addon/PBO and its
dependencies, alongside the user's existing stock installation. Merely pointing
at a manifest does not install generated model or texture files. The parent task
owns the preparation and launcher integration. Do not also enable cached proxy
roads/trees/buildings for the same region when judging the actual assets.

For a Worldgen output with extent `cells * cell_size`, source X/Z map exactly
through the inverse of Worldgen's bbox projection:

```
longitude = west  + X / extent * (east  - west)
latitude  = south + Z / extent * (north - south)
```

The object origin's height relative to Worldgen's quantized, bilinearly sampled
source grid is retained. That offset is applied along the spherical up direction
above the current resident Earth triangle height. Native Earth collision heights
remain unchanged. Source affine columns are mapped with the geographic east/
north scale and rotated into the active ENU frame. This preserves authored road
pitch and forest shear; bbox aspect/stretch in the generated map is converted
back to geographic dimensions. The model origin is retained as authored, with
no additional bounding-centre correction or native-island grounding.

## Bounds, threading and lifecycle

Manifest/index metadata is read once on a value-only worker. Limits are 4 MiB
manifest, 32 MiB RVW4, 2,048 grid cells per axis and 131,072 source object records. Each
snapshot selects at most 2,048 nearest covered objects and 64 model paths.
Coverage means the object's base lies on valid resident ground. Selection is
deterministic by distance, then source ID. Snapshots carry geographic revision
and terrain generation; obsolete worker results cannot publish.

The main thread admits at most 32 object attempts and two new top-level model
names per frame. Dependent texture/proxy work remains the existing asset bank's
responsibility; this is not a hard frame-time or process-memory guarantee. Models
over 250,000 LOD-zero positions are refused. Missing/unsupported models are
reported and suppressed for the rest of the snapshot. The Earth registry has
separate IDs and ownership from native far objects; shared material texture
pins/liveness and the existing GPU shadow gate include these owners.

Worldgen objects use rigid retained mode with their supplied full affine frame.
Shared ClipLand model selectors remain available to native OFP objects, while
the Earth owner never samples the unrelated native island heightmap. Relocation,
changed terrain generation, altitude at or above 12 km and teardown retire the
Earth instances. Normal OFP instance admission and terrain remain unchanged.

## Verification and actual remaining limits

`scripts/Test-EarthWorldgen.ps1` exercises the actual RVW4 wire layout with tree,
road and building model identities, relative-ground placement, forest shear,
geographic frame rotation, resident exclusion, selection/model caps, malformed
input/path rejection and latest-request worker publication. `-Manifest` also
reads a real generated dataset and builds a bounded CPU resident snapshot.
No GPU, source download or dependency installation runs in this test.

Installed acceptance must show both `Earth Worldgen publication` with no error
and `Earth Worldgen installed assets` with nonzero `created`. Inspect the actual
trees, roads and building models, their grounding and depth against the Earth
terrain; a positive instance count alone is not visual acceptance. Missing model
and texture diagnostics must also be checked. Rebase and move out of resident
coverage to verify retirement/republication; above 12 km the near assets retire.

This connection loads up to two prepared regional Worldgen outputs from an
explicit offline index. It is not a worldwide generated dataset or an automatic
regional generation/distribution service. Beyond the indexed coverage, more
licensed outputs and an appropriate distribution source are still needed.
The existing optional feature JSON backend supplies input features,
not generated P3Ds or Worldgen placement records. Generated texture/material
styles, terrain grading, road network/navigation, enterable building simulation,
destruction and object collision are not transferred by retained rendering.
Nested proxy completeness and forest conformity require actual model-family
acceptance. Source road/forest pitch/shear is retained; differences between the
generated terrain and resident DEM can still cause local fit errors. No global
playability or parity claim follows from this adapter.

## Prepared region index and camera residency

The same environment input can name this versioned, offline JSON index:

```json
{
  "earth_worldgen_regions": 1,
  "regions": [
    {
      "id": 1,
      "manifest": "lux-region-01/generated-compatible/manifest.json",
      "bbox": [49.6035, 6.12, 49.6165, 6.14]
    },
    {
      "id": 2,
      "manifest": "mamer-worldgen-01/generated/manifest.json",
      "bbox": [49.59, 6.00, 49.64, 6.08]
    }
  ]
}
```

The index is <=64 KiB with 1–64 entries. Stable integer region IDs are 1–255 and
must be unique, as must manifest paths. Paths are relative to and contained
within the index directory; absolute paths, traversal and escaping symlinks
are refused. Each finite, non-antimeridian-crossing bbox must match its actual
manifest within 1e-9 degrees. The manifest continues to define the source grid,
height scale and exact inverse Worldgen geographic projection. The index does
not reinterpret a region's source height datum or change resident collision.
Mount the necessary local generated addons/dependencies before launch.

The worker chooses at most two camera-containing/nearest regions within the
current resident patch's reach plus a 512-metre margin, bounded to 20 km. It
evicts unselected sources before loading replacements. No other indexed WRP
or game models are loaded. Requests remain keyed to geographic revision,
ground generation and 256-metre camera cells; cancelling during a file load or
build prevents stale publication. Travel with a moving near patch and later
geographic rebases both select replacements. Outside prepared coverage the
snapshot is empty; this does not invent a populated world.

The two sources share the existing 2,048 selected-instance and 64-model caps.
The source-record limit was raised from 65,536 only because the actual Mamer
5.8 km output has 98,421 placements in a 12,876,556-byte WRP; the 32 MiB file and
rendered-instance limits remain. Source value storage is estimated conservatively
from height/object vector capacities and model strings and bounded to 32 MiB per
source, 64 MiB total. Runtime logs report valid resident region IDs, wire bytes
and value bytes. These bounds exclude temporary JSON/parser/sort allocations,
asset-bank allocations and the rest of the process; they are not a process-memory
or frame-time guarantee.

Indexed placements receive a stable uint32 renderer ID:
`(regionId << 24) | rank`, where rank is the zero-based order of the original
unique authored source IDs. This survives source-record/index reordering,
eviction/reload, camera movement and geographic rebase. Single-manifest mode
retains authored IDs. Sources are immutable during a run: size/mtime metadata
is checked around reads and pinned per visited region, so detected replacements
are refused after eviction. This detects ordinary regeneration, not malicious
changes preserving both size and modification timestamps.

For overlapping prepared rectangles, the lowest-ID **valid loaded** source owns
placements whose bases lie in its bbox. Higher-ID placements in that overlap are
suppressed, avoiding double buildings or vegetation. Missing, invalid or changed
sources cannot suppress valid ones. This policy assumes each prepared output
actually covers its declared rectangle; it does not infer per-feature holes or
merge independently generated roads across a boundary. A source unavailable at
load time remains a reported error until evicted/revisited or restarted. Source
preparation and generated asset mounts remain the operator's responsibility.

CPU tests cover ordinary camera travel, eviction/reload, subsequent rebase,
stable sparse-ID namespaces, global instance/model caps, overlap ownership with
invalid/missing sources, modified-output refusal, dateline-near selection and
index path/bounds/count rejection. `Test-EarthWorldgen.ps1 -Manifest` accepts
either a manifest or an index for actual prepared-source CPU parsing. Installed
acceptance must still check actual model visibility and boundary transitions.

## Installed two-region residency check

`scripts/Test-EarthWorldgenRegionsInstalled.ps1 -SelfTest` checks its receipt
parsers and rejection controls without accessing the game. The installed run
requires the matching deployment's `-ExpectedCommit` and the shared game lock:

```bash
LOCK_OWNER="Earth Worldgen region acceptance" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-EarthWorldgenRegionsInstalled.ps1 \
  -ExpectedCommit <installed-source-commit>
```

The default index is the external lab file
`D:/OpenPoseidon-Worldgen-Lab-20261008/worldgen-regions-complete.json`. Prepare
exactly two entries under that common directory: ID 1 references
`lux-region-03/generated/manifest.json` with bbox
`[49.6035,6.12,49.6165,6.14]`; ID 2 references
`mamer-worldgen-complete-02/generated/manifest.json` with bbox
`[49.59,6.00,49.64,6.08]`. Use the region-index schema above. Both paths must
remain relative and contained. Override `-Index`/region IDs for another prepared
index matching these two geographic fixtures. Inputs are hashed before and after
the run. The script neither generates source regions nor downloads missing DEM
tiles; the cached Earth windows for both poses must already be available.

Mount only the complete Mamer `@CWR-Milestone4/AddOns` archive root (the default
`-AddonRoot` accepts either the mod directory or its `AddOns` directory). Its
generated water-tower model uses the same `earth_lux_worldgen` PBO prefix as the
city output. City model references are stock assets, so simultaneously mounting
both same-prefix PBOs would introduce ambiguous lookup without adding required
models. The original installed stock assets remain required.

The bounded run captures city at origin `[49.61,6.13]`, Mamer at
`[49.615,6.04]`, then the same city view. Each geographic Go must invalidate the
previous ground revision, receive fresh resident elevation, and complete model
admission for that exact revision/generation. It also holds and releases actual
SDL D once across a 256-metre request-cell edge before the first capture, proving
ordinary camera movement can refresh the selection without geographic Go.
Captures wait 30 rendered frames after admission and verify actual camera
position, view direction and 5–30-metre ground clearance. Logs must show the
requested region, city-source retirement at Mamer, a globally bounded 2,048
selected/created placements, successful road/house/tree model registration and
zero publication/model errors. Returning to the same city pose must reproduce
its source, covered, selected, created and resident-region counts.

The process owns one PID, exits it cleanly and verifies the deployment stamp is
unchanged. `build/earth-worldgen-regions-installed/<run>/result.json` and three
PNG files retain the evidence. Region dictionary keys serialize as decimal
strings, with numeric `id` values preserved. A `passed: false` receipt is written
before launch and after each completed capture; the final receipt alone can
record all teardown and unchanged-source checks as passed. JSON conversion
finishes before replacing the previous checkpoint, preserving it on a later
serialization failure. A diagnostic PASS does **not** accept their
visual quality, object collision, road navigation or upstream playability.
The runtime bridge exposes counts and resident region IDs, not individual
placement IDs or retired GPU slots; their stable namespace behavior remains
covered by the CPU tests, not claimed by this installed receipt. This finite
check covers two offline prepared outputs, not worldwide populated data.

## First actual offline region receipt

On 2026-10-08, upstream M4 at the source revision above generated a Luxembourg
region from existing cached OSM and Terrarium data, without network requests.
The source rectangle is `[49.6035, 6.12, 49.6165, 6.14]`, with 128 cells and
11.2582600703422 metres per source cell. The local lab output is
`D:/OpenPoseidon-Worldgen-Lab-20261008/lux-region-01/generated-compatible`;
its `manifest.json` and `@CWR-Milestone4` mod root are the adapter/launcher inputs.
The actual WRP SHA-256 is
`94962459cf79c5cb75046ff78ce6e6fcab0386fd40d48d01ea3bdd6fb925dcca`.

M4 initially failed because its bridge policy called `dataclasses.replace` with
fields absent from M4's `PlayabilitySpec`. The local upstream compatibility
guard uses that replacement only when both bridge fields exist; otherwise it
uses the already existing read-through stock-bridge proxy. The external lab
contains the exact patch and command receipt. No feature family was disabled.
Twelve upstream bridge-policy tests and the M4 compatibility probe passed.

The actual CPU adapter test parsed 7,369 objects, covered all of them in its
synthetic resident patch, selected 2,048 and skipped zero model names. Source
totals are 1,057 road pieces, 1,249 houses, 2,950 single trees, 2,066 bushes and
47 forest blocks across 16 model paths. Upstream verified all 17 asset records
with zero missing required models. This proves source interoperability, not
installed model rendering or grounding.

Upstream `final_validation.status` is **failed**, despite generation exit zero
and `deployment_allowed: true`. Failures include 386 of 547 road-junction
connections, 3.854 metres maximum road-axis overlap, sampled road grade worsening
from 43.91% to 127.79%, and a town-name catalogue containing only two places.
The adapter exposes the supplied known validation status in publication logs.
Use this output as an asset-presentation fixture; it is not a validated playable
island. The local lab's `worldgen-camera-poses.json` supplies three record-derived
road/house/tree views and nearest-selection counts. Their heights use cached DEM
samples: installed acceptance must query current resident ground before capture.

## Bounded installed presentation harness

`scripts/Test-EarthWorldgenInstalled.ps1` defaults to the actual region-03
`generated/manifest.json` and `generated/@CWR-Milestone4` in the external lab.
The parent/operator runs it only after a matched integrated deployment, through
the shared game lock:

```powershell
./scripts/Test-EarthWorldgenInstalled.ps1 -ExpectedCommit <integrated-commit> -Manifest D:/OpenPoseidon-Worldgen-Lab-20261008/lux-region-03/generated/manifest.json -AddonRoot D:/OpenPoseidon-Worldgen-Lab-20261008/lux-region-03/generated/@CWR-Milestone4 -CacheDirectory <existing-Earth-cache>
```

This launches one owned empty Earth fixture at geographic origin 49.61/6.13,
with the generated mod mounted through `--addon-root`, local imagery enabled and
cached proxy buildings/features disabled. It uses an isolated graphics profile
and verifies actual native 1600×900, 4x MSAA, no upscaler. Invalid local network
proxies enforce a cache-only fixture; no shader, imagery or data preparation
downloads run. The existing supported `-PrepareOnly` helper writes the empty
fixture below the caller's cache.

Three record-derived poses query current resident ground before placing the
camera 15 metres above it, facing headings 5.56°, 220.875° and 237.315°, with
pitch −35°. Each pose waits for a fresh matching-revision/terrain-generation
Worldgen publication and terminal admission log (`attempted == selected <=
2048`). Successful retained model logs must include stock tree, road and house
families in that publication. Missing models and any created/attempted shortfall
remain explicit receipt fields; positive source-parser counts cannot satisfy
this gate. It saves each actual camera, ground, model identities, screenshot,
weather and published engine performance, then checks clean exit, unchanged
deployment/source manifest, no elevation downloads and no GPU/runtime errors.

The receipt carries upstream's failed validation status unchanged. Its pass
means these presentation gates completed, with `visualAccepted=false` and
`playabilityAccepted=false`. Inspect road fit, tree/forest grounding, houses,
textures and depth in all images before accepting appearance. Retirement,
rebase, collision, navigation and upstream road validation are outside this
three-pose smoke test. `-SelfTest` runs only parser and failed-registration
negative controls, without accessing the game or GPU.

## Earth-only stock road coverage admission

The installed `0e4e1ed4` presentation attempt at
`build/earth-worldgen-installed/20261008-153940-3ce881` correctly failed its road
registration gate: 1,885 of 2,048 attempted objects registered, including real
trees and houses, while `o/road/sil25.p3d` and `ces25.p3d` were not retained.
The actual sil25 section diagnostic was `special=0x24102 blend=1 surface=1`,
texture `o/road/sil_new.paa`, alpha class 2 and `owned=0`. Road GPU vertex-buffer
creation succeeded. The shared ownership contract deliberately excludes
alpha-blended OnSurface geometry; ClipLand loading was not the blocker.

The bounded correction deep-copies each selected stock `o/road/*.p3d` shape
into a dedicated Earth identity, retaining its authored source geometry,
textures, materials and LODs. An explicit Earth-road registration argument
admits only visible textured alpha-blend OnSurface sections of that owner, with
successfully uploaded texture and opaque section material. Native shared shape
identities and the CPU/retained ownership predicate remain unchanged. The
independent copies retire with the Earth publication and use ordinary retained
texture pins and model teardown.

This route uses the existing retained cutout variant with alpha reference 0.5;
MSAA enables the existing alpha-to-coverage behavior. It approximates the
original smoothly blended road edges, and a one-sample profile uses hard cutout.
It does not add a retained translucent/decal pipeline. A 0.02 metre offset along
the authored road up axis replaces the unavailable native OnSurface depth bias;
authored affine geometry and relative resident placement remain otherwise
unchanged. It does not grade the resident DEM or cure the upstream road-junction,
overlap and grade validation failures. Registration and edge/ground-fit images
must pass the strict installed harness before accepting this correction.

`scripts/Test-EarthRoadCoverage.ps1` covers actual stock identities and captured
section facts, with negative controls for native owners, other model families,
unsafe paths, glass/default surfaces, missing texture uploads, hidden sections
and translucent material. Production C++ syntax checks are CPU-only; they do
not establish a successful GPU draw. The installed harness now also attempts a
bounded native screenshot on failure while retaining the failed verdict.


## Resident Earth ground shading follow-up

The installed `ea71483b` three-pose run at
`build/earth-worldgen-installed/20261008-160045-77ebe8` passed actual retained
registration (2,048/2,048 at every pose, no model errors), including roads,
trees and houses. Its images still show uniformly olive near ground. This is a
separate presentation defect from upstream's invalid road connections.

The Earth material's early shader return used its coarse overview albedo and
global sun/sky alone. A 4,096 by 2,048 global overview has kilometre-scale pixels;
it cannot supply ground detail. The resident DEM already supplies height-derived
slope/curvature normals. The material return also bypassed the ordinary scene
shadow and depth-AO receivers, along with the finite native island maps which
must remain excluded from Earth.

An explicit internal `SubmitEarthHorizon(..., bool groundDetail=false)` argument
now marks only the horizon/resident ground submissions when the caller opts in.
The two ground calls in `TerrainWgpu.cpp` must pass `true`; globe, geographic
feature and proxy-building calls keep the default. This adds no public ABI,
texture or vertex layout. Previously unused Earth local-light lanes carry a
512-metre high/low ECEF origin and the marker. The detail evaluates absolute
metre coordinates in that geographic frame, independently of camera movement;
the split prevents Earth-sized float cancellation. Three fixed bands at 8 metres,
1 metre and 0.125 metres add bounded neutral reflectance and analytic tangent
normal variation. Pixel-footprint attenuation removes unresolved bands before
Nyquist; distance attenuation is exactly neutral beyond 180 metres. These are
procedural surface irregularities, not sourced land cover or biome classification.
The current implementation uses at most 24 lattice corner hashes per nearby
fragment and adds no detail texture reads or fog work.

Only marked ground within 400 metres receives existing camera-relative scene
CSM and screen-depth GTAO, fading between 200 and 400 metres. GTAO multiplies
ambient light; shadows multiply direct sun. Their existing no-cascade/AO-disabled
readiness guards remain in force. Source inspection establishes that
`SetShadowCascades` captures the scene camera origin, passes camera-relative
cascade matrices, and shares that origin with retained caster culling/depth;
`shadow_strength` consumes camera-relative receiver positions. This establishes
the intended coordinate contract, not actual shadow presence in this Earth
fixture. Native finite-heightmap cave, GI, wetness and terrain-shadow receivers
remain excluded. The global sphere's material behavior is unchanged.

CPU verification: `scripts/Test-EarthGroundDetail.ps1` checks origin split
precision at the dateline and supported latitude extremes plus two-frame ECEF
reconstruction within two centimetres; Rust
`earth_ground_detail_` tests validate the actual production shader through Naga,
check bounded noise/analytic gradients and split invariance, and verify exact
minified/distant neutral endpoints. Existing native-material preview/tangent
shader composition variants also pass. Production `EngineWgpu.cpp` passes single-TU
syntax validation. These checks create no GPU device. The parent/operator still
needs the integrated installed three-pose images and engine frame-cost comparison
with identical scene/settings. Inspect actual tree/house cast shadows, contact AO,
slope readability, grain stability during translation/rebase, and minification;
shader compilation alone cannot accept their appearance or cost. No new ground
shading runtime or visual acceptance is claimed at this source checkpoint.
