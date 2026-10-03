# Source-qualified retained-water soil medium

Candidate only. Full Rust/C++ build, device execution, installation and actual
original-map visual acceptance remain the coordinator's next checks. Six
cached-library CPU checks passed; no game or device was started for this slice.

## Why this slice

The calibrated installed `94f261c5` campaign
`build/original-weather-player/calibrated-rain-soil-water-views-20261003-164853-3b21d9`
reduced the excessive retained-water mirror: the coordinator recorded 6.638 mm
water at the actual contact and 14.576 mm at another genuine Field point. This
changes the water/rough-substrate handoff without requiring a global tint. The
previous retained-water shader nevertheless had no source-specific medium;
actual admitted Field, Grass, sand and hard ground all shared one body/extinction.

This slice adds conservative source lookup and a bounded scalar optical medium.
It does not simulate sediment concentration, erosion or sediment transport.

## Exact source and lifetime contract

The actual terrain `index_map`, material storage and parameter uniform are
borrowed into previously empty rainwater group 3. There is no new public C++ ABI,
semantic texture, source-grid upload or per-frame all-land `SurfaceAt` scan.

Admission requires the actual in-domain world-cell row, current ready source
generation, valid storage index, ground receiver bit, and `surface_count == 0`.
Only existing pure legacy/native soft-soil bit 1 or the existing cultivated
bit-2 central-cell fraction qualifies. Hard ground, sand, unknown/non-ground
rows and authored multi-surface masks remain clear water. The index query never
clamps an off-map fragment onto an eligible edge. Existing cultivated-source
classification is reused; no new filename or colour heuristic is introduced.

`TerrainWgpu::UploadIfNeeded` now publishes its existing actual source key for
coarse as well as fine water. On the CPU's proven `!sameMap` event, an explicit
zero source is published before the new height upload. This invalidates prior
material/index readiness even when a Landscape address and dimensions are
reused. Source/table delivery before the new height receipt cannot admit the
old geometry. A same-map height/fine-epoch edit disables the medium between
height upload and the actual source receipt, then preserves the existing
material resources. Both new index and table receipts are required after an
actual new-map reset. Invalid or missing resources yield the current clear
medium, rather than suppressing valid water geometry.

An internal resource revision advances independently for source, height, index
and table replacement, so a same-size storage replacement rebuilds the borrowed
bind. This does not depend solely on heightmap allocation generation. The
existing fine publication validation and ownership lifetime remain separate.

The exact 384-byte WGSL TerrainMaterial row is shared by terrain and rainwater.
Its former `pad0` shader name is `legacy_satellite_generated` because naga_oil
refuses the former name inside a composable module. Its offset and generated
satellite branch meaning are unchanged; public Rust/C++ structs are unchanged.
The shared row's flag offset is checked against the real ABI.

## Optical calibration and energy

The source fraction interpolates scalar extinction from the current **3/m** to
**18/m** (soil-medium e-fold length 5.56 cm). Both use true Beer–Lambert
`exp(-extinction * supportedLocalDepth)`. Reflection Fresnel, depth-based
rough-film handoff, normal filtering, GGX sunlight, SSR and all lighting/shadow
inputs are preserved. There is no artificial opacity floor or dependence on
instantaneous rain, so the retained medium persists when the drops stop.

The admitted soil body has low **linear** reflectance `(0.035, 0.018, 0.007)`;
the unchanged clear body is `(0.045, 0.060, 0.052)`. A per-channel reflectance
ratio applies to the already computed direct/ambient body illumination. Every
ratio is at most one, preserving actual sun visibility/night behaviour and
preventing a luminous milky scattering body. These are explicitly calibrated
appearance constants, not measurements of suspended sediment. No RGB absorption
or refracted-bed composition is claimed: destination transmission remains scalar.

Reflection + weighted body + transmitted destination sum to one under the
existing straight-alpha blend. At zero source fraction the previous clear
helper is retained exactly. At zero depth/coverage the medium returns zero.

No RainWaterField/RainWaterFine volume, source forcing, physical native bed,
flow, raster geometry, roof, snow, sea, support, pause or ownership policy changes.

## Verification and next runtime

Cached-library CPU checks used the actual production SourceLifetime module,
actual ABI structs and production shader composer; **6 passed**:

* Required current-world index/table receipts and independent replacements.
* Different-world same-size replacement.
* Explicit new-map reset with a reused pointer, including reordered receipts.
* Same-world height edits, changed source generation and invalid height/source.
* Complete actual terrain/rainwater composition and shared 384-byte row/flag offset.
* New actual compute-probe composition without a device.

Root device targets, using its existing serialized cargo/device environment:

```
cargo test --manifest-path engine/WgpuRenderer/rust/Cargo.toml rain_water::medium_tests:: -- --test-threads=1
cargo test --manifest-path engine/WgpuRenderer/rust/Cargo.toml rain_water_medium::tests:: -- --test-threads=1
cargo test --manifest-path engine/WgpuRenderer/rust/Cargo.toml rain_water:: -- --test-threads=1
```

The new actual compute test reads the production source-query code with real
index/table/uniform resources. It checks pure Field/cultivated positives and
hard/unknown/non-ground/mixed/out-of-range/nonfinite/off-map/stale-generation
negatives; unchanged clear-source output; scalar optical energy over zero,
millimetre, actual calibrated/previous campaign and deep depths; finite bounds;
and a black night body/reflection. Existing actual full coarse/fine VS/FS
admission tests now bind the production medium layout with readiness zero, so
all native/roof/snow/sea/raster negatives remain independent clear controls.

After full build/deployment, rerun the actual calibrated Eden contact low/top
views with source/depth receipts; inspect both the Field and refused Grass
neighbours. Keep the soil-agent's rough-substrate sheen separate. Check original
OFP sand/hard and unknown Cain controls, and an actual world replacement. A
visually darker brown pool is not sufficient proof of correct source admission
or physical drainage. No visual acceptance is claimed here.

### Invalid-coordinate device counterexample

The first actual GPU source probe admitted the NaN world-x case (quiet NaN
`0x7fc00000`) as pure soil even though `cell != cell` was intended to refuse it.
The strict expected refusal remains. The source query now checks the IEEE
exponent through integer bitcasts before world-coordinate arithmetic and again
on the computed cell. This excludes NaN and both infinities without relying
on driver floating-point self-comparison behavior. Ordinary finite bounds,
source identity, index limits and original material eligibility remain in force.
Cached CPU composition checks precede the root's actual device rerun; visual
acceptance of muddy retained water remains pending installed-game testing.
