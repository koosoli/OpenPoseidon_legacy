# Earth terrain streaming prototype

This opt-in Freefly experiment loads real elevation around a geographic origin
and moves the terrain working set as the camera travels. Normal OFP gameplay is
unchanged. It includes a low-detail satellite globe overview; it is not
a VBS4-equivalent world simulation.

## Backup and development

The complete pre-experiment source is preserved in the private safety mirror at
`codex/backup-before-world-streaming-20261007`, commit
`e20c3974de6f91b55b402e3f5bc6defe76c84690`.
Work proceeds on `codex/whole-world-horizon-20261008`, following the initial
`codex/whole-world-streaming-20261007` prototype. Private development
history must not be merged into the cleaned OpenPoseidon release repository.

## Try it

After building and deploying the matching engine pair, close other game sessions
and run from the source checkout:

```powershell
./scripts/Launch-WholeWorld.ps1
# Another starting region (Grand Canyon):
./scripts/Launch-WholeWorld.ps1 -Latitude 36.1 -Longitude -112.1 -Altitude 3000
# Curved horizon from the stratosphere:
./scripts/Launch-WholeWorld.ps1 -Altitude 120000
# Complete globe from orbit:
./scripts/Launch-WholeWorld.ps1 -Globe
# Optional locally cached Luxembourg building footprints:
./scripts/Launch-WholeWorld.ps1 -Latitude 49.61 -Longitude 6.13 -Altitude 1000 -CachedBuildings
```

The default view starts above the Swiss Alps at latitude 46.6, longitude 8.1,
altitude 4500 metres. Use the existing Freefly controls. Initial terrain can take
several seconds to appear on an empty cache; the log records each completed patch.
The launcher obtains the shared game-folder lock and restores process environment
variables when the game exits. It creates its own empty test world and mission;
no retail map is copied into this fixture. Near and far Earth surfaces share the
cached NASA overview imagery. Its coarse resolution is particularly visible
near the ground; detailed satellite tiles are not implemented. Dev Tools
**Earth** changes geographic origin in the same session. W/S follow the view
direction, the mouse wheel changes travel speed, and Shift accelerates. The
virtual camera target translates with movement, so approaching Earth no longer
overtakes the target and reverses the view.

## Implementation and bounds

- Fixed geographic origin and double-precision coordinate conversion. Tile
  longitude wraps across the dateline; northern image rows become positive world Z.
- Mapzen Terrarium near elevation uses a fixed 257 x 257 grid: 25m spacing and
  zoom12 below 8km altitude, 100m and zoom10 above 12km, with hysteresis between.
  Coverage is respectively 6.4 or25.6km. Source pixel spacing varies with latitude;
  this remains a rendering preview rather than infantry-ready ground.
- A second, nonuniform 257 x 257 mesh fills the area outside the near rectangle.
  Its half-width grows in power-of-two bands from 64 km to 2048 km, based on
  geometric horizon distance and altitude. At 4500 m it is 512 km; at 120 km it
  is 2048 km. Its triangle budget stays fixed (about 125,000 plus seam skirts).
  This uses the existing mesh/depth/lighting pipeline, not another fullscreen pass.
- Far source zoom is chosen to keep approximately four source tiles across the
  window. Near the shared boundary it blends back to the active near heights; edge
  skirts close fine/coarse T junctions. Near and far geometry publish together.
  Both use a stable spherical sagitta about the fixed origin, so the far horizon
  curves. This elevation window uses a curved tangent presentation; the separate
  global overview sphere covers the rest of Earth.
- The window recentres in 800m or3.2km steps. Its resident margin loads terrain ahead of
  ordinary camera motion without creating an unbounded request queue. Very fast
  jumps can outrun the current window; it stays visible until a replacement is ready.
- One background worker performs HTTPS, disk access, PNG validation and bilinear
  resampling. One latest request and one completed patch cross a mutex; stale work
  is discarded. The render thread consumes a completed heightmap and rebuilds its
  small CDLOD tree and fixed-budget horizon mesh only on a window/altitude-band
  change. GPU upload/sky visibility work still
  occurs on publication and is not claimed to be hitch-free.
