# Earth coast coverage transition

Source checkpoint based on `a6e6e1e4`, branch
`codex/earth-coast-transition-20261008`. Saved installed image
`build/earth-streaming/20261008-101152/coast-1000m.png` still failed visual
acceptance despite passing diagnostics: the detailed sea ended on two straight
edges, revealing dark opaque terrain/overview background. No game, GPU test,
full renderer build or deployment was performed for this source correction.

Two explicit limits caused the rectangular border: `water_tw/mod.rs` rejected
all selected nodes outside the resident heightmap rectangle, and
`tw_water.wgsl` discarded crossing fragments at that same boundary. The 200 m
edge fade mixed detailed water back to the opaque scene rather than continuing
the ocean. The resident rectangle now bounds source elevation detail only.
Existing water geometry continues over the curved sea, using the existing
16 CDLOD levels, 32x32 grid and 1500-node cap. These budgets are unchanged.

Earth selection now uses conservative spherical vertical bounds for both LOD
distance and frustum tests. Flat +/-25 m bounds would incorrectly cull distant
curved sea below the tangent plane. The default native OFP selection retains
its flat bounds. The Earth material uses the spherical base normal and rejects
the hidden side of the sea at the analytic tangent horizon. No extra fog or
arbitrary enlarged distance is used.

Within the source heightmap, the previous exact coastal triangle proxy remains.
Outside it, the existing 400 m transition to a render-only floor remains; no
seabed is inferred or published. Opaque rendered land wins the existing
reverse-Z `GreaterEqual` hardware depth test; an explicit comparison with the
same opaque scene-depth snapshot rejects a water fragment whenever its reverse
depth is less than the nearer land depth. This also protects terrain outside
the resident near window. It does not establish real elevation for missing
far-source terrain or convert overview imagery into authoritative land. A
remaining dark shape can be opaque land/background and still needs installed
inspection; this source check does not claim every dark pixel is repaired.

The original heightmap, resident collision/contact values, Earth surface
append guard, native OFP water, sky/material files, Rust `lib.rs`/`gfx3d`, and
optional post-processing bridge are unchanged. Earth water still fades from
6000 to 8000 m altitude and retires at 8000 m.

CPU validation:

- `Test-EarthWaterCoverage.ps1` compiles the actual production selector against
  an existing host `glam` library. Eight tests pass: unchanged native selection,
  curvature bounds containing sampled surface points, a flat-bound false-cull
  regression, coverage beyond all four resident edges, the actual Guernsey
  view through 100 km within the fixed cap, reverse-depth land/floor ordering,
  and the analytic horizon sign from a 1000 m camera.
- `Test-EarthWaterShader.ps1` composes all four actual production material,
  wake, breaker and shore modules without a GPU. All pass.
- `git diff --check` passes.

Parent acceptance remains the same Guernsey `Test-EarthStreaming.ps1 -Coast`
capture at local [8000,1000,4800], azimuth 135, elevation -20, after current
paired ground/horizon publication, plus the existing 9000 m retirement capture.
Continuous sea, coastline depth and near/far appearance require those pixels;
the earlier diagnostic receipt is not visual acceptance.
