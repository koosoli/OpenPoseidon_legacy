# Cached Earth buildings: bounded presentation slice

Opt-in only: `POSEIDON_EARTH_BUILDINGS=1` with the existing Earth mode/cache.
The game reads one local 0.02-degree building cell around the camera on a worker.
No Overpass request is made by the game. Preparing a cell while the game is
running requires crossing a cell boundary or relocating to refresh it.

Explicit preparation, once:

```powershell
./scripts/Prepare-EarthCell.ps1 -Latitude 49.61 -Longitude 6.13
```

Then launch Earth with the buildings flag and use the Earth tab to go to
49.61, 6.13 at 1000 metres. Only prepared cells have buildings. Missing cells
remain empty. Meshes follow the actual resident curved elevation patch;
origin revisions invalidate old work and renderer state.

The prototype renders neutral, flat-roof footprint extrusions. Heights use
OSM `height`, else `building:levels` times 3m, else an explicitly estimated
6m; a log reports estimates. Foundations extend below the lowest sampled
footprint boundary and roofs start above its highest sample. Slopes over
30m, invalid/open/self-intersecting polygons, relations, elevated outlines
and unsupported height ranges are skipped. No interiors, collision,
vegetation, roads, custom assets or accurate roof shapes are provided.

Limits: 4MiB/cell; at most 2000 ways, 128 polygon vertices and 200000 triangles;
the manual preparer permits at most 64 cell files. The manual query samples
at most 2000 ways, so a dense cell can be incomplete. One GPU mesh is live.
Work and publication are generation/revision checked. Default Earth and OFP
rendering are unchanged. Full build and installed runtime acceptance are
owned by the parent integration task.

Data attribution is visible in the Earth tab and beside downloaded cache
files: **(c) OpenStreetMap contributors, ODbL 1.0**,
https://www.openstreetmap.org/copyright . Data are cached outside Git and are
not bundled with source or binary releases. Source code and synthetic tests
are independent of OSM data licensing.

The [Overpass public-instance policy](https://dev.overpass-api.de/overpass-doc/en/preface/commons.html)
allows occasional manual queries, but explicitly rejects public instances as
an application backend or a means to scrape the world. This script performs
one capped query, reuses an existing cache, and does not retry. Wider streaming
needs licensed bulk/local data or a controlled backend before expansion.

Source validation: `scripts/Test-EarthBuildings.ps1` passed convex/concave roofs,
outward winding, known sloped foundations, degenerate/open/self-intersecting
outlines, height and size limits, triangle caps and relocation publication.
One explicit Luxembourg cell query produced 1,586,100 bytes. Parsing it against
synthetic flat 300m ground produced 1995 buildings, 5 skipped and 46281 triangles;
this checks data ingestion, not actual terrain or runtime visual acceptance.
`TerrainWgpu.cpp` and `DebugOverlay.cpp` passed clang-cl MSVC syntax validation. Parent integration
still owns full linking, deployment and the installed-game acceptance.

