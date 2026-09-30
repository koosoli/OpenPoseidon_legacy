# Conservative fine-support CPU prototype

Status: pure core and CPU tests, **not runtime integrated or installed**.
The existing `RainWaterField`, world producer, presentation ABI, terrain and
ocean remain unchanged by this candidate. The domain fix in `dd6e5ef7` is a
separate preceding commit. A passing prototype does not close actual crater
water filling in the owner's game.

## Ownership and coupling

`RainWaterFine.hpp` defines rectangular finite-volume owners. Configure accepts
actual supplied coarse bed samples. Each coarse rectangle has a volume and area;
depth is volume/area and head is its sampled bed plus depth. A tile replaces up
to eight by eight coarse owners with four by four children per parent, giving a
maximum 32 by 32 fine cells. At 25-metre coarse spacing the fine cells are 6.25
metres wide. Tile keys are absolute domain indices, not camera positions.

The caller must supply actual terrain triangle heights at fine-cell centres.
The core generates no crater or terrain noise. Refinement splits a parent's
existing volume equally among its equal-area children and deactivates the parent.
Only active owners receive rain, infiltration, evaporation and sea outlet loss.
Repeated edits/restoration change beds without changing volumes. A fixed
64-tile budget refuses new tiles before modifying any ownership; existing wet
tiles remain present, and already admitted tiles can still update at capacity.
No read/query, camera input or ordinary step removes a tile.

The topology enumerates each internal shared face once, including the four fine
segments of a coarse/fine face. Flux volume uses the shared face length, wet
donor depth, the existing bounded head-driven speed, and half the exact two-cell
equalisation volume. It then sums *all* outgoing proposals per donor and applies
one donor-volume limiter to each reciprocal transfer. Water cannot be spent
independently on multiple faces. Sea loss remains an explicit budget sink.
Faces are traversed in spatial order independent of tile event order.

The same quarter-second integration and maximum 16 steps per call retain pending
time. Tests cover constant-forcing frame partitions, not arbitrary historical
weather changes: as in the existing field, queued time is drained using the
latest call's weather. That separate limitation must be addressed before a
varying-weather backlog equivalence claim. Queries do not advance simulation.

`Regrid` only accepts an equal world footprint with unchanged origin. It remaps
each owner's uniform volume density to new coarse rectangles by actual overlap
area, preserving rain/loss budget and pending time. It advances generation and
explicitly coarsens the admitted tiles. A changed world footprint is refused;
mission reset is explicit. This does not retrofit the legacy world domain reset
policy or silently remap existing production data.

## Publication and production integration required

Snapshot is an immutable, spatially ordered list of disjoint rectangles with
sampled bed, volume, size and flow. Generation/revision are separate accessors.
An eventual producer must latch all of them together after simulation on the
main thread. This is **not** the existing vertex RGBA grid publication format.

Before integration, explicitly map legacy equal-area node volumes to defined
rectangles. The old field's query coordinates represent terrain vertices while
its volume counts each vertex as a whole cell; this prototype uses rectangle
origins and centre samples. Copying the old depths without defining that spatial
mapping would shift ownership. Source range/grid, actual terrain generation,
height revision and complete dirty edit footprints must key admission. Repeated
edits and restoration need fresh actual bed data, and an exhausted footprint
journal must fail safely or resample, rather than miss edits.

Runtime `At`, source snapshot/upload and renderer must change coherently. Centre
bed samples are finite-volume approximations of the real triangles; they do not
encode an exact integrated terrain hypsometry. A free surface must be drawn as
the owner's horizontal water plane and clipped against the actual piecewise
terrain, with real per-fragment water depth, coverage and sea/roof support.
Interpolating sampled bed-plus-depth into a sloping solid water cell would not
show correct hollow filling. The current 15-cm support mismatch check cannot be
simply disabled. Exact below-rim disconnected microbasins within one fine cell
still require more geometry resolution/hypsometry. Mud/sand deformation needs
its own source revision and support policy, not just `HeightRevision`.

The first production admission should use real height-edit footprints. Natural
small hollows require a separate bounded source-geometry catchment analysis; a
crater-only tile producer would not satisfy the entire valley-flow request.

## CPU evidence and costs

`scripts/Test-RainWaterFine.ps1` compiles the actual core under Clang C++20 with
`-Wall -Wextra -Werror`. It passed these assertions:

- Exclusive owner area and integrated rain/loss mass before/after refinement.
- Reciprocal coarse-to-fine and fine-to-coarse flow, including both face axes;
  multiple outgoing faces remain nonnegative and conserve the budget.
- A crater made with the actual `TerrainCraterDepth` source rule at 6.25-metre
  terrain vertices, evaluated through the engine's triangle diagonal. Every
  old 25-metre sample remains unchanged, while the fine sampled depression
  collects over 3 cm and over twice flat-ground depth after 120 simulated
  seconds of natural rain. This is a source geometry fixture, not an installed
  explosion or a runtime positive.
- Below-rim fine retention, real bed-lowering spill into a coarse neighbour,
  sea drainage, repeated edit/restoration and same-footprint overlap regrids.
- Pause/read stability, opposite tile activation order, constant-rain frame
  partitions, bounded retained backlog, budget exhaustion with wet retention,
  finite admission, invalid/outside queries, and mission generation reset.

The existing `Test-RainWaterField.ps1` also passed unchanged. `git diff --check`
passed. No game, GPU test or full engine build was run for this prototype.

Unlike the earlier scalar-only estimate, this straightforward prototype retains
explicit owners, topology, donor sums, deltas and face fluxes. The actual CPU
test reports vector **capacities** for a 513-square domain: 29.74 MiB coarse-only
and 33.75 MiB with 64 full fine tiles, excluding map nodes, snapshots and allocator
overhead. This is about five times the existing six-array field storage. A full
snapshot can add about 18 MiB at maximum ownership. Topology rebuild currently
scans the bounded coarse domain after each new tile; this is event work, not a
per-frame camera scan, but batching/incremental rebuild and measured cost remain
necessary. The earlier 1.5 MiB number described only 65,536 extra cells times six
floats, not the adaptive core or its metadata.

Next gate: review this ownership/model migration, then wire real edit sampling
and coherent renderer publication under separate ownership. Require an installed
real crater missed by the old stride, actual centre/rim/valley water-height
samples, dry/sea/raised-road/interior negatives, spill and camera move/return,
and performance at ordinary and accelerated simulation. Do not enable the
prototype as default simply because these CPU tests pass.
