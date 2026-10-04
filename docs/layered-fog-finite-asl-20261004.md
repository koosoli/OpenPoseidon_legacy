# Finite ASL fog: opt-in candidate, 4 October 2026

Two independent, finite world-altitude density layers now transport scene-linear
radiance with `L + background*T`, with `T=exp(-integrated extinction)`. Density
has linear edge feathers and its height primitive integrates thin slabs exactly;
the renderer does not rely on a march midpoint landing inside a thin layer.
Overlapping layers add extinction. Extinction is per metre, scattering albedo is
bounded to `[0,1]`, and the Henyey–Greenstein phase is normalized per steradian.
Actual captured landscape Fog in `[0,1]` scales density. It is not an independent
forced weather state or terrain-following noise layer.

The pre-existing distance aerial perspective, atmosphere and ground/weather
shaders are preserved. This is separate single-scattering finite-medium transport
composed after that closure, not a full coupled atmospheric multiple-scattering
solver. The default remains **OFF**. Tactical visibility and culling ranges are
unchanged by this control; actual weather Fog still controls their existing rules.

## Configuration and safeguards

In the Sky panel, expand **Layered low fog (experimental)**, enable **Physical ASL
fog layers**, and set each ASL base/top, boundary feather and extinction. HDR,
procedural sky, sky-based lighting and nonzero actual weather Fog are required.
The built-in profiles are `[0,35,5,.008]` and `[70,100,5,.003]` metres/metre inverse;
albedo `.95`, HG `g=.6`. Profiles may therefore be below a high hill: that is
expected for fixed ASL layers. They are not automatically shifted to the camera.

Exact environment override `WGR_LAYERED_FOG=1` enables the candidate; any other
present value disables it. Optional `WGR_FOG_LAYER0` / `WGR_FOG_LAYER1` contain
four comma-separated finite values `base,top,feather,extinction`. Invalid intervals
or values refuse the effect. UI settings are session controls, not new persistent
configuration entries. `WGR_LAYERED_FOG_TRACE=1` emits bounded CPU encoding records.

No volume is encoded with missing native terrain, missing current near physical
weather publication, a non-HDR/legacy-lighting camera, an underground camera or
layers overlapping the cloud deck. Each segment requires native-above-ground and
current physically open start/mid/end samples. Weather-map overflow reads the
actual retained COUNT/EMIT buffers; a pending near map never borrows a far result
at the same position. Far coverage is selected only outside the planned near
footprint. Beyond both physical maps, additional fog is conservatively omitted.

Layer lighting uses the actual scene directional-light direction and radiance,
terrain shadow ceiling, CSM and cloud-transmittance resources, plus a bounded
six-point upper-hemisphere environment quadrature. It does not use the artificial
negative-Y sky floor as illumination. Self-attenuation toward the directional
light uses the same analytic profile. Lighting is sampled twice per volume slice;
small casters/cloud details and diffuse multiple scattering remain approximations.

Shared surface application covers the existing `apply_fog` consumers: terrain,
direct/retained objects, grass, water and physical puddles. It uses each fragment's
own distance, exact view/projection/camera match and physical endpoint coverage.
Legacy effects using a separate fog-factor closure are not promoted to this path.
An indoor endpoint is conservatively unchanged even when its external viewing ray
crosses outdoor fog; tree canopy can also suppress fog through roof coverage.
Neither case is claimed as a complete interior volumetric solution.

The sky receives a background-only L/T pass before geometry. Higher cloud radiance
is then transported through the certified lower layers exactly once:
`cloudL*fogT + fogL*(1-cloudT)`, with the existing destination cloud transmittance.
Planar cloud composition explicitly disables this correction. Cockpit, monitor,
reflection and other unmatched cameras refuse the main volume. No depth overlay
is applied to transparent objects or UI.

## Resource, ABI and cost contract

The transport allocation is a fixed `32³ RGBA16Float` volume (256 KiB). OFF does
not dispatch it, draw the background pass or newly request weather maps. The
consumer is invalidated on transition from a ready frame; no readback is added to
normal frames. ON may newly require existing physical near/far weather maps and
their cull/draw work, in addition to 1,024 columns × 64 bounded segment samples.
Its compute cost has separate append-only timer index 88; timer count is 89.
Whole-frame GPU/CPU measurements are still needed, including the incremental
weather-map cost. Default promotion requires installed visual and cost evidence.

Paired ABI is **21**. `WgrSkyRuntime` grows 112→128 bytes for captured weather and
readiness; `WgrRenderParams` grows 636→684 bytes. `WgrLayeredFog` is 48 bytes at
render-params offset 636. Its standalone size is appended at the **same final
position** of both layout-hash lists. The shared frame UBO size and existing
weather/source/physics structs are unchanged. Both binaries must be rebuilt and
deployed together by the root runtime owner.

## Verification and installed runner

Source commit `7e66d0e5` passed cached whole-library Rust type checking and full
actual Naga composition for the fog compute, background, cloud composite,
terrain, direct/retained object and grass shaders. Focused device batch passed:

- `layered_fog::tests::actual_fog_profile_gpu_integrates_thin_slabs_phase_energy_and_camera_refusals`
- `layered_fog::tests::actual_fog_volume_gpu_obeys_beer_lambert_native_roof_overflow_and_weather_gates`
- `sky::sky_device_tests::sky_pipelines_build_on_a_real_device`
- `rain_water::full_draw_tests::actual_gpu_complete_pool_draw_visibility_and_admission`

The actual volume fixture verifies Beer–Lambert per slice, monotonic extinction,
cloud-zero direct-light refusal without changing extinction, physical-roof and
native-underground vacuum, retained overflow refusal, missing generation/coverage,
cloud-height refusal, weather-zero and OFF. The pure production-helper GPU fixture
includes thin upward/downward slabs, segmentation invariance, phase energy, HDR
transport, vacuum identity and mismatched/disabled camera controls. The existing
water fixture retains all coarse/fine positive and roof/snow/sea/support negatives.
Headless synthetic coverage is not installed visual acceptance.

The root owner should use the normal installed lock and provenance workflow:

```powershell
scripts/Test-LayeredFog.ps1 -ExpectedCommit <installed-commit> -Label original-everon-layered
```

The runner executes four normally exiting stock Everon arms: OFF/ON at actual
Fog `.45`, and ON/OFF at actual Fog zero. It records native source and height,
weather/ranges, pair hashes, bounded actual encoding trace and ground/between/above
camera captures. Profiles for ON are frozen ASL slabs selected once from the
actual OFF baseline height; the OFF scout's disabled profiles do not affect its
output. Cameras are render-only and scene simulation is paused after real weather
settles. No forced texture, snow, wetness, fog colour or exposure is used.

The runner refuses source/weather/camera mismatches and an ON capture without
current encoding readiness. Its trace proves CPU command admission, not GPU
completion, exact screenshot-frame identity or aesthetic quality. Root must judge
the images, check interior/cave/canopy boundaries and obtain a quiet cost comparison
before enabling by default. Source-only shader check without a device:

```powershell
scripts/Test-LayeredFogShader.ps1 -DependencyDirectory <matching-cached-deps> -RustCompiler rustc
```
