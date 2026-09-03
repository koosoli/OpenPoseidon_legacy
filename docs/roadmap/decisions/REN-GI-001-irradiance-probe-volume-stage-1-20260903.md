# REN-GI-001 — The irradiance probe volume, stage 1 of the hybrid GI

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Why now:** the owner asked for the lighting system instead of further small optimisations,
with the new lighting switchable. This is the first slice of the hybrid GI outlined in the
roadmap discussion: probe-based indirect light around the camera, no ray-tracing hardware
required, budgeted per frame.

## What it is

A world-anchored grid of **32 × 8 × 32 light probes** (4 m spacing: a 128 × 32 × 128 m box that
follows the camera in whole spacings) holding an **ambient cube** each — irradiance/π for the
six axis directions. Every frame **one eighth of the probes** are re-integrated from 24 rays
(Fibonacci sphere, rotated per update so the hysteresis averages directions over time). A ray
is answered by scene proxies the renderer already had:

| the ray meets | answered by | radiance |
|---|---|---|
| the terrain | the heightfield (the conform bind group's heightmap, 12-step march to 48 m) | ground albedo × (sun × sun visibility × cos + sky irradiance) |
| a roof or canopy above | the interior-sky dome maps (REN-SKY-003's five directions) | wall albedo × sky irradiance from the open side × ½ |
| nothing | the SH-9 sky | sky radiance in that direction |

Sun visibility along a ray is the long-range terrain ceiling mask times the cloud transmittance;
the cascade maps are per-pixel work and too short-ranged to matter at probe scale. Direct sun on
the shaded pixel is untouched: this is the indirect + sky term only.

**Shading**: where the volume covers a point, `shade()` blends the probe irradiance (trilinear
over the six slabs, weighted by n²) over its analytic sky-dome ambient. The probes already saw
the roof and the terrain, so of the three sky occluders only GTAO and the long-range terrain
mask still multiply it; the per-pixel interior-sky AO keeps a tunable share (default 0.5),
because a 4 m grid cannot resolve a small room. Outside the volume, and with the feature off,
the expression is the pre-GI one exactly.

**Cost model**: 1,024 probe updates per frame × 24 rays × (12 heightfield taps + a few dome
taps) ≈ 0.3 M texture taps — a compute pass in the hundreds of microseconds, timed as
`GI: probe update`. State: a 1.5 MB storage buffer and a 32 × 48 × 32 rgba16f texture.

## Switches

`WGR_GI=0` (pre-GI ambient exactly), `WGR_GI_WEIGHT=<0..1>`; the dev panel's **GI** tab exposes
weight, interior AO share, spacing, rays, hysteresis, ground albedo/gain, wall albedo, ray
length. Default ON, pending the owner's picture check.

## Known limits of stage 1 (the next slices)

- **Leaks**: no visibility weighting between probe and pixel, so light crosses thin walls at
  the grid scale; the interior-sky AO share is the stop-gap. Chebyshev visibility per probe
  (DDGI-style) is stage 2.
- **Walls and trees have no colour**: the dome maps give occlusion, not albedo, so bounce off a
  red wall is neutral. A low-resolution colour proxy around the camera is stage 3.
- **Horizontal rays see no objects**: only the terrain and the (upward) dome directions answer;
  a wall beside a probe is invisible to it. Stage 2's proxy fixes this too.
- **Moving the volume** resets the batch integrated that frame and lets the others converge;
  toroidal addressing would keep them.

## Measured (2026-09-03, 3441x1440, VSync pinned off, one run per arm)

| scene | `GI: probe update` | GPU frame off / on | CPU frame off / on | pixels differing >32 levels |
|---|---:|---:|---:|---:|
| Everon `perf_combat` 12:00 | 0.10 ms | 17.5 / 18.9 | 21.6 / 22.8 | 2.6% |
| Everon `perf_town` 12:00 | 0.10 ms | 15.3 / 14.5 | 19.1 / 17.9 | 1.3% |
| Everon `perf_town` 17:00 | 0.15 ms | 18.6 / 18.1 | 17.8 / 18.1 | 1.3% |

The probe update is a tenth of a millisecond; the frame figures move inside their run-to-run
spread. What the picture shows at stage 1: tree crowns and canopies darken their undersides
(the dome maps report them as roofs), sky ambient on open ground is unchanged, and the ground
bounce is there but subtle at the default gain. No colour bleeding yet -- the proxies have no
colour -- which is stage 2's job. The owner has the crops.

## The two traps on the way (see memory: compute reuse of render bind groups)

naga's backend panicked at renderer creation on `select()` over abstract ints and on a local
array of vectors; and the camera/conform bind group layouts had no COMPUTE visibility, which
made the compute pipeline invalid -- at runtime that surfaced as a lost device, not a message.
The pipeline-on-a-real-device test (`gi_probe_pipeline_builds_on_a_real_device`) now catches
both before a build. A third: the sampled probe texture sits in the camera group the compute
pass itself binds, so it writes a twin and copies; and the camera group's eighth sampled
texture pushed water's pipeline layout over the default 16-per-stage limit, which is raised
now (`max_sampled_textures_per_shader_stage`, clamp 16..64).