- Decoded tile LRU: 64 tiles across source zooms (16 MiB height data). Disk: at most 256 validated
  tiles after a successful patch, each capped at 2 MiB (512 MiB hard payload bound).
  Zoom-specific cache directories preserve existing zoom-10 tiles.
  Cached corruption is discarded and refetched. Timeout and 10-second retry
  backoff retain the last complete patch. Shutdown cancels the active transfer.
- The native terrain renderer is reused; the finite map's procedural grass and
  water are not submitted by this path. No new default-frame shader pass is added.

## Relationship to CWR-Worldgen and data

Reviewed [CWR-Worldgen](https://github.com/svamp3717/CWR-Worldgen), reference
commit `b46124fc9c810773b5402633168af4829240f143`.
Its current pipeline generates finite playable islands from geographic source
bundles; it does not provide a continuous globe runtime. No code, generated game
assets or texture pack from that repository is vendored by this prototype.
Its `source.json` and `elevation/heightmap-meters.tif` are potential inputs for a
later geographic asset/cache adapter, including authored roads and settlements.
That adapter is not implemented here.

Runtime heights come from the public
[Terrain Tiles dataset](https://registry.opendata.aws/terrain-tiles/):
`https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png`.
Downloaded tiles stay in `%LOCALAPPDATA%/OpenPoseidon/world-stream`, outside Git
and release assets. The launcher writes a data-source receipt there. Retain the
[dataset acknowledgements and individual source terms](https://github.com/tilezen/joerd/blob/master/docs/attribution.md)
before redistributing geographic data; the cache is not an asset redistribution
approval. Terrain source coverage and quality vary.

## Explicit limits and next stages

The active rendering area moves; the underlying `Landscape`, AI, collision,
navigation, weather ground sampling and mission editor remain finite. Stay in
Freefly. There are no worldwide vegetation, roads, physical oceans or terrain-aware
spawn points yet. An optional bounded, locally cached OSM building presentation
layer is described in [the building prototype](EARTH-CACHED-BUILDINGS-20261008.md);
it has no interiors or collision and is not a worldwide settlement database.
The cached low-detail satellite texture covers the globe and near/far meshes.
Curved elevation coverage is bounded to 2048 km half-width and altitude up to
500 km; beyond that the overview sphere remains visible without tile downloads.
The geographic addressing remains an approximate tangent map, constrained to
500 km of engine coordinates per axis, and the provider has no coverage above
85.051 degrees latitude. Different global origins can be selected at launch or live in the Earth tab, but
one cannot yet circumnavigate the globe in a single session. Elevation cells beyond
the provider's latitude coverage are omitted; the overview sphere covers the poles.

Next stages should be separately measured: detailed imagery/landcover and
ocean, shared geographic simulation tiles, floating-origin rebasing and full
globe elevation addressing, then CWR-Worldgen geographic content tiles. Complete
these before presenting this as an entire playable Earth.

## Validation

`scripts/Test-EarthCoordinates.ps1` checks origin, north/east orientation,
Terrarium negative/fractional heights, dateline wrapping, recenter invariance and
polar/nonfinite rejection. `scripts/Test-EarthStreaming.ps1 -ExpectedCommit <sha>`
under `scripts/with-game-lock.sh` checks the installed renderer at five camera
positions, including outside the fixture's finite bounds and return to cached
terrain, captures frames and records worker/cache receipts. `-Horizon` adds
30 km/120 km ascent and return, with horizon/upload/frame-performance receipts.
Coordinate tests include stable curvature, near/far alignment, ordered sample
spacing, altitude coverage and low-zoom dateline addressing.

Installed source `6c0075ee` passed the online run recorded in
`build/earth-streaming/20261007-225432/result.json`: five camera positions,
including x=32000 outside the original world, produced new terrain patches and
returned to the original cached heights. The far patch downloaded two new tiles;
six tiles were cached overall. Subsequent uploads reused height/shadow resources.

The offline run in `build/earth-streaming/20261007-225607/result.json` also passed:
all five cached positions loaded with zero downloads. An uncached move to
x=100000 failed through a deliberately unavailable proxy, retained the last
complete terrain patch and left the frame-performance harness responsive. These
checks validate the bounded elevation prototype, not globe gameplay or populated
geographic content. These historical receipts describe the original near-window
implementation; the extension's acceptance is recorded below.

## Global overview extension (8 October)

`./scripts/Launch-WholeWorld.ps1 -Globe` starts 18,000 km above the chosen
geographic origin, looking down at the complete Earth. `-Altitude` now accepts
500 m to 30,000 km. This is an opt-in rendering preview, not an expanded gameplay
or collision world. The fixed sphere covers both poles and the far side, while
near/far elevation streaming retains its tangent-map addressing limits.

The launcher caches NASA Earth Observatory's June 2004 Blue Marble Next
Generation base map (5400 x 2700 JPEG) outside Git. Source and image-use guidance
are recorded in DATA-SOURCES.txt beside the cache:
https://science.nasa.gov/earth/earth-observatory/blue-marble-next-generation/base-map/
https://www.nasa.gov/nasa-brand-center/images-and-media/
The image is uploaded once at 4096 x 2048 with mipmaps (about 43 MiB GPU memory).
This is a global overview texture, not high-resolution streamed ground imagery.
Without it the sphere uses a plain fallback. No game content is downloaded.

The globe costs 65,536 triangles at any altitude. Above 500 km only this fixed
mesh is submitted: no new height-tile requests or fullscreen rendering passes.
Below that altitude the existing near terrain and adaptive coarse horizon resume.
The Earth material bypasses finite-map cave/shadow/weather receivers; those
receivers previously misclassified the curved far surface as underground.
Normal OFP maps retain their existing lighting. Orbit uses a larger camera near
plane to preserve reversed-Z depth precision at Earth-scale distances.

`Test-EarthStreaming.ps1 -ExpectedCommit <sha> -Horizon -Globe` additionally
checks the actual orbital camera position, image upload, screenshot and return
to the near terrain. Coordinate tests check the sphere's origin, antipode,
unit normals, longitude seam and poles.

## Integrated acceptance, 8 October

Installed `292e86fd` passed live Alps -> Grand Canyon -> dateline -> Alps
relocation in `build/earth-locations/20261008-031645/result.json`. Camera positions,
origin revisions, bounded cache, invalid requests and cancellation of superseded
requests passed. Screenshots were inspected: geographic relief is present, while
near imagery remains coarse. This is not detailed terrain-texture acceptance.

The adaptive near/far mesh and actual W/S input passed in
`build/earth-streaming/20261008-023809/result.json`, source `dbeb9d36` plus a
test-script-only correction. At 12,000km altitude W approached to 10,847,989m;
S retreated to 11,988,383m without reversing the view. Geometry stays bounded:
at most 256,000 near/far/skirts triangles plus 65,536 overview triangles.

The final paired `64b1df89` acceptance additionally passed optional cached
Luxembourg buildings and neighboring-cell retention, then repeated geographic
relocation. The current 3x3 window keeps 1995 buildings/46281 triangles visible
when entering an adjacent uncached cell, with a shared 200k-triangle ceiling.
Detailed receipts and remaining limits are in
[the integrated verification note](../verification/integrated-earth-material-reshade-20261008.md).

Installed source `27b7bf23` adds one cached ACT CC0 Luxembourg orthophoto via
`Launch-WholeWorld.ps1 -LocalSurface -CachedBuildings`. The Earth tab's
Luxembourg button visits the prepared cell; Globe returns to orbit. The atlas
shares identical near geometry, feathers into NASA and costs one additional
draw, with no extra triangles. Runtime before/after, recenter, geographic
fallback and actual W/S approach checks pass on this installed source; receipts
are in the integrated note. See [surface preparation and limits](../earth-local-surface-prototype.md).
This one cell does not establish worldwide detailed surfaces or VBS4 parity.
