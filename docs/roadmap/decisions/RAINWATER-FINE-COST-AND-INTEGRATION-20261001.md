# Fine water cost and production migration gates

Source: `bd3cfb3f` fine prototype versus actual `RainWaterField` containing
`dd6e5ef7`. This assessment adds a reusable CPU benchmark only. World, Landscape,
graphics, ABI and shaders are not changed. Installed crater filling remains open.

## Actual CPU measurement

Hardware: Intel Core i7-11700F, 8 cores/16 logical processors. Compiler: Clang
22.1.0, `-O2 -DNDEBUG -std=c++20 -Wall -Wextra -Werror`. Run
`scripts/Measure-RainWaterFine.ps1`; CSV is retained in
`build/rain-water-fine-policy/fine-cpu.csv`. Single-thread calls are sequential;
each case has five warm-up steps and three alternating implementation-order
repeats of 32 actual quarter-second steps, then 16 actual snapshots. A Clang
memory-escape barrier observes the complete snapshot without timing another
checksum sweep, so dead payload writes cannot be removed. Field copies,
configuration and tile topology construction are outside the timed sections.
Weather, step time, domain dimensions and forcing are identical. Deep water is
one matching 1,250-cubic-metre seed (two metres in one coarse owner), with rain
wetting the rest; it is not an entire world seeded to two metres. The crater
case intentionally has finer actual terrain geometry absent from the coarse
stride, as that is the feature being measured.

The owner's game/background workload was not controlled. These are CPU
comparisons on the same machine, not quiet installed-game frame timings or a
guarantee of stable frame latency.

| Scenario | Legacy Advance | Fine core, coarse only | Fine core, 64 tiles | 64-tile/legacy |
| --- | ---: | ---: | ---: | ---: |
| Closed shallow | 4.91 ms | 10.24 ms | 12.37 ms | 2.52 times |
| Closed deep seed | 5.55 ms | 11.72 ms | 14.28 ms | 2.58 times |
| Actual sampled crater | 5.37 ms | 11.03 ms | 14.60 ms | 2.72 times |

Values are medians per quarter-second simulation step. The full prototype would
cost roughly 49–58 ms of this CPU per simulated second, versus 20–22 ms for the
legacy field. At four-times simulation speed, that becomes about 207–228 ms per
wall second. A 15-ms single-step spike and accumulated multi-step backlog are
material frame-time risks even though this is below a ten-times regression.
Do not replace the production solver solely on these results.

| Scenario | Legacy snapshot | Fine coarse-only snapshot | Fine 64-tile snapshot |
| --- | ---: | ---: | ---: |
| Closed shallow | 1.99 ms | 4.87 ms | 5.69 ms |
| Closed deep seed | 2.11 ms | 4.16 ms | 4.98 ms |
| Actual sampled crater | 1.62 ms | 4.15 ms | 5.51 ms |

Actual payloads are 4.02 MiB legacy RGBA, 12.05 MiB coarse-only owners and
14.86 MiB maximum fine owners. The owner struct is 48 bytes, correcting the
earlier approximate 18-MiB snapshot estimate. These timings include allocation,
construction and destruction of the actual snapshot. They exclude the existing
second C++ copy, FFI, Rust retention and GPU upload.

Before integration, retain the fast regular coarse sweep or optimise the pure
prototype's common dry/equal-head face path, cache area/head quantities, and
avoid rebuilding a world-sized topology separately for every admitted tile.
Re-run identical conservation/partition tests and this benchmark after each
change. Publication should carry rectangle/tile metadata only when its topology
generation changes; ordinary steps can update packed head/depth/flow lanes.
Uploading 48-byte owner records every quarter-second is not a sensible default.

## Explicit legacy-node mapping

Legacy volume is `depth * spacing squared` for every node. Treat each old node
as the centre of that equal-area rectangle: new coarse rectangle origin is
`oldOrigin - spacing/2`, and each new owner receives exactly that node's old
volume. This preserves node positions, area and water, unlike interpreting old
node coordinates as new rectangle lower-left corners, which shifts everything
by half a cell. At 25 metres, child rectangle bounds then fall on multiples of
6.25 metres, matching authored native terrain vertices; child centres are
offsets -9.375, -3.125, 3.125 and 9.375 metres from their parent centre.

