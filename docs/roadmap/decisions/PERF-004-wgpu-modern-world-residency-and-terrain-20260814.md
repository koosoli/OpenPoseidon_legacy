# PERF-004 — Modern worlds needed working sets, not a larger eager renderer

- **Date:** 2026-08-14
- **Backend:** WGPU only; GL33 is not a modernisation target
- **Scope:** CWA/OFP, Arma 1, Arma 2/OA, Arma 3, DayZ, and the current Reforger XOB foothold
- **Status:** implemented and runtime-checked

## Outcome

The performance cliff beginning with Arma 3 was not one slow terrain shader. It
was the compatibility bridge making fine terrain resolution multiply unrelated
CPU and GPU state, followed by an eager renderer that retained an entire modern
world forever.

The WGPU path now has bounded, camera-centred working sets for terrain materials,
world objects, object meshes and object textures. The authored coarse land grid
and fine height grid remain distinct. Planar water reflection scales with visible
water and uses a reduced-detail, one-sample view. Static roads reuse their authored
mesh and conform in the WGPU vertex shader instead of allocating a CPU-split mesh
per placement.

The full 2,971,218-placement ChernarusPlus database now reaches gameplay with
19,982 live objects at the reference camera. A deterministic traversal moved to a
non-overlapping 19,939-object set and returned to 19,982 with zero refused objects,
zero WGPU validation errors and a clean shutdown. Tracked WGPU residency returned
to 1.20 GB after the sweep; retired geometry stayed at zero.

## Why the cliff starts in the later generations

1. **The compatibility bridge erased the dual grid.** Modern WRP files have a
   coarse land/semantic grid and a fine elevation grid. The old bridge mounted
   everything at elevation resolution and replicated land cells `ratio²` times:
   16× on Sahrani and 64× on Takistan, Stratis and ChernarusPlus. This expanded
   geography, material and object-cell bookkeeping even though only height needed
   the fine grid.
2. **Placements became a database, but were treated as a scene.** The measured
   official worlds contain 602,481 (A1), 438,906 (A2/OA), 163,953 (A3) and
   2,971,218 (DayZ) placements. Eager creation decoded every referenced model,
   uploaded its textures and geometry, and created every scene object before a
   useful frame.
3. **Terrain material plates were globally resident.** Later worlds author
   thousands of per-tile LCA/LCO plates. Raising a bindless limit avoids wrong
   clamping but does not make loading the whole map a viable residency policy.
4. **GPU resource lifetime was effectively process lifetime.** Texture and mesh
   handles had no per-model retain/release ownership, and grown geometry-pool
   buffers remained in the retired list. Traversal could only increase memory.
5. **Road sharing was destroyed on the draw path.** An all-`ClipLandOn` road used
   `ShadowCache::Shadow(..., splitOnly=true)`, producing a terrain-fitted Shape and
   vertex buffer for each placement even though WGPU already had heightfield
   conform data.
6. **The reflection was another full scene.** The original planar pass rendered
   terrain, objects, sky and clouds at high resolution and main-view detail even
   for a small strip of water. Named GPU timers showed 20–29 ms planar frames in
   the DayZ policy/LOD experiments.

Arma 3 is therefore where the old assumptions become visibly expensive, but DayZ
is where they become impossible. Replacing terrain rendering alone would not fix
the placement, residency, road and reflection multipliers.

## Changes that landed

### Authored dual grid

`Landscape` now mounts land semantics at the land resolution and elevation at the
terrain resolution. World extents remain identical. Measured semantic-cell
reductions versus the old bridge are:

| Layer/world | Land grid | Terrain grid | Reduction |
|---|---:|---:|---:|
| Arma 1 Sahrani | 512² at 40 m | 2048² at 10 m | 16× |
| Arma 2/OA Takistan | 256² at 50 m | 2048² at 6.25 m | 64× |
| Arma 3 Stratis | 256² at 32 m | 2048² at 4 m | 64× |
| DayZ ChernarusPlus | 256² at 60 m | 2048² at 7.5 m | 64× |

CWA/OFP remains a same-grid eager world. That is deliberate: it is small and has
no modern authored material-page database to stream.

### Terrain material pages

Modern worlds bind a camera-centred 64-land-cell material window with a 1,024-layer
budget. CWA retains the eager 4,096-layer path. Page turnover replaces the Rust
bind group before evicting the old C++ texture handles.

The DayZ out-and-back diagnostic moved from 918 candidate materials / 337 bound
layers to 2,120 / 541, evicting 308 old page textures, then returned to 918 / 337
and evicted 510. No material was dropped and the final frame matched the fixed
camera.

### Object, model and texture working sets

Modern placement rows are retained compactly by land cell. The default live set is
20,000 objects inside 64 cells, created at 128 per update to remove the single-frame
decode/upload cliff. Duplicate authored IDs are reassigned above the source range.
Static marker-light ownership follows the streamed object.

WGPU model registration now captures the model's texture and dedicated mesh
handles. First live use pins textures. Last live use unpins/evicts unshared object
textures, destroys dedicated geometry, and removes the C++ registration so a later
visit can rebuild it. A generic last-used-frame texture LRU trims unpinned object
textures toward 90% of the dynamic VRAM budget; terrain pages remain under their
own owner. Geometry buffers destroyed this frame are retained only until the next
queue submission.

