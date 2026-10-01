#define_import_path rain_water_optics

// Metres of ACTUAL supported local water depth. This is optical roughness,
// never a simulation/admission cutoff: a millimetre film still transmits the
// already-lit terrain and has its Beer-Lambert body. Below 8mm the substrate's
// grit/relief controls the wet response; by 50mm it is a standing-water mirror.
fn rain_water_standing_fraction(local_depth: f32) -> f32 {
    return smoothstep(0.008, 0.050, max(local_depth, 0.0));
}

fn rain_water_surface_optics(local_depth: f32, view_cosine: f32, reflected: vec3<f32>,
                             body: vec3<f32>, coverage: f32) -> vec4<f32> {
    if (local_depth <= 0.0 || coverage <= 0.0) { return vec4<f32>(0.0); }
    let mu = clamp(view_cosine, 0.0, 1.0);
    // The bed has already received its broad rough wet-soil/puddle shading.
    // Do not paint a second smooth interface over a rough millimetre film.
    // Continuous depth handoff replaces that response with pooled reflection.
    let fresnel = rain_water_standing_fraction(local_depth)
        * (0.025 + 0.975 * pow(1.0 - mu, 5.0));
    let transmission = exp(-3.0 * min(local_depth, 64.0));
    let scatter_weight = (1.0 - fresnel) * (1.0 - transmission);
    let opacity = fresnel + scatter_weight;
    if (opacity <= 0.0) { return vec4<f32>(0.0); }
    // Straight alpha: reflection + body + transmitted destination sum to one.
    // No global grade, HDR clamp, alpha floor or instantaneous-rain dependence.
    let colour = (max(reflected, vec3<f32>(0.0)) * fresnel
        + max(body, vec3<f32>(0.0)) * scatter_weight) / opacity;
    return vec4<f32>(colour, opacity * clamp(coverage, 0.0, 1.0));
}
