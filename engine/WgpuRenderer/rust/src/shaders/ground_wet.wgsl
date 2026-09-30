#define_import_path ground_wet

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

// Broad, low-energy moist-soil response. Use the geometric normal: authored
// grit cannot turn sunlight into individual sparkles. No time/position noise.
fn soil_wet_fresnel(view_cosine: f32) -> f32 {
    let grazing = 1.0 - clamp(view_cosine, 0.0, 1.0);
    return 0.02 + 0.10 * pow(grazing, 5.0);
}
fn soil_wet_sun_lobe(half_cosine: f32) -> f32 {
    return 0.06 * pow(clamp(half_cosine, 0.0, 1.0), 8.0);
}
