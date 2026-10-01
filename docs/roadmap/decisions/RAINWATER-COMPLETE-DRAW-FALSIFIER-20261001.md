# Rainwater full vertex/fragment draw falsifier

## Installed evidence and bounded hypothesis

The five-arm installed campaign at
`build/wet-ground-ablation/soil-water-original-daytime-noe-20261001-023651-1aeaae`
used the matched 50db754c executable/DLL. Functional rain accumulation and local
sampling passed, but the PhysicalSky water view appears to expose bare sand.
Its after-rain local sample is valid: depth 0.3292004 m, surface 24.0599804 m,
native terrain 23.7307796 m. Actual rain is then zero, which should stop droplets
without deleting retained water or its depth-dependent optics.

The oblique camera is derived from that surface: X2800, Z5151, Y33.05998,
bearing0, elevation-20.55. It is above the pool, looking forward and down toward
the target at Z5175. Thus an underwater/underside view is not an adequate
explanation for the expected pool disappearing. The available field sample does
not by itself certify every rendered coarse cell's support or rain cover.

The candidate raster fault is concrete: `RainWater::new` uses the default Ccw
front-face convention, `cull_mode=None`, and the actual `fs_main` discards
`!front`. Disabling fixed-function culling does not suppress that shader discard.
The engine uses left-handed forward projection and its rigid pipelines explicitly
select clockwise fronts. Rainwater's generated upward 00/01/10 triangle can
therefore be screen-clockwise from this ordinary above-ground camera. This is a
source hypothesis pending the actual GPU draw falsifier, not a proven installed
failure or a reason to remove the underside guard.

## Test ownership and scope

Root ran the required-device baseline at e8a59cb7. Both production positives
painted zero pixels, while the identical clockwise diagnostic painted320 pixels
with RGB energy14890. The covered, stale-support, OFF and snow negatives were
all zero. This confirms the raster convention fault rather than missing uploads.

Changing only the production front face to Cw makes the flat positive paint320
pixels/14890 energy and triangular sloped positive337 pixels/15482 energy. All
four negatives remain zero. Logs: build/rainwater-full-draw-baseline.log and
build/rainwater-full-draw-fixed.log. The underside shader guard is retained.
Installed appearance, reflections and ripple quality still require new captures.

The isolated candidate starts at 2ce068d6, including the private far-weather
Frame lane and binding20. Only the test section of `rain_water.rs` and this
record change; producer, upload API and production WGSL/pipeline code are intact.

`rain_water::full_draw_tests::actual_gpu_complete_pool_draw_visibility_and_admission`
requires a real adapter/device and the production five bind groups. It composes
the real Frame, Conform, ground-puddle/rain and reflection modules, discovers
actual Frame offsets/span from Naga, and invokes the actual `RainWater::new`,
`set_grid`, `prepare`, `draw`, `vs_main` and `fs_main`. Real native-height texels,
simulation RGBA grid, comparison-depth weather map, filtered directional sky,
depth attachment, straight-alpha blending and pixel readback participate.

It records pixel counts/RGB energy for:

- flat 30 cm retained water over matching native ground;
- triangular sloped/saddle bed with the exact matching native anti-diagonal;
- an actually closed physical zenith depth map, with artistic AO disabled;
- a one-metre stale native-bed mismatch;
- explicit master-off upload;
- actual snow deposition hiding the local pool;
- a diagnostic flat control changing only the raster front face to clockwise.

The two production positive cases must paint substantial pixels; the four
negative cases must paint none. The clockwise control must paint, separating
front-facing failure from resource/projection/admission fixture failure. Counts
for all cases are printed before assertions, so a baseline all-zero production
draw and a nonzero clockwise control provide actionable evidence. That test-only
pipeline is not a production fix and does not replace the real shader.

The framebuffer is transparent black so any blended physical-water RGB/alpha
is directly measurable. This tests submission and material visibility, not final
visual quality over a fully rendered landscape or the accepted ocean path.

## Verification and root handoff

The new Rust test module parsed/formatted successfully with rustfmt; whitespace
checks pass. No local crate build, GPU execution, game launch or deployment was
performed. Root owns execution under the existing serial game/GPU lock:

```text
cargo test rain_water::full_draw_tests::actual_gpu_complete_pool_draw_visibility_and_admission -- --exact --nocapture
```

Run the falsifier before correcting front-face state, retain the printed counts,
then rerun after the bounded production correction if supported. An installed
fixed-pair PhysicalSky capture remains mandatory; source or synthetic GPU pixel
acceptance cannot establish the installed coarse-grid cover/support footprint
or final appearance. Current acceptance is **authored falsifier; runtime pending**.
