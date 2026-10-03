# Bounded hillside entrance rendering

The existing hole packet remains a sequence of at most 64 float4 records. Normal
records are `{nx, nz, d, last}`; `last=1` ends a convex polygon. An optional
`{0, 0, worldCeilingY, 2}` record immediately BEFORE a polygon limits its terrain
cut to `worldY <= worldCeilingY`. That record consumes one of the 64 slots.
Legacy polygons without metadata retain their unbounded behavior. Hidden interior
footprints are not serialized into this drawing packet.

Terrain color, its depth/normal prepass and planar reflection use actual
interpolated world Y. All three grass placement kernels use sampled ground Y.
This preserves the mountain and its grass above the entrance ceiling. GL33 uses
the same predicate, subtracting camera Y from ceiling metadata and applying the
existing camera-XZ conversion only to ordinary edge distances.

Metadata is explicitly skipped before edge validity and terminal checks. Bound
state resets after each polygon. The existing finite-packet upload refusal and
collapsed-edge exclusion remain. A metadata record alone never removes terrain.
Buffer size remains 1040 bytes; no extra resource binding or ABI stride is added.

Bounded mouths are conservatively excluded from the existing vertical skylight
distance calculation, in both conform shading and CPU camera-effects exposure.
They are not vertical skylights. This does not implement lateral sun/sky portal
transport; the cave can remain too dark until that separate feature is verified.
Existing legacy stairwell daylight remains unchanged.

The root agent owns authored ceiling serialization and height-aware CPU collision,
LOS and camera support. This renderer change does not generate cave geometry or
make the heightfield volumetric. A model supplies the interior walls and floor.

Focused validation filters (root owns actual GPU/game verification):

```
cargo test terrain::hole_ceiling_tests:: -- --test-threads=1 --nocapture
cargo test terrain::terrain_hole_tests:: -- --test-threads=1 --nocapture
```

The focused tests validate extracted actual production predicates and four complete
production shaders through Naga. Actual GPU cases cover ceiling equality/above/below,
legacy reset, overlapping bounds, the 64-record boundary, empty/metadata-only and
collapsed packets, and bounded-mouth versus legacy-skylight daylight. No child
agent GPU or installed-game test was run. Root still needs the combined paired
build and an authored hillside mouth walkthrough, with terrain/grass above the
ceiling and hidden interior roof retained.