The earlier 100k DayZ run reached 4.64 GB tracked, including 855.6 MB of retired
geometry. With the working-set design, the full 2.97M database converges near
1.14 GB at the reference camera with retired geometry at zero. Final static
cross-generation captures measured approximately 269 MB (A1), 758 MB (A2/OA),
1.03 GB (A3), 1.14–1.20 GB (DayZ), and 49 MB (CWA).

### Shared WGPU road conform

For WGPU, an all-`ClipLandOn` road now draws the shared authored mesh through the
existing heightfield-conform vertex path. GL33 keeps its legacy CPU SurfaceSplit
path unchanged. The exact WLD-021 Takistan camera now shows a continuous road,
and the instrumented run emitted no road `surface-split` copy. This also removes
the unbounded per-placement `ShadowCache` allocation from WGPU road-heavy worlds.

### Planar reflection policy and timers

Named timers now separate main terrain prepass/colour and planar sky, terrain,
objects, clouds and mip work. The planar path uses exact water occlusion coverage,
a faded coverage threshold, adaptive resolution, reduced-view LOD, one-sample
pipelines and bounded refresh/freshness.

At the same DayZ reference experiment, the pre-policy reflection recorded
26.92–29.21 ms. The final one-sample run recorded 5.356 ms (sky 0.876, terrain
1.147, objects 0.784, clouds 1.773, mips 0.776): about 80% below the 26.92 ms
sample. These are same-machine diagnostic samples, not portable frame-rate claims.

## Cross-generation result

| Layer | Policy | Final runtime witness |
|---|---|---|
| CWA/OFP | eager same-grid | capture clean; eager terrain materials; 49 MB tracked |
| Arma 1 | dual-grid + pages + objects | 19,993 objects, 285 layers, 269 MB, clean |
| Arma 2/OA | dual-grid + pages + objects + shared roads | 20,000 objects, 316 layers, 758 MB; reference road clean |
| Arma 3 | dual-grid + pages + objects | 19,998 objects, 96 layers, 1.03 GB, clean |
| DayZ | full bounded stack | full 2.97M database; traversal and page return clean |
| Reforger | parser foothold only | 7 XOB tests / 69 assertions pass; no world renderer exists yet |

The Reforger statement is intentionally narrow. XOB direction packing, UV
dequantisation, 32-bit triangle counts, list remapping, chained LZ4, refusal of
missing UVs and byte-accounting closure are checked. There is not yet an Enfusion
world/terrain compatibility layer to performance-test.

## Robustness found by traversal

The first DayZ sweep crashed in `Poly::CalculateArea`. The offending tractor LOD
contains 49,135 source vertices while the base runtime face stream stores signed
16-bit `VertexIndex`. Indices above 32,767 wrapped negative. `ShapeAdapter` now
keeps the representable vertex prefix and rejects only unrepresentable faces;
`Shape::RecalculateAreas` independently refuses any invalid face vertex. The
measured model rejected 8,833 faces and the world continued. A synthetic 32,769
vertex regression test guards the boundary.

## Controls

- `WGR_OBJECT_STREAMING=0` restores eager modern object creation for diagnosis.
- `WGR_OBJECT_STREAM_RADIUS_CELLS` (default 64),
  `WGR_OBJECT_STREAM_MAX_OBJECTS` (20,000), and
  `WGR_OBJECT_STREAM_CREATES_PER_UPDATE` (128) tune the live set.
- `WGR_TERRAIN_PAGE_RADIUS_CELLS` (64) and `WGR_TERRAIN_PAGE_LAYERS` (1,024)
  tune terrain pages.
- `WGR_TEXTURE_EVICT_GRACE_FRAMES` defaults to 120 frames.
- `WGR_DYNAMIC_VRAM_MB` supplies the tracked dynamic budget.
- `WGR_OBJECT_STREAM_TEST_SWEEP_CELLS` and
  `WGR_TERRAIN_PAGE_TEST_SWEEP_CELLS` are deterministic diagnostic hooks, not
  gameplay settings.

## Remaining work

The dominant structural costs are now bounded. The next performance work should
be measured, not assumed:

1. recycle Rust model/section metadata slots after streamed model destruction;
   GPU bytes are bounded now, but the small metadata arrays are append-only;
2. move cache-miss ODOL decode and texture transcode to staged worker jobs so the
   128-object batch cannot produce traversal micro-hitches;
3. evaluate GPU CDLOD node selection only if the new main-terrain timer shows CPU
   selection or submission is material; and
4. design Enfusion world/terrain ingestion before choosing a Reforger terrain
   renderer. XOB alone is not evidence for either virtual texturing or meshlets.

The architectural rule is now explicit: compatibility layers translate authored
data, while the base WGPU engine owns bounded residency, shared resource lifetime
and pass scaling. A fix in either location must be checked against every older and
newer layer, with CWA's deliberate eager policy and Reforger's current absence
recorded rather than guessed.
