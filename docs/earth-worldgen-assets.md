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
