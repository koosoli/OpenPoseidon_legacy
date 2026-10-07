# Earth terrain streaming prototype

This opt-in Freefly experiment loads real elevation around a geographic origin
and moves the terrain working set as the camera travels. Normal OFP gameplay is
unchanged. It is an initial runtime streaming slice, not a completed globe or
a VBS4-equivalent world simulation.

## Backup and development

The complete pre-experiment source is preserved in the private safety mirror at
`codex/backup-before-world-streaming-20261007`, commit
`e20c3974de6f91b55b402e3f5bc6defe76c84690`.
Work proceeds on `codex/whole-world-streaming-20261007`. Private development
history must not be merged into the cleaned OpenPoseidon release repository.

## Try it

After building and deploying the matching engine pair, close other game sessions
and run from the source checkout:

```powershell
./scripts/Launch-WholeWorld.ps1
# Another starting region (Grand Canyon):
./scripts/Launch-WholeWorld.ps1 -Latitude 36.1 -Longitude -112.1 -Altitude 3000
```

The default view starts above the Swiss Alps at latitude 46.6, longitude 8.1,
altitude 4500 metres. Use the existing Freefly controls. Initial terrain can take
several seconds to appear on an empty cache; the log records each completed patch.
The launcher obtains the shared game-folder lock and restores process environment
variables when the game exits. It creates its own empty test world and mission;
no retail map is copied into this fixture. The ordinary ground material still
comes from the user's installed CWA data.

## Implementation and bounds

- Fixed geographic origin and double-precision coordinate conversion. Tile
  longitude wraps across the dateline; northern image rows become positive world Z.
- Mapzen Terrarium elevation at zoom 10; 257 x 257 samples spaced 100 metres apart,
  covering 25.6 x 25.6 km. Resolution is a prototype tradeoff, not detailed ground
  suitable for infantry. Source pixel spacing varies with latitude.
- The window recentres in 3.2 km steps. Its 12.8 km margin loads terrain ahead of
  ordinary camera motion without creating an unbounded request queue. Very fast
  jumps can outrun the current window; it stays visible until a replacement is ready.
- One background worker performs HTTPS, disk access, PNG validation and bilinear
  resampling. One latest request and one completed patch cross a mutex; stale work
  is discarded. The render thread consumes a completed heightmap and rebuilds its
  small CDLOD tree only on a window change. GPU upload/sky visibility work still
  occurs on publication and is not claimed to be hitch-free.
- Decoded tile LRU: 64 tiles (16 MiB height data). Disk: at most 256 validated
  tiles after a successful patch, each capped at 2 MiB (512 MiB hard payload bound).
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
Freefly. There are no worldwide buildings, vegetation, roads, satellite imagery,
water or terrain-aware spawn points yet. Distant working-set boundaries can be
visible at high altitude. The approximation is locally planar, constrained to
500 km of engine coordinates per axis, and the provider has no coverage above
85.051 degrees latitude. Different global origins can be selected at launch, but
one cannot yet circumnavigate the globe in a single session.

Next stages should be separately measured: a persistent low-detail horizon with
near/far rings, shared geographic simulation tiles, floating-origin rebasing and
curved globe/polar addressing, then CWR-Worldgen geographic content tiles. Complete
these before presenting this as an entire playable Earth.

## Validation

`scripts/Test-EarthCoordinates.ps1` checks origin, north/east orientation,
Terrarium negative/fractional heights, dateline wrapping, recenter invariance and
polar/nonfinite rejection. `scripts/Test-EarthStreaming.ps1 -ExpectedCommit <sha>`
under `scripts/with-game-lock.sh` checks the installed renderer at five camera
positions, including outside the fixture's finite bounds and return to cached
terrain, captures frames and records worker/cache receipts.

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
geographic content. The installed binary remains `6c0075ee`; this acceptance
record is documentation only.
