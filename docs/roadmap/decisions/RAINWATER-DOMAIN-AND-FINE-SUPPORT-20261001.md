# Rainwater domain identity and finer terrain support

Source assessment baseline: `ed22244d9cdb3c9d82241acded045c19cdcf136b`.
The domain identity fix below is a source candidate. Installed-game acceptance
remains pending; the agent did not build, deploy or launch the game.

## Runtime terrain-grid changes

`World::SimulateCloudlets` previously configured water only when its width was
zero. `setTerrainGrid` changes terrain range/grid and increments
`Landscape::HeightRevision`, but the subsequent water update reused the old
water dimensions and spacing. Writing new sample indices through `SetBed`
cannot change that domain, and out-of-range writes simply fail. Old water could
therefore be queried at incorrect world positions after a terrain regrid.

The producer now checks the actual water width, height, spacing and origin
through `RainWaterField::MatchesDomain` before updating heights. Ordinary
same-domain terrain edits retain depth, volume, generation and pending time.
A changed water domain explicitly retires the old field, then configures a dry
field from the new actual source vertices. The diagnostic
`RAIN_WATER_DOMAIN_RESET` records old/new dimensions and spacing plus removed
volume in cubic metres. A rejected new bed leaves an empty field; stale water
does not continue simulating. Reset/configuration advances the existing
generation, so same-numbered revisions cannot reuse old presentation data.

This is deliberately an explicit reset policy for the developer terrain-grid
operation. It is not conservative remapping: retained water and its budget are
discarded, as logged. Camera movement does not trigger it. A future remapper
must define the existing node-centred equal-area storage footprints and their
boundary overlap before distributing volumes between differently sized cells.

CPU validation: `scripts/Test-RainWaterField.ps1` passed with Clang C++20 and
warnings-as-errors. New cases exercise the production domain predicate for
both dimensions, spacing and origins; same-domain excavation preserves water;
reconfiguration resets backlog/budget/generation; nonfinite new bed rejection
cannot expose the retired domain; shrinking invalidates old outer queries.
Existing hollow, downhill flow, spill, sea outlet, conservation, drying, pause
and constant-weather frame partition cases also passed. `git diff --check`
passed. The full `WorldImpl.cpp` engine build is pending with the parent agent.

Installed check: query `dev_rain_water` state/sample through the harness, build
some ordinary rainwater, change `setTerrainGrid`, and compare the new state to
the actual source range/grid. Require the reset diagnostic, changed generation,
new spacing/dimensions, zero initial retained volume, and finite new sample
coordinates. Repeat with a terrain edit that does not change the effective
water domain: generation must remain unchanged and volume must survive before
the next ordinary flow/loss step. Moving/returning the camera must not reset.

## What the current field can resolve

`AlignedSourceGrid` caps the sampled world grid at 513 vertices per side using
an exact power-of-two stride. Native terrain range 2048 and spacing 6.25 metres
therefore produces 513 water vertices at 25 metres. Original CWA range 256 at
50 metres produces 256 vertices at 50 metres, unless its terrain was resampled.
These cases are pinned by the existing production helper tests. The field uses
the terrain's triangle diagonal on its *sampled* grid; it does not retain all
the fine triangles that lie between those vertices.

Real missile/impact-shell craters run through `RocketTerrainImpact`,
`PaintTerrain`, a successful Box3D terrain patch, then actual `HeightChange`
calls and `FlushCache`. Craters are enabled by default in `RocketCraterSettings`;
the path remains single-player and rejects direct-object impacts and sea hits.
`HeightRevision` changes for each edited vertex. The water refresh nevertheless
resamples only its coarse source stride. A real crater whose changed vertices
fall between those samples can remain completely absent from the water bed.
An old decorative crater object is not evidence of heightfield excavation.

The actual source geometry is queryable: `ClippedData`/`GetHeight` expose edited
vertices; `Landscape::SurfaceY` evaluates the actual triangles. However,
`SurfaceY` also adds mud and sand displacement now, whereas `ClippedData` does
not. Fine water support must explicitly choose immutable terrain/crater bed
versus these simulation deformations and carry their independent revision keys.
Blindly replacing `ClippedData` with `SurfaceY` would not provide those keys.

