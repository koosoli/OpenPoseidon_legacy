# Wet soil broad direct-light coat, 2026-10-03

Source-only candidate, requiring root terrain wiring below. No compilation,
device execution, full build or game run was performed while root measured
uncontended flight performance. Proposed tests are unexecuted. Appearance
acceptance remains pending.

## Actual inspection and missing admission observation

Inspected bd590703 campaign
`build/original-weather-player/resolved-soil-sand-core-contacts-20261003-124947-835446/eden`.
The low/top mud views no longer have the broad blue/milk coating. They still
read as matte agricultural soil. Top view retains the actual walked holes;
the low view targets a later contact beside the actor and is not the identical
earlier low-camera footprint crop.

At the low capture, actual liquid rain was0.8067140579223633, actual overcast1,
sun travelY -0.5898340940475464, nightEffect0, solarDrying0 and GMud wetness
0.45231232047081. Actual source at the inspected contact is native Eden,
`eden/pol.paa`, Field/pol/dirt, sourceEligible true, with stored mud offset
-0.0478852316737175m. These statements do not measure shader wet amount or
fragment rain-proof coverage.

The runner cleared `WGR_TERRAIN_PUDDLE_FIXTURE`, so it did not record the actual
TerrainPuddles accumulation/upload. In this no-wind/overcast1 interval its ODE
has the same rates as GMud; clock sampling and rendering admission are still
separate. Root should first enable the existing telemetry, retain actual
source/physical coverage proof, and let real rainfall accumulate longer. No
wetness, gloss, source flag, sun direction or geometry is forced by this
candidate.

## Independently justified direct-light response

The existing soil coat adds `.06 * pow(N.H,8)` to reflected radiance, then
multiplies it by the environment/view Fresnel through the sky-coat mix. Its
normal-incidence direct response is therefore `.06*.02 = .0012` times the
terrain's diffuse-normalized sun input. That is an artistic amplitude, not a
normalized microfacet direct-light BRDF. A hemisphere-integral assertion
about that half-vector power cannot be inferred from a reflection-vector
cosine-power normalization.

The new pure `soil_wet_sun_specular` reuses the accepted Tidewater dielectric
Fresnel/GGX distribution/Smith masking from `rain_water_sun_specular`:

- The wet-soil broad slope floor is alpha0.4 (alpha²0.16), widened by actual
  derivatives of the filtered geometric normal and capped by the existing
  shared alpha²0.5 upper bound. This is a material presentation choice, not
  measured soil roughness.
- Direct dielectric Fresnel uses water IOR1.333 exactly once, at V.H. The
  helper returns BRDF×N.L in inverse steradians, multiplied by physically
  admitted wet fraction and actual solar visibility.
- Terrain `sun_diffuse` is the diffuse-normalized irradiance input. Multiplying
  by PI converts it for the normalized direct BRDF. At normal incidence the
  proposed response is `F0/(4*alpha²)`≈0.03183 of that sun input, versus the
  previous0.0012; this roughly26.5× difference is a *normal-incidence*
  comparison, not a claimed global amplification or an arbitrary multiplier.
- The solar contribution is added after the existing environment-Fresnel
  mix. It must never pass through that Fresnel a second time. The directional
  sky reflection uses the existing2–12% rough environment cap unchanged;
  the sky reflection bake excludes the direct sun disc, so direct light is
  not counted twice.
- Source, dry, snow, physical shelter/coverage, residual hard-film, shadow,
  clouds and actual sky-visibility weights remain actual zero gates. There is
  no authored-grain normal, position/time noise, new texture or environment
  tap. The derivative is computed outside nonuniform fragment branches.

This introduces a broad, energy-bounded actual direct-light sheen when the
existing material is admitted. It cannot establish that the current matte
capture was admitted, that all original map soils now look wet, or that a
direct-light lobe solves missing saturation under every overcast exposure.

## Required root terrain wiring

In `engine/WgpuRenderer/rust/src/terrain/terrain.wgsl`, replace the imported
`soil_wet_sun_lobe` with `soil_wet_sun_specular` in the ground_wet import.
Immediately after the existing unconditional geometric normal sample, add:

```wgsl
let moist_normal_variation = length(fwidth(geometric_n));
```

Keep `let moist_n = geometric_n`, existing source/rain proof, darkening,
reflected directional sky and `soil_wet_fresnel` intact. In the wet-soil
branch, keep the existing half-vector calculation; remove only:

```wgsl
reflected += sun_diffuse * soil_wet_sun_lobe(dot(moist_n, half_vector))
    * (1.0 - shadow) * cloud_sun_shadow(in.world_xz) * local_sky_visibility;
```

Keep the existing environment composition:

```wgsl
rgb = mix(rgb, rgb * (1.0 - fresnel) + reflected * fresnel,
    soil_wet * (1.0 - rain_puddle));
```

Then add the direct contribution separately:

```wgsl
let solar_visibility = (1.0 - shadow) * cloud_sun_shadow(in.world_xz)
    * local_sky_visibility;
let solar_coat = soil_wet_sun_specular(
    clamp(dot(moist_n, view), 0.0, 1.0),
    clamp(dot(moist_n, -frame.sun_dir_world.xyz), 0.0, 1.0),
    clamp(dot(moist_n, half_vector), 0.0, 1.0),
    clamp(dot(view, half_vector), 0.0, 1.0),
    moist_normal_variation, soil_wet * (1.0 - rain_puddle), solar_visibility);
rgb += sun_diffuse * 3.14159265359 * solar_coat;
```

The old pure amplitude helper remains for pre-integration compatibility;
the new source proof rejects its use in the actual terrain solar path.
No terrain geometry, normal sampling, sand, grass, snow or true water draw
needs changing.

## Proposed root checks

CPU actual module/full source proof:
`shaders::ground_wet_tests::wet_soil_sun_coat_composes_and_actual_terrain_uses_one_fresnel_and_filtered_normal`.
This intentionally requires the wiring above before execution.

Required-device actual helper probe:
`shaders::ground_wet_tests::actual_wet_soil_sun_gpu_is_energy_bounded_broad_filtered_and_physically_gated`.
It checks64 view angles, independent F0/normalized-GGX peak, widened-roughness
peak reduction, hemispherical incoming-energy quadrature at512 directions,
dry/hard/snow/shelter/unproven/shadow/backface zeros, mixed admission and
visibility linearity, unchanged rough sky cap and bounded external weights.
No numerical claim about execution is made before root runs it.

Root should capture actual accumulated/uploaded terrain wetness and actual
physical rain proof, then compare the same walked contact at matched low/top
poses after longer real rain, in sun, overcast and shade. Inspect broad sheen
without glitter or a renewed blue blanket, retained dark substrate/holes,
dry/hard/covered controls and distant stability. Appearance remains separate
from a bounded BRDF or valid source counter.
