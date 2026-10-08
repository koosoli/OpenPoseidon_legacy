# Bounded Earth procedural trees

The optional Earth feature layer replaces its floating octahedra with a trunk
and a rounded, two-ring broadleaf crown. These are generated presentation
meshes, not OFP vegetation assets or a species database. No game data is copied.

Each tree has 28 triangles: eight trunk sides and twenty crown triangles. The
trunk starts at the resident terrain's actual triangle-interpolated height at
the tree centre. Its small base is horizontal; individual base corners do not
conform to steep slopes. Neither terrain heights nor collision data change.
The crown shares smooth normals at coincident vertices. Trunk sides retain
flat normals and use a brown palette band; the crown selects one of three
green bands from a generated 16-by-1 RGBA texture. Every triangle has constant
UVs within a band, avoiding interpolation across bark and leaf colours.

Width, rotation, lean and crown asymmetry use a deterministic geographic hash.
Forest samples also vary their estimated nine-metre height by 0.82–1.18;
individual tagged tree heights retain the existing parsed height. This seed
stabilizes appearance for the same geographic sample across ENU origins. The
existing forest placement grid still follows the local origin and may change
sample locations when rebased. No new ecology or placement persistence is claimed.

The existing single tree mesh and draw remain. Limits stay at 4,000 trees,
16,000 attempted forest samples and 120,000 triangles shared by all feature
kinds. A full tree layer uses 112,000 triangles; earlier road/water features may
leave less room. The tree builder checks the complete 28-triangle cost before
appending. No additional render pass, external texture, model, shader or data
request is added. Native OFP vegetation and terrain paths remain unchanged.

These opaque crowns do not have leaf cutouts, wind animation, tree collisions,
shadows or authored vegetation LODs. Runtime appearance must be judged in the
installed Earth feature scene; CPU geometry tests do not establish visual quality.

`scripts/Test-EarthFeatures.ps1` checks geometry winding against every vertex
normal, unit smooth normals, shared-position continuity, palette coordinates,
determinism, geographic seed stability, source-height grounding and fixed caps.
It covers two-to-forty-metre trees at local coordinates up to 500 km and negative
curved heights. Its offline `BuildNeighborhood` case reads a generated cell
from `content-stream/features`, then verifies that a prepared empty cell wins
over that generated copy. The test makes no HTTP requests.

Production C++ syntax checks use the configured parent build's compile flags
for `EarthFeatureStream.cpp` and `TerrainWgpu.cpp`; the parent task owns the
combined build, deployment and installed Luxembourg visual check.
