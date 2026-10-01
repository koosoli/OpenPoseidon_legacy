# Actual-edit fine rainwater producer and query gate

Status: CPU/source candidate, default OFF. Renderer/FFI integration, engine build
and installed-device acceptance remain pending. This is not a shipped crater
water claim. Baseline: `c666e357`.

## Source and ownership

`POSEIDON_RAIN_WATER_FINE=1` opts World into the conservative fine backend.
Without that exact value the existing RainWaterField simulation, vertex
publication, forcing and queries remain the default. There is no world/map gate
and no camera admission. An enabled world with no admitted real edit retains
the legacy backend too.

The central `Landscape::HeightChange` publisher records only in-range finite
committed heights that differ from the previous raw height. `SetData` and the
ordinary HeightRevision increment still happen in their existing order. A
finite unchanged write covers revision continuity without creating a footprint;
raw-height quantization therefore cannot fabricate an edit or spuriously break
an otherwise complete brush transaction. Nonfinite/out-of-range or unobserved
revisions remain a coverage gap. The publisher does not claim every editor
caller committed a Box3D patch: the tested crater/brush producers do so, while
existing editor HeightChange callers retain their own behavior.

The rainwater-private journal stores at most 32 coalesced actual source-vertex
rectangles and keys them by Landscape owner, range, spacing and revision. It is
not a shared multi-subscriber journal. First rain Configure preserves same-mission
queued edits; explicit mission Reset retires both edits and backend. Gaps or
overflow refresh already-owned geometry and the coarse bed, but cannot promote
a new fine tile. No-op-only receipts and an unknown initial source admit none.

Initial admission is intentionally narrow: native 6.25 m source, aligned stride4
25 m water grid, zero legacy origin, at most eight complete interior inland
tiles. Each tile owns 8x8 old parent rectangles, split into 32x32 fine cells,
using 33x33 actual source vertices. Any new-tile corner at/below sea, nonfinite
corner, partial source range or partial parent tile refuses admission. A ninth
tile remains coarse; wet admitted tiles never disappear because a camera moves.
The complete coarse simulation is retained, with disjoint fine ownership at
admitted parents and donor-limited shared-face coupling.

Legacy node `(x,z)` is the centre of its old equal-area control rectangle. The
fine core origin is `oldOrigin - spacing/2`. At 25/6.25 m, tile `(tx,tz)` reads
source corners starting `(32*tx-2,32*tz-2)`. Tile `(2,2)` starts at world
`(387.5,387.5)`, not `(400,400)`. Fine beds are the actual antidiagonal two
terrain triangles; volume inversion supplies a horizontal water head and query
depth is `max(0, head - triangleHeight(localPosition))`.

World samples `GetHeight(z,x)` after source admission, not SurfaceY (which adds
mud/sand displacement). Landscape's out-of-range `ClippedData` is the actual
`YOutsideMap=-100` sea sentinel, not a clamped edge height. Fine source bounds
are checked before any sample and never use that sentinel as geometry proof.

## Lifetime, mass and cost

The first admitted edit imports old per-node `depth*spacing^2`, all water budget
lanes, and pending fixed-step time. Refinement preserves each parent's volume;
its initial subdivision is uniform by child area, followed by actual-corner
head inversion and subsequent conservative flow. Instant local hydrostatic
equilibrium after refinement is not promised. Existing geometry edits preserve
retained volume and update head rather than erasing water.

A recorded edit immediately increments the field revision and closes active
fine queries/publication. Unknown/nonfinite source keeps them closed and retains
pending time and mass; it never advances against stale fine beds. Successful
refresh reopens them. Failed domain reconfiguration similarly retains volume,
closes the old coordinates, and cannot be revalidated through World's source
update while MatchesDomain is false.

An enabled backend's real terrain regrid conservatively remaps overlapping
owner areas, redistributing uncovered volume to the nearest new boundary owner.
It preserves total mass, budget and pending time and retires fine topology;
shorelines and curved subcell wet placement are explicitly redistributed. The
default legacy reset policy remains unchanged. Unsupported spacing continues
with conservative coarse ownership after this explicit regrid, not invented
fine terrain.

