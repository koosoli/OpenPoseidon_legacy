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
stabilizes appearance for the same geographic sample across ENU origins.
Forest placement uses a separate fixed 32-metre lattice in a source-derived
geographic frame. The lexicographically smallest latitude/normalized-longitude
source vertex selects its .02-degree cell centre as the frame anchor. Both the
polygon containment test and sample grid use that frame. Reversing a closed
ring or loading the same geometry from a neighboring cache cell does not move
the lattice. Candidates project into the current resident terrain frame before
coverage and ground-height checks. A conservative disk around the inverse
projection of the resident rectangle bounds the search; the existing sample
cap still applies. Geometry updates or shared-budget priority changes can alter
the visible subset. Separate polygons use their own lattice; this is synthetic
forest presentation, not persistent individual-tree or ecological data.

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
curved heights. Forest cases preserve geographic centres (within 5 cm of float
mesh quantization), height and palette across near/orbital origin changes, at
the equator, Luxembourg and high latitude across the dateline. Ring reversal,
neighbor source-cell duplication and remote-patch zero sampling are checked.
Its offline `BuildNeighborhood` case reads a generated cell
from `content-stream/features`, then verifies that a prepared empty cell wins
over that generated copy. The test makes no HTTP requests.

Production C++ syntax checks use the configured parent build's compile flags
for `EarthFeatureStream.cpp` and `TerrainWgpu.cpp`; the parent task owns the
combined build, deployment and installed Luxembourg visual check.

## Remaining worldwide content limits

A configured optional pre-tiled provider or prepared local cells are still
required for populated regions. The implementation does not contain a worldwide
building/road/forest dataset and does not query public OSM services automatically.
Absent cells produce absent presentation features. An operator must provide a
licensed dataset, geographic partitioning and an endpoint/cache layout matching
the bounded cell contract; source ways crossing cells need compatible geometry
in the cells where they should be encountered. Transport caching is bounded,
not a download or retention plan for an entire planet.

The loaded neighborhood and geometry budgets deliberately bound detail. Forests
currently support simple closed ways with at most 256 points; multipolygon
relations, holes and species are not implemented. Trees, roads and buildings
are presentation meshes without global object simulation, building contact,
navigation or terrain grading. Resident terrain coverage still defines valid
infantry ground. Free-flight geographic continuity does not relocate native
mission entities into a globally populated simulation.