Crater size is also bounded by the actual source geometry:
`TerrainBrushRadius` enforces at least 0.75 times source spacing. The minimum
supported radius is thus 4.6875 metres on 6.25-metre terrain and 37.5 metres on
unsubdivided 50-metre CWA terrain. A one-metre water grid cannot manufacture a
one-metre geometric crater on the latter. Smaller physical craters need a
coherent finer terrain/physics representation as well as water refinement.

The presentation shader additionally rejects support disagreement over 15 cm
between its coarse water bed and actual GPU terrain. This is a useful safety
gate; loosening it would draw water through unmatched geometry rather than
repair the simulation. Read-only `dev_rain_water sample` reports both field
height/depth and actual `terrainY`, allowing this mismatch to be measured.

## Cost and next credible improvement

The field stores six float arrays (24 bytes per vertex). Each RGBA snapshot or
texture stores another 16 bytes per vertex; the C++ producer currently creates
the float snapshot and then a second copied `WgrVec4` vector before queuing it.
Rust also retains the uploaded cell data. These are per-resource costs, not a
single combined memory total. A changed revision can copy/upload the entire
field every fixed quarter-second step.

| Grid on an approximately 12.8 km world | Vertices | Field arrays | Each RGBA copy/texture | Pair-edge visits/simulated second |
| --- | ---: | ---: | ---: | ---: |
| Current 513 squared, 25 m | 263,169 | 6.02 MiB | 4.02 MiB | 4.20 million |
| 1,025 squared, 12.5 m | 1,050,625 | 24.05 MiB | 16.03 MiB | 16.79 million |
| 2,049 squared, 6.25 m | 4,198,401 | 96.09 MiB | 64.06 MiB | 67.14 million |
| 6,401 squared, 2 m | 40,972,801 | 937.79 MiB | 625.20 MiB | 655.46 million |
| 12,801 squared, 1 m | 163,865,601 | 3,750.59 MiB | 2,500.39 MiB | 2.62 billion |

The edge counts are analytical counts from the two current edge sweeps at four
steps per simulated second, excluding all cell sweeps and copies; they are not
measured timings. Accelerated simulation multiplies the work. A global 1,025
squared option already exceeds the current 1,048,576-cell C++/Rust cap and Rust's
1,024-per-dimension limit, costs about four times as much, and still misses
6.25-metre source detail. It is not the smallest credible small-crater solution.

Recommend a fixed-budget, simulation-owned fine tile layer at **actual source
spacing**, initially admitted by real height-edit footprints. A possible first
budget is 64 tiles of 32 by 32 fine cells: 65,536 cells, approximately 1.5 MiB of
the same six scalar arrays and 1 MiB per RGBA resource, before halos, metadata
and boundary bookkeeping. At 6.25 metres those are 200-metre patches. Overlap
must coalesce; excess allocation must be explicitly refused/deferred. Wet tiles
must not disappear or reseed as the camera moves. Extending admission to natural
hollows needs a bounded source-geometry depression/catchment analysis, not a
camera-driven noise mask or only height-edit events.

This is a meaningful next implementation slice, but requires all of these
pieces together:

- A bounded height-change footprint journal keyed by terrain generation and
  revision. The current edit baseline journal saves original heights for undo;
  it is not a consumable dirty-range stream. All actual height producers need
  to publish changes, not only the rocket path.
- Conservative coarse/fine transfer: tile birth transfers existing coarse
  volume; replaced area receives rain/infiltration/evaporation once; reciprocal
  boundary fluxes use areas and exactly one donor limiter. Isolated fine bowls
  without exchange would break valley/catchment flow and double-count rain.
- One fixed-step order independent of frame partitions and presentation. Keep
  pending time, budget totals and tile generations. Current partition tests
  cover constant forcing; delayed time uses the later call's weather, so varying
  forcing/backlog needs an explicit integration test too.
- Coherent `At`, snapshot/upload and GPU tile support. Current publication is
  one rectangular texture with one spacing/origin. An independent visual tile
  overlay cannot represent authoritative hydrology. Actual terrain/crater
  support and generation admission must remain strict.
- Tests for an off-coarse-stride crater, tile-border spill/downhill valley,
  coarse/fine volume conservation, repeated edits/restoration, pause/partitions,
  allocation exhaustion, mission/regrid reset and camera move/return. Runtime
  proof needs a real impact crater plus actual terrain and water-height samples,
  then matching installed captures; synthetic bowls alone do not close it.

The domain fix is ready independently. Fine tile implementation remains a
proposed scoped follow-up, not an implemented or installed tiny-hollow claim.
