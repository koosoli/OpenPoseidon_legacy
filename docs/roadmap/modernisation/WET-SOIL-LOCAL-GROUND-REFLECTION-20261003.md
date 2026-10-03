# Local ground reflection for wet footprint edges

Source candidate; installed appearance acceptance remains pending. This follows
the actual Eden edge receipt from
`pinned-box3d-wet-uniform-and-footprint-optics-20261003-191735-542d35` and its
root-exported `weather-oct3-actual-footprint-edge.csv`.

## Measured cause and limits

The production Sky device probe shows clouds affect upward environment rays.
With representative coverage .95, ray Y +.20711 returns approximately
[.38696,.40063,.42651], rather than the clear [.27197,.56641,1.12891]. Negative
directions return blue [.76465,1.01660,1.25488] under either coverage and either
march budget: the atmospheric horizon padding remains while the original
downward cloud ray is clipped by the planet. Increasing cloud steps does not
address this local source mismatch.

The actual paused 17-by-17 contact receipt contains 289 wet points, 39 downward
reflected rays and no standing-water or SSR-eligible points. All 39 downward
rays are front-facing: N.V is approximately .00589 to .14703. A backface-only
guard would leave this measured mechanism. Reconstructed bare native bed varies
by about 15 micrometres across this small receipt. CPU impression slopes are
still distinct from the game's footprint-filtered fragment normal and its broad
6.25-metre stencil; the replay uses an explicitly flat broad-plane control, not
a claim to have read those fragment values.

## Bounded terrain-only correction

Expose the broad normal already computed by `sample_mud_normal` alongside its
unchanged detailed normal. This keeps the same one native normal evaluation,
two mud-gradient evaluations and two sand-gradient evaluations. Vertex and
prepass callers retain the original detailed-normal wrapper. There are no new
height samples, reflection rays, textures or bindings.

For the existing moist-soil coat only, calculate the reflected direction's dot
product with that broad normal. Filter the local sky hemisphere using its
actual pixel derivative outside material branches:

`S = clamp(.5 + .5 * horizon_cosine / max(abs(fwidth(horizon_cosine)), 1e-6), 0, 1)`

Let B be the existing already-lit, absorption-darkened substrate, L the existing
directional sky radiance after its visibility factors, F the unchanged coat
Fresnel and w the unchanged source-admitted wet fraction. Replace the coat's
radiance by `S*L + (1-S)*B`. The resulting colour is
`(1-w*F*S)*B + w*F*S*L`: a convex redistribution of the same radiance inputs.
Fully blocked sky preserves B, available sky preserves the prior result, black
inputs stay black and HDR radiance is not clamped. Direct solar GGX, roughness,
source admission, shelter, snow, hard films, retained water and sea paths remain
unchanged.

B is an approximate nearby diffuse-ground radiance. This is not an SSR hit or a
terrain ray intersection. The tangent-plane horizon misses distant mountains,
buildings and some opposing pit walls, and can conservatively occlude rays near
curved hilltops. It assumes nearby ground resembles the local lit substrate.
Installed near/mid/far checks must verify that this approximation removes cyan
footprint rims without introducing dark or moving horizon seams.

## Validation and root follow-up

Cached-library Rust type-check of the complete focused test body passed. The
actual production composer passed the new probe, complete terrain and complete
rain-water modules. Static regression checks preserve the original normal
arithmetic and height-sample counts and restrict the replacement to one moist
soil callsite. No child device/game/full-engine build was run.

Root should set `WGR_WET_SOIL_EDGE_RECEIPT` to the actual exported CSV and run:

`shaders::ground_wet_tests::reflection_tests::actual_gpu_recorded_edges_remove_unavailable_sky_without_extra_energy`

with ignored tests enabled. The real device replay checks the causal blocked
and available directions, derivative transition, sloped macro hemisphere,
convex energy bounds, HDR preservation and dry/hard/snow/shelter/zero-light
negatives. Its environment radiance and lit-substrate inputs are explicitly
controlled, rather than claims about an installed HDR pixel.

Then root owns full compilation, deployment and the normal installed Eden
contact/bare-soil clear/overcast captures with the existing source, mass,
camera, depth and lifecycle gates. Visual acceptance remains open until those
actual captures are reviewed.
