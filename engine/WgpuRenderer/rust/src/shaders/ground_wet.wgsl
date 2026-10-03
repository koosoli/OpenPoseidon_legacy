#define_import_path ground_wet
#import rain_water_optics::rain_water_sun_specular

// Exact cultivated Noe tiles omit CfgSurfaces. Source identity is carried by
// bit2; only their unjittered central world-cell region can be soft soil.
fn cultivated_soil_fraction(flags: u32, world_cell_uv: vec2<f32>) -> f32 {
    if ((flags & 4u) == 0u) { return 0.0; }
    let edge = min(min(world_cell_uv.x, world_cell_uv.y),
                   min(1.0 - world_cell_uv.x, 1.0 - world_cell_uv.y));
    return smoothstep(0.12, 0.17, edge);
}

// Retained rainfall wets the entire proven soil surface, not just the random
// puddle patches. Snow and missing physical shelter proof leave it dry.
fn soil_wet_amount(soil: f32, wetness: f32, snow_depth: f32) -> f32 {
    return clamp(soil, 0.0, 1.0) * smoothstep(0.03, 0.65, clamp(wetness, 0.0, 1.0))
        * (1.0 - smoothstep(0.002, 0.02, snow_depth));
}

// x: diffuse substrate multiplier, y: remaining cosmetic smooth-film share.
// Call only after the existing source, snow and physical rain-proof gates.
// The ground's procedural mask has no water depth: on proven soft soil it is
// a moist rough substrate, not another smooth water interface. Actual retained
// pools are drawn separately from their supported depth and keep their optics.
fn soil_wet_surface(soil: f32, wet_amount: f32, film: f32) -> vec2<f32> {
    let wet = clamp(wet_amount, 0.0, 1.0);
    let source = select(0.0, clamp(soil, 0.0, 1.0), wet > 0.0);
    let smooth_film = clamp(film, 0.0, 1.0) * (1.0 - source);
    // Tidewater's accepted damp substrate darkens albedo by 0.58. Reuse that
    // bounded, channel-preserving response for the admitted wet-soil share;
    // never exempt a wet substrate because the procedural film mask is high.
    // Preserve the existing 0.38 film darkening on the remaining hard share.
    let diffuse = mix(1.0, 0.58, wet) * (1.0 - 0.38 * smooth_film);
    return vec2<f32>(diffuse, smooth_film);
}

// Broad, low-energy moist-soil response. Use the geometric normal: authored
// grit cannot turn sunlight into individual sparkles. No time/position noise.
fn soil_wet_fresnel(view_cosine: f32) -> f32 {
    let grazing = 1.0 - clamp(view_cosine, 0.0, 1.0);
    return 0.02 + 0.10 * pow(grazing, 5.0);
}
fn soil_wet_sun_lobe(half_cosine: f32) -> f32 {
    return 0.06 * pow(clamp(half_cosine, 0.0, 1.0), 8.0);
}

// Direct-sun BRDF times N.L, in inverse steradians. The shared Tidewater water
// interface includes dielectric Fresnel exactly once and normalized GGX/Smith
// masking. Rain smooths the moist coat continuously from slope width alpha=.4
// to .22; actual geometric-normal derivatives widen that still-broad minimum.
// No grain/time noise, artificial gloss gain or added unshadowed light.
// Input wet_amount is the caller's source/snow/physical-rain-admitted fraction;
// visibility is its actual sun shadow/cloud/sky visibility, never a gloss knob.
fn soil_wet_sun_specular(ndv: f32, ndl: f32, ndh: f32, vdh: f32,
                         normal_variation: f32, wet_amount: f32, visibility: f32) -> f32 {
    if (wet_amount <= 0.0 || visibility <= 0.0) { return 0.0; }
    let alpha = soil_wet_sun_alpha(wet_amount);
    // Remove the shared helper's .0064 floor before adding the real normal
    // variance, so its final GGX alpha² is alpha² + derivative variance.
    let variance = sqrt(alpha * alpha - 0.0064
        + max(normal_variation, 0.0) * max(normal_variation, 0.0));
    return rain_water_sun_specular(ndv, ndl, ndh, vdh, variance)
        * clamp(wet_amount, 0.0, 1.0) * clamp(visibility, 0.0, 1.0);
}

fn soil_wet_sun_alpha(wet_amount: f32) -> f32 {
    return mix(0.4, 0.22, clamp(wet_amount, 0.0, 1.0));
}
