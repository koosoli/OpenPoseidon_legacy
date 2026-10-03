# Rough environment response for admitted moist soil

Source candidate following installed `afffcc53` campaign
`porous-cloth-local-ground-mud-20261003-200703-05a75d`. Actual overcast low/top,
clear low, contact coat/water diagnostic and prior `542d35` images were viewed.
The local horizon guard reduces cyan footprint rims, but flat ground remains a
grey sheet at grazing angles while the top view retains dark brown substrate.
The coat diagnostic has zero smooth-film share on the pure source; the broad
region's water diagnostic is neutral without a visible retained-water overlay.
This supports improving the moist-soil environment coat, rather than changing
water depth, tint, mass, source eligibility or the existing direct sunlight.

## Actual mismatch

Direct sunlight uses a broad GGX/Smith coat with slope alpha .4 to .22 and real
geometric derivative variance. Environment reflection uses one mirror direction
at LOD0, independently of that roughness. The shared 256-by-128 sky environment
has one mip. Asking for a different texture LOD cannot supply the missing
rough-surface angular integration. Direct coat Fresnel/roughness and environment
reflection therefore describe different angular surfaces.

## Bounded quadrature

The pure `soil_wet_environment_samples` helper returns eight reflected
directions and normalized weights. It samples the GGX slope radial CDF
`r = alpha * sqrt(u/(1-u))` at four stratified radial midpoints with antipodal
azimuth pairs. The normal-space basis is continuous on the upper terrain
hemisphere; there is no time, position noise or camera-axis rotation.

The effective alpha squared is the existing direct-coat slope width squared
plus the same nonnegative geometric-normal derivative variance, with the same
.0064/.5 bounds. Half-vector NDF sampling has reflected-direction PDF
`D*(N.H)/(4*(V.H))`. Dividing `BRDF*(N.L)` by that PDF, with Fresnel factored out,
gives the nonnegative weight `4*(N.L)*V_GGX*(V.H)/(N.H)`. Correlated GGX/Smith
visibility is the same expression used by the direct helper. Back-facing
half-vectors and unavailable local incident directions have zero weight.

The accepted weights are normalized to one. This deliberately preserves the
existing coat's Fresnel amplitude and diffuse energy split: it filters the
incoming radiance kernel rather than silently replacing the environment BRDF
amplitude with a different Fresnel/white-furnace model. It is a sparse quadrature
approximation, not an exact environment BRDF integral or a prefiltered map.

Every sampled direction independently uses the existing broad-plane sky
visibility and already-lit local-ground fallback. A conservative cone horizon
width is derived from existing broad/fine/view screen derivatives in uniform
flow before material/source branches; there are no derivatives inside the
wet-dependent sample loop. No extra height taps are introduced. The existing
source, snow, shelter, wet amount, substrate darkening, direct sun, hard film,
retained-water and sea paths remain intact.

The only new production sampling cost is at most eight environment reads on
admitted sky-lit moist soil, replacing its one read. Root must assess actual GPU
cost as well as appearance before calling this accepted. Sparse fixed quadrature
can retain directional bias or miss small environment features; the pre-existing
local plane and ground-radiance approximation still do not see distant geometry.

## Checks and remaining acceptance

Cached-library Rust type-check passed for the complete focused tests. Five
distinct CPU composition/source checks passed, including the actual complete
terrain/rain-water modules and both old and new probe paths. The old detail-noise
regression now permits trigonometry only for the fixed quadrature angle while
still refusing position/time noise, hash/fract, resources and altered admission.
No child device, engine build, deployment or game was run.

Root device filter:

`shaders::ground_wet_tests::rough_env_tests::actual_gpu_rough_soil_environment_is_filtered_convex_and_locally_visible`

The 192 cases cover two flat/sloped normals, four view elevations, four azimuths,
three wet-dependent alpha values and two derivative widths. They test unit rays,
normalized nonnegative weights, backface refusal, per-direction local ground
occlusion, clear/cloudy sharp angular feature filtering, convex radiance bounds,
zero light and unclamped HDR. Matched normal-view controls require actual alpha
and derivative changes to widen the lobe and filter the same sharp feature.
Controlled radiance functions test the production kernel; they are not claims
about an installed pixel or the complete sky state. Existing source/dry/snow/
shelter tests and actual recorded-edge replay remain required.

Root then owns full build, installed contact/bare clear/overcast low/top captures
and GPU-cost assessment. The remaining grey-sheet appearance is still open until
those actual comparisons establish whether this first angular correction helps.
