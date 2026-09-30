# Simulation rainfall water presentation

## Source and ownership

This adds an independent rainwater surface pipeline, shared by Current OP and
Tidewater Native. It does not change either ocean simulation, accepted rotor
water interactions, ocean nodes or water material parameters. CPU hydrology
remains owned by `RainWaterField` and advances in the world simulation, including
downhill transfer, hollow storage, slow drainage and terrain-bed updates.

Dependencies are root `88de8e3c` (snapshot/world integration), `dba0072c`
(mission generation) and `9b025ec3` (aligned native grid and triangular bed).
The renderer copies the immutable RGBA snapshot
`[physical surface height, depth, flow X, flow Z]`. Camera selection never seeds,
advances, drains or erases simulation cells. Generation, revision, origin,
spacing and dimensions identify uploads; simulation time and rain update only
the small uniform. Generation prevents a same-map reload from reusing stale
cells when the revision happens to match an old mission.

The main-thread `WaterWgpu` producer queues an owned snapshot through the
existing `EngineWgpu` pending-settings worker drain. A second camera's unchanged
header cannot erase an already queued changed-grid payload. No direct producer
call borrows the live Rust renderer during render overlap.

## Geometry and material

The new `RainWater` pipeline draws actual simulation surface height, with four
subquads per cell. It reconstructs corner bed heights from stored surface minus
depth and samples the same anti-diagonal terrain triangles as `RainWaterField.At`
and `Landscape.SurfaceY`; depth and signed flow remain bilinear. The mesh uses
the matching anti-diagonal, with subquads approximating smooth depth. Fragment depth
comes from the simulation, and fragments with at most 1 mm of water are clipped.
The shader imports the actual production bare-terrain height sampler. It
rejects out-of-map terrain, below-ground surfaces, active sea water, snow cover,
and unknown/blocked physical rain cover using existing directional depth maps.

The coarse field can disagree with detailed terrain. A fragment is therefore
rejected when `[physical surface height - physical depth]` differs from actual
bare terrain by over 15 cm. This prevents floating water without replacing the
simulation's authoritative height with a cosmetic terrain-plus-depth plane.
Native source-grid strides are now aligned powers of two, avoiding sampling
drift, but coarse-map coverage refinement remains pending. Mud/sand depressions and
rigid paving are not newly resolved impermeable hydrology boundaries here.

Water normals use the physical surface gradient, bounded flow animation and
the shared actual-liquid-rain ripple helper. Distant footprint filtering
removes fine ripple/flow variation. Reflection samples the existing directional
atmosphere/cloud environment; no new unresolved sun-disc glint is added.
Transparent depth testing preserves opaque foreground objects. A zero-node
water plan marker at Scene's existing water boundary is submitted only when
the field has wet cells, preserving the transparent tail on inland views with
no selected ocean nodes. It emits zero ocean instances.

## Bounds, ABI and controls

- Simulation snapshots are at most 1024² cells; root currently configures at
  most 513² in ordinary worlds. A 513² RGBA snapshot/texture is about 4 MiB each.
  A native 2048-cell grid at 6.25 m uses stride 4 and spacing 25 m; a stock
  256-cell grid at 50 m retains stride 1 and 256 vertices.
- Camera selection is at most 65,536 cells, bounded by a 600 m rectangle and
  127-cell radius. Each selected cell has eight triangles. This is presentation
  culling only; returning the camera reads the same stored water.
- The additive `WgrRainWaterParams` ABI is 48 bytes: two vec4 float lanes, a
  uint64 generation and uint64 reserved lane. Existing `WgrWaterParams` remains
  1472 bytes. Both ABI layout hashes include the new struct, requiring a matched
  engine/renderer pair. Old struct layouts are unchanged.
- `WGR_RAIN_WATER=0` disables this new producer/pipeline/marker only.
  `WGR_RAIN_WATER_TRACE=1` records accepted upload generation/revision,
  dimensions, copied-cell count and actual maximum depth. The Water tab master
  switch also clears rainfall presentation without changing simulation water.
- No new fragment storage binding is added. The pipeline reuses Frame group 0,
  adds a sampled RGBA texture/uniform at group 1, and imports the actual bare
  terrain texture/uniform at group 4.

## Evidence and remaining checks

Agent ran `scripts/Test-RainWaterRendererPolicy.ps1` with clang++ and rustc,
both under warnings-as-errors. It compiles and executes the unmodified
production Rust dimension/validation/camera-selection/identity functions,
tests dry/malformed grids, finite camera and bounds, return without cell
mutation, and mission-generation identity. It compiles the actual C++ header
layout assertions and runs root's actual CPU runoff tests covering hollow,
spill, flow, water budget, drainage, sunlight, pause/reset/partition behavior.
The focused checks and `git diff --check` pass.

The Rust test module additionally contains full production shader composition
and a required-device falsifier calling verbatim production `water_at` and
`rain_water_support_depth` on real RGBA texture data. A nonplanar saddle with
varying water depth checks both bed triangles separately, the shared diagonal,
bilinear depth and signed flow, dry/submerged/coarse-bed rejection and finite clamped
border sampling. Agent did not run those tests, compile the full crate/game,
launch the game or deploy. Root must run `cargo test rain_water::tests` serially,
then the matched full build and installed capture/measurement campaign.

Required installed proof: actual-rain accumulation and hollow water surface,
downhill flow, dry/disabled controls, slow drainage after rain stops, exact
pause, camera move/return, actual source height/depth/flow telemetry, real roof
cover, sea/shore separation, foreground body/glass/particles, both ocean
backends and preserved helicopter water downwash. No appearance or all-map
physical-water acceptance is claimed from these source/CPU checks.

Current limitations include coarse-bed rejection coverage, finite physical
cover-map coverage, the independent artwork-AO/map-enable limitation already
documented by root, and no actor-footfall ripple queue in this first commit.
The triangular-bed renderer follow-up matches root's `9b025ec3` CPU policy,
whose canonical field checks pass. Its new GPU falsifier and installed height
agreement still require root's serial verification; this document does not
claim that source-string contracts establish actual shader execution.
