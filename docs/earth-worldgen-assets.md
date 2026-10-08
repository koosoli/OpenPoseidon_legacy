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
before starting an Earth-enabled game. Restart to load a changed output.

The adapter reads these existing Worldgen manifest fields:

- `world.name`, `world.cells`, `world.cell_size`;
- `world.bbox`: `[south, west, north, east]`;
- top-level `height_scale`.

The corresponding file is `source/<world.name>.wrp` below the manifest directory.
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

One manifest is loaded once on a value-only worker. Limits are 4 MiB manifest,
32 MiB RVW4, 2,048 grid cells per axis and 65,536 source object records. Each
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

This first connection loads one regional Worldgen output. It is not a worldwide
generated dataset or an automatic regional output provider. Beyond that region,
more licensed outputs and a bounded geographic output index/provider are still
needed. The existing optional feature JSON backend supplies input features,
not generated P3Ds or Worldgen placement records. Generated texture/material
styles, terrain grading, road network/navigation, enterable building simulation,
destruction and object collision are not transferred by retained rendering.
Nested proxy completeness and forest conformity require actual model-family
acceptance. Source road/forest pitch/shear is retained; differences between the
generated terrain and resident DEM can still cause local fit errors. No global
playability or parity claim follows from this adapter.
