#define_import_path snow_material

// Snow is a diffuse powder. Filter every procedural band by the WORLD pixel
// footprint; no time/camera seed, specular grains or unresolved sparkle dots.
fn snow_band(footprint: f32, wavelength: f32) -> f32 {
    return 1.0 - smoothstep(wavelength * 0.18, wavelength * 0.5, footprint);
}

fn snow_powder_albedo(world: vec3<f32>, footprint: f32, compacted: f32) -> vec3<f32> {
#ifdef DISABLE_SNOW_POWDER_DETAIL
    return vec3<f32>(0.70, 0.75, 0.80) * (1.0 - clamp(compacted, 0.0, 1.0) * 0.28);
#else
    let p = world.xz + world.y * vec2<f32>(0.17, -0.11);
    let broad = sin(dot(p, vec2<f32>(1.13, 1.67))) * snow_band(footprint, 3.0);
    let powder = sin(dot(p, vec2<f32>(7.31, -6.17))) * snow_band(footprint, 0.6);
    let fine = sin(dot(p, vec2<f32>(37.0, 23.0))) * snow_band(footprint, 0.12);
    // Fresh powder retains the existing palette; directional sky supplies blue
    // shadows. Compacted trails retain the established 28% darkening.
    return vec3<f32>(0.70, 0.75, 0.80)
        * (1.0 + broad * 0.018 + powder * 0.012 + fine * 0.008)
        * (1.0 - clamp(compacted, 0.0, 1.0) * 0.28);
#endif
}

fn snow_powder_normal(world: vec3<f32>, base: vec3<f32>, footprint: f32) -> vec3<f32> {
#ifdef DISABLE_SNOW_POWDER_DETAIL
    return base;
#else
    let p = world.xz + world.y * vec2<f32>(0.17, -0.11);
    let broad = cos(dot(p, vec2<f32>(1.13, 1.67))) * snow_band(footprint, 3.0);
    let powder = cos(dot(p, vec2<f32>(7.31, -6.17))) * snow_band(footprint, 0.6);
    let fine = cos(dot(p, vec2<f32>(37.0, 23.0))) * snow_band(footprint, 0.12);
    let perturb = vec3<f32>(broad * 0.014 + powder * 0.025 + fine * 0.014,
        0.0, broad * 0.020 - powder * 0.021 + fine * 0.009);
    let tangent = perturb - base * dot(perturb, base);
    return normalize(base + tangent);
#endif
}

fn snow_surface_coverage(depth: f32, normal_up: f32, rain_reach: f32) -> f32 {
    return smoothstep(0.002, 0.04, depth)
        * smoothstep(0.35, 0.7, normal_up)
        * smoothstep(0.6, 0.95, rain_reach);
}
