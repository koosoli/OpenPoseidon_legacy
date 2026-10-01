# Actual triangle storage and face spill gate

Status: CPU prototype only, following `bd3cfb3f` and benchmark `8f7424ec`.
World, Landscape, the publication ABI and shaders are unchanged. Actual installed
crater filling and performance remain pending; this is not default activation.

## Actual geometry rather than centre-bed depth

`RainWaterGeometry.hpp` sorts each triangle's three authored corner heights once.
It analytically integrates `max(head - terrainHeight, 0)` over the exact
anti-diagonal triangles 00/10/01 and 10/11/01. The low branch is cubic; the middle
branch is evaluated as positive increments from the middle height to avoid
subtracting large near-equal tail terms. Equal low, equal high and fully flat
triangles have finite branches. Heights remain immutable behind accessors so a
caller cannot change corners while leaving stale sorted coefficients.

The storage curve and its wet-area derivative invert retained volume to a
horizontal head. Flat and completely submerged cells use direct formulas.
Partially wet cells use safeguarded Newton iteration with the previous head as
a hint and a bounded rare bisection fallback. This evaluates analytic scalar
curves; it does not clip dozens of polygons per owner per frame.

`RainWaterFine::RefineTileGeometry` accepts actual four-corner data for every
fine owner. Updates preserve existing volumes and recompute heads. The original
float-centre `RefineTile` overload remains for the preceding flat-cell prototype
tests; a real producer must use the geometry API. `Owner.Depth` remains average
volume/area, while `Owner.Head` is now explicitly published separately. They
must not be combined as `minimumBed + averageDepth`. Read-only `LocalDepth`
evaluates `max(head - actual triangle height at the point, 0)`.

Each fine shared face uses its actual two endpoint heights. Its integrated wet
cross-section prevents a lower neighbouring centre from draining a bowl across
a dry high rim. Mixed coarse/fine boundaries use the fine profile; a coarse
donor is additionally capped to its existing column depth. Two inconsistent
fine profiles use their higher endpoint heights until both sides are updated,
rather than opening an unproved channel. All-coarse proposals retain the legacy
donor-depth calculation. The single donor limiter and reciprocal volume updates
remain the same. Equal-head and zero-flux faces now skip unnecessary work.
Heads are cached for flux passes and refreshed after all volume changes.

## CPU evidence

`scripts/Test-RainWaterFine.ps1` passes both the original adaptive ownership suite
and the new actual geometry suite under Clang C++20 with warnings-as-errors.
The existing `Test-RainWaterField.ps1` also passes. New checks include:

- Analytic flat, inclined plane, anti-diagonal ridge and hollow volume/area.
- Unequal-height cubic branches compared with an independent numerical triangle
  integral; equal-height permutations and translated negative terrain heights.
- Volume-to-head roundtrips, finite admission and source-corner immutability.
- Wet/dry local points on one inclined owner, source edits preserving volume,
  and a genuine raised face blocking flow into a lower coarse centre until that
  face is actually lowered.
- A 6.25-metre source-corner crater generated with `TerrainCraterDepth`: all
  old 25-metre vertices miss it, but exact triangle storage yields over 3 cm
  local water depth and over twice nearby flat depth after 120 simulated seconds.
  Every owner's integrated volume at its published head matches stored volume.
- Exact budget accounting, curved-source constant-forcing frame partitions,
  pause/read stability and total-volume-conservative regrid.

An early test failure was a test ordering error: its flat-cell midpoint followed
a positive head increment, so the expected monotone sample sequence decreased.
Sorting the query heads corrected the fixture; physical tolerances were unchanged.

## Measured cost after geometry

`Measure-RainWaterFine.ps1` now evaluates actual crater corners and adds 64 fully
sloped tiles, rather than timing only flat cells. Same i7-11700F/Clang22.1/O2
sequential protocol as `RAINWATER-FINE-COST-AND-INTEGRATION-20261001.md`; owner
game/background load was uncontrolled. Medians per quarter-second step:

| Scenario | Actual legacy | Geometry core, coarse only | Geometry core, 64 tiles |
| --- | ---: | ---: | ---: |
| Closed shallow | 3.66 ms | 4.62 ms | 6.65 ms |
| Closed deep seed | 3.71 ms | 4.53 ms | 6.70 ms |
| Actual source-corner crater | 3.75 ms | 4.53 ms | 7.28 ms |
| Fully sloped fine terrain | 6.82 ms | 10.35 ms | 18.78 ms |

These timings include the equal-head/zero-flux fast paths added with geometry;
they are not an isolated estimate of curve overhead. Full slope costs 2.75 times
legacy and remains a material approximately 19-ms single-step hitch. This is not
production performance acceptance. Requiring source-corner proof must not be
traded away for the faster centre-only model.

Maximum vector capacities are now 31.75 MiB coarse-only and 40.28 MiB with
64 full tiles, excluding map nodes, snapshots and allocator overhead. Explicit
head expands Owner to 56 bytes: actual maximum snapshot is 17.34 MiB. Median
64-tile snapshot times are 5.25 ms shallow, 5.36 ms deep, 6.85 ms crater and
7.30 ms slope. Legacy snapshots range 1.11–1.20 ms and remain 4.02 MiB. Immutable
topology metadata plus packed changing lanes is still required before upload;
these raw Owner records should not become the production quarter-second ABI.

## Remaining review gates

The exact storage fixes the local horizontal fill level, not every numerical
hydrology limitation. Separate disconnected basins inside one fine cell still
share a head; partial coastal cells retain the prototype's minimum-bed sea
outlet policy. The face transfer cap is the prior planar-area approximation,
followed by the exact donor-volume limiter, rather than a solved nonlinear
two-cell equilibrium. Test stability and decide these bounds before broader
world acceptance. Source sampling and soil loss currently have no per-fragment
roof/wet-area policy. No full-world shelter claim is made.

Regrid still conserves *total* volume by uniform source-owner overlap density;
it coarsens tiles and does not preserve curved subcell wet placement or exact
free-surface shape. The test and API comment make this explicit. A runtime
geometry regrid needs either actual clipped-column overlap integration at that
rare event or an explicitly documented redistribution/reset policy. It must not
be advertised as preserving every shoreline. Mission and camera lifetime
policies remain separate.

Next production review remains the journal/bare-height hooks and coherent
publication in the preceding integration assessment. Actual fine planes must
clip against the same versioned piecewise heightmap and carry `head - bed`
optical depth, with coarse rendering excluded from fine-owned rectangles.
Tile-edge continuity, shallow water, real crater impact, source restoration,
raised support/sea negatives and installed frame cost remain runtime gates.