Unchanged source revision performs no terrain reads or geometry rebuilds.
Changed-source refresh reads at most eight existing 33x33 patches plus admitted
patches, and examines at most 128 new candidate tiles; larger footprints refuse
fine promotion instead of scanning the world. Initial admission rebuilds the
core topology per admitted tile, bounded to eight tiles, so cold edit cost still
needs installed timing. Existing all-coarse bed refresh remains unchanged.
The fine-only snapshot visits at most 512 parent records/8192 surfaces and uses
cached heads; it does not scan every coarse owner or reinvert every cell.
Production simulation still sweeps the coarse domain. The earlier pure-core
benchmark is evidence for that cost, not an installed producer timing claim.

On this compiler, `Surface` is 104 bytes: eight-tile snapshot payload is
0.8125 MiB before vector/allocator and later FFI/GPU copies. The core also
retains coarse topology and RainWaterField keeps legacy proxy arrays; this
candidate does not claim the 0.8125 MiB is the entire backend allocation.

## Renderer handoff contract (still required)

`FineSnapshot()` publishes generation, field revision, applied terrain source
revision, validity, and surfaces containing owner rectangle/head/volume/flow,
actual four-corner geometry, and legacy parent index. Coarse `Snapshot()` still
publishes old RGBA proxies: refined parents contain aggregate child volume,
not additional simulation ownership.

The renderer must atomically pair the two publications and the actual bare
terrain revision. `SourceReady()==false` must suppress the complete water
publication. Fine `valid==false` alone can also mean zero fine tiles: a healthy
coarse-only backend is still legal. Fine rectangles remain present during
staleness so ownership can be retained without drawing stale heads. Refined
coarse proxies MUST be excluded over those exact physical rectangles; simply
adding a fine layer to the old interpolated grid would double draw water.

Render each fine owner at its horizontal head, with fragments clipped against
its actual two-triangle terrain bed and strictly positive local depth. Never
draw an inclined/sloping water cell, a whole rectangle over a dry upslope
triangle, or a centre-depth plane without actual bed clipping. Reuse existing
opaque support, raised-road/floor, physical roof exposure, snow/sea, reflections
and finite-depth guards. Ocean, wakes and downwash are outside this change.
GPU interpolation and boundaries must use the same origin/diagonal as CPU.
Metadata can be cached by topology generation/source revision; ordinary steps
can update packed head/flow rather than copying full CPU records blindly.

## Validation and acceptance gaps

`scripts/Test-RainWaterField.ps1` compiles the actual headers with Clang C++20
`-Wall -Wextra -Werror` and runs legacy policy plus producer/query checks:
default-off equivalence; genuine before-rain receipt; no-op continuity without
admission; exact rectangle/corner mapping; actual TerrainCraterDepth source edit
between old samples filling above 3 cm; mass/budget/pending preservation; pause;
unchanged-source zero reads; immediate stale closure/restoration; missing
revision/overflow; finite/interior/inland admission; eight-tile retention;
nonfinite source; invalid identity and failed regrid closure; valid regrid mass;
mission Reset; and constant-forcing frame partitions.
`scripts/Test-RainWaterFine.ps1` additionally verifies the production pure-core
owner/flux, geometry integration/inversion, spill, sea, mass and partition gates.
Both passed for this candidate. Full World/Landscape compilation is pending.

Remaining physical limits from the core assessment still apply: disconnected
pockets inside one 6.25 m cell share a head; the existing minimum-bed sea outlet
policy is approximate for a later edit crossing sea; coarse beds remain centre
approximations outside admitted patches; nonlinear pairwise equilibrium is not
solved exactly, though shared-face donor caps preserve mass. Rainfall/loss
forcing retains the existing whole-owner policy rather than a new per-roof
hydrology model. Eight tiles are a bounded first slice, not all-world microgrid
coverage or sub-native-resolution crater geometry.

Before default activation: root must integrate the paired fine renderer,
compile the real engine, execute real-device geometry/resize/stale/regrid tests,
and run an installed actual terrain edit/restore with fixed rain and paused
state. Capture the true hollow, flat/slope dry upsides, coarse/fine boundaries,
raised support/inside floor negatives, dry/rain/snow controls, camera return,
footstep/rotor consumers, and volume/source-revision diagnostics. Measure cold
admission and sustained simulation/publication cost at the eight-tile cap.
CPU tests alone do not close the user's visible crater-water requirement.