The old equal-area node domain extends half a cell outside its query rectangle,
and the last strided terrain sample may be clamped. That is a preexisting
boundary approximation, not actual geometry for a fine tile. Initial production
refinement must reject tiles whose complete rectangles/source halo leave the
real terrain extent. GPU `bare_surface_valid` remains mandatory. Fully correcting
world-border control-volume areas is a separate conservative migration, not a
silent clamped-height proof. Mission initialisation can start the new backend
dry; an optional live backend switch must use the explicit mapping above and
preserve budget/pending time/generation.

## Narrow source and lifecycle hooks proposed

1. Add a bounded terrain-height footprint journal next to
   `Landscape::HeightChange`/`HeightRevision`. Successful changed vertices publish
   a source-grid rectangle and revision; coalesce a brush/crater patch rather
   than retaining every vertex event. A subscriber asks for changes since its
   own revision. Overflow returns an explicit full-resample requirement; a reader
   must not clear a shared journal used by other consumers. Terrain generation,
   range and spacing retire all prior footprints on load/regrid. The existing
   undo baseline journal is not this stream.
2. Keep actual `TerrainBrush::PaintTerrain` transaction order: Box3D patch first,
   then `HeightChange`, then `FlushCache`. Restoration uses the same publisher.
   No fine tile is admitted from an uncommitted physics/terrain edit. Other
   height producers and `LandSave` grid changes need corresponding journal
   reset/full-change publication.
3. Provide a read-only bare-terrain triangle query in Landscape, separate from
   `SurfaceY`, which currently adds mud/sand offsets. Use the actual four source
   heights and triangle diagonal, finite/in-range validation and source identity.
   Do not subtract a later mud/sand sample from a previous `SurfaceY` result.
   Fine tile bed/geometry sampling happens after the edit transaction on the
   simulation thread. Mud/sand support can be added only with their own revision
   and storage policy.
4. World consumes complete footprints once per fixed simulation boundary,
   admits/coalesces interior tiles under the existing fixed budget, updates
   existing tiles even when full, and publishes one coherent generation/revision.
   Source sampling and topology work must have an explicit budget/backlog; an
   edit cannot publish stale water over fresh terrain while waiting. No camera
   admission or wet-tile eviction. Ordinary `At` remains read-only, preserving
   footstep/rotor consumers and actual sea/raised support checks.
5. `WaterWgpu::QueueRainWater` currently copies a vertex RGBA grid through
   `EngineWgpu::QueueRainWater`; Rust caches its generation/revision/domain. A
   separate versioned fine publication should carry a coarse ownership mask,
   packed per-tile head/depth/flow and immutable tile geometry metadata. Producers
   must publish all components atomically with terrain source revision, and
   invalidate missing/stale pairs. Ocean/wake/downwash resources stay separate.
   Existing `TerrainWgpu::UploadIfNeeded` already observes HeightRevision; that
   does not by itself prove the new water/heightmap pair belongs to the same edit.

## Geometry gate before runtime visualisation

A centre-bed cell is a finite-volume approximation, not the actual integrated
volume of a tiny bowl. Drawing a horizontal plane at its head and clipping by
actual terrain is necessary, but alone can leave adjacent rectangular seams or
incorrect fill levels. It must not be claimed sufficient for this requirement.

Recommend the next pure-core gate: supply actual four-corner/two-triangle fine
geometry, integrate the volume above those triangles as a monotone function of
water head, and invert that function from retained volume. Use actual shared
face height profiles for spill/wet-face admission. This preserves water while
placing the local plane at a terrain-supported level, rather than `centreBed +
volume/area` or a sloped `bed + constantDepth` carpet. Test real triangular
crater fill height, dry corners, spill edge, source changes and conservative
coarse/fine exchange before source wiring. Disconnected basins below a ridge
inside one cell remain a resolution limitation and need explicit treatment.

The eventual fine shader emits a horizontal water plane, uses the matched actual
bare heightmap at each fragment, rejects invalid terrain and `head <= bed`, and
uses `head - bed` for optical depth. Real terrain intersection determines the
shoreline, rather than a rectangle alpha mask. Parent-owned coarse rendering
must be excluded in fine-owned rectangles. Continuity needs tests at wet cell
faces and tile borders; cosmetic blending cannot license water above a dry rim.
The current coarse `abs(sampledBed - actualBed) <= 0.15` check is appropriate for
the old bed publication; a new geometry-backed head publication needs its own
explicit source-revision admission, not simply removal of that gate.

These hooks are proposals, not implemented changes. Next runtime acceptance
requires actual crater geometry, sampled centre/rim/valley levels, conservation,
no doubled water, supported shorelines, pause/camera-return and measured frame
cost on the matched installed pair.
