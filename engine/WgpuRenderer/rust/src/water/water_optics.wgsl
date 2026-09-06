#define_import_path water_optics

// WRL-002 — the ONE optical model for water, shared by the surface shader (water.wgsl) and
// the underwater compositor (underwater.wgsl). Before this module each side derived its own
// extinction from the same sliders, three different ways: the surface used a hard-coded
// (0.280, 0.065, 0.020) red-absorbing curve for the seabed transmittance AND a separate
// scalar "seabed visibility" falloff, while the compositor derived a hue from the authored
// deep colour. Looking down at 5 m of water and swimming in it therefore showed two
// different liquids, and no slider moved both.
//
// Units: every coefficient is in inverse metres and non-negative.
//   sigma_a  RGB absorption      — removes light, colours the water (the deep-colour hue)
//   sigma_s  scalar scattering   — removes light from the transmitted image and re-emits it
//                                  as the body's own glow; grey, so it does not tint
//   sigma_t  = sigma_a + sigma_s, the extinction the transmitted image decays with:
//   T(path)  = exp(-sigma_t * path)
//
// The in-scattered radiance along a path that starts at zero and saturates is
//   L_body = albedo * E_available / PI * (1 - T)
// which is the single-scatter closed form for a homogeneous slab: no light at zero path,
// and a fully saturated body colour once the path is a few mean free paths long. `albedo`
// is the authored shallow->deep swatch pair: authored control over WHAT colour the
// scattered light has, physics deciding HOW MUCH of it there is.
//
// Compatibility mapping of the existing Water tab controls (nothing saved is reinterpreted
// silently; the mapping is written down here and in the tab tooltips):
//   Colour clarity  (color_ext)  -> sigma_a magnitude = max(color_ext * 2.5, 0.12) (the old
//                                   surface Beer-Lambert scale) and sigma_s = color_ext * 1.6
//                                   (the old "seabed visibility" falloff, which WAS a
//                                   scattering term without being named one)
//   Deep colour                  -> sigma_a hue (mean-normalised -log of the linear swatch)
//   Underwater density           -> multiplies sigma_t for the compositor only
//   Underwater colour bias       -> mixes sigma_a's hue toward the retired neutral curve
//   Opacity                      -> lifts T toward 1 (more of the background), physical
//                                   composite only; it used to be a framebuffer alpha that
//                                   blended the background in a SECOND time

const NEUTRAL_ABSORPTION_HUE: vec3<f32> = vec3<f32>(0.280, 0.065, 0.020);
// Mean of NEUTRAL_ABSORPTION_HUE; a swatch-derived hue is normalised to the same mean so
// swapping hue for the neutral curve changes balance, never density.
const NEUTRAL_ABSORPTION_MEAN: f32 = 0.1216;
const OPTICS_PI: f32 = 3.14159265359;

fn optics_srgb_to_linear(c: vec3<f32>) -> vec3<f32> {
    let lo = c / 12.92;
    let hi = pow((c + vec3<f32>(0.055)) / 1.055, vec3<f32>(2.4));
    return select(hi, lo, c <= vec3<f32>(0.04045));
}

// Per-channel absorption hue from an authored deep swatch (gamma space, as the Water tab
// stores it). -log(colour) is the absorption that would produce that colour over unit
// depth; the darkest channel absorbs most, which is what makes deep water blue. The result
// has mean NEUTRAL_ABSORPTION_MEAN whatever the swatch's brightness.
fn water_absorption_hue(deep_swatch_gamma: vec3<f32>) -> vec3<f32> {
    let deep_lin = optics_srgb_to_linear(clamp(deep_swatch_gamma, vec3<f32>(1e-4), vec3<f32>(1.0)));
    let absorb = -log(deep_lin);
    let mean_absorb = max((absorb.r + absorb.g + absorb.b) / 3.0, 1e-4);
    return absorb * (NEUTRAL_ABSORPTION_MEAN / mean_absorb);
}

// RGB absorption coefficient (1/m). `hue_bias` 1 = the swatch hue, 0 = the neutral curve.
fn water_absorption(deep_swatch_gamma: vec3<f32>, clarity: f32, hue_bias: f32) -> vec3<f32> {
    let hue = mix(NEUTRAL_ABSORPTION_HUE, water_absorption_hue(deep_swatch_gamma),
        clamp(hue_bias, 0.0, 1.0));
    return hue * max(clarity * 2.5, 0.12);
}

// Scalar scattering coefficient (1/m). Grey: scattering in water is nearly wavelength
// independent, and it is what makes the seabed disappear long before absorption would.
fn water_scattering(clarity: f32) -> f32 {
    return max(clarity, 0.0) * 1.6;
}

// Total extinction (1/m) the transmitted image decays with.
fn water_extinction(deep_swatch_gamma: vec3<f32>, clarity: f32, hue_bias: f32) -> vec3<f32> {
    return water_absorption(deep_swatch_gamma, clarity, hue_bias) + vec3<f32>(water_scattering(clarity));
}

// Transmittance over `path_m` metres of water. Zero path is exactly 1; a negative path
// (a dry ray) is treated as zero rather than amplifying.
fn water_transmittance(sigma_t: vec3<f32>, path_m: f32) -> vec3<f32> {
    return exp(-max(sigma_t, vec3<f32>(0.0)) * max(path_m, 0.0));
}

// Single-scatter body radiance for a path with transmittance `transmittance`, lit by the
// irradiance `light` (already including any shadowing and the sun's cosine term).
// `albedo` is the scattering albedo the artist authored. Lambertian 1/PI, the same
// normalisation the foam material in water.wgsl uses.
fn water_inscatter(albedo: vec3<f32>, light: vec3<f32>, transmittance: vec3<f32>) -> vec3<f32> {
    return albedo * light / OPTICS_PI * (vec3<f32>(1.0) - clamp(transmittance, vec3<f32>(0.0), vec3<f32>(1.0)));
}

// WRL-002 — whitecap coverage versus wind. Whitecaps are essentially absent below about
// 4 m/s (Beaufort 3 is "scattered whitecaps" at 5.5 m/s) and grow steeply with wind
// (Monahan's coverage law goes as roughly U^3.4). One gate, applied at every whitecap
// source: the persistent-foam injection, the crest whitecap term on the surface, and the
// spray emitter. Before this, foam and spray were driven by crest geometry alone, so a
// 5 m/s sea carried a lace of whitecaps and floating spray sprites. Anchored so the
// 12 m/s reference sea is untouched (factor 1.0).
fn whitecap_wind_factor(wind_speed: f32) -> f32 {
    let s = smoothstep(3.5, 12.0, max(wind_speed, 0.0));
    return s * s;
}

// How far along a path the authored colour drifts from the short-path (shallow) swatch to
// the long-path (deep) one. Uses the mean extinction so a swatch that is pure blue does
// not drift at a different rate per channel.
fn water_albedo_along_path(shallow: vec3<f32>, deep: vec3<f32>, sigma_t: vec3<f32>, path_m: f32) -> vec3<f32> {
    let mean_ext = (sigma_t.r + sigma_t.g + sigma_t.b) / 3.0;
    let drift = 1.0 - exp(-max(mean_ext, 0.0) * max(path_m, 0.0));
    return mix(shallow, deep, drift);
}
