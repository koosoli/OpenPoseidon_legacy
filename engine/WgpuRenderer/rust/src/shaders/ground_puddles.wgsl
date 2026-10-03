#define_import_path ground_puddles
#import rain_water_optics::rain_water_impact_normal

// Cosmetic standing-water film on actual ground. No source-language whitelist,
// camera seed, texture allocation or geometry/collision change. Callers supply
// absolute world coordinates and a derivative footprint from uniform control.
fn puddle_hash(p: vec2<f32>) -> f32 {
    let q = vec2<u32>(vec2<i32>(p));
    var h = q.x * 1597334677u ^ q.y * 3812015801u;
    h = (h ^ (h >> 16u)) * 2246822519u;
    return f32(h ^ (h >> 13u)) / 4294967295.0;
}

fn puddle_noise(p: vec2<f32>) -> f32 {
    let b = floor(p);
    let f = fract(p);
    let w = f * f * (3.0 - 2.0 * f);
    return mix(mix(puddle_hash(b), puddle_hash(b + vec2<f32>(1.0, 0.0)), w.x),
               mix(puddle_hash(b + vec2<f32>(0.0, 1.0)), puddle_hash(b + vec2<f32>(1.0)), w.x), w.y);
}

fn ground_puddle_mask(world_xz: vec2<f32>, footprint: f32, wetness: f32) -> f32 {
    let wet = clamp(wetness, 0.0, 1.0);
    let field = puddle_noise(world_xz / 6.0) * 0.8 + puddle_noise(world_xz / 2.2) * 0.2;
    let threshold = mix(0.82, 0.64, wet);
    let edge = max(0.035, max(footprint, 0.0) / 6.0 * 0.5);
    // Wetness expands connected patches. Fade before the finest field becomes
    // subpixel. Preserve the existing world mask, including accepted mud sites.
    return smoothstep(threshold - edge, threshold + edge, field)
        * smoothstep(0.18, 0.6, wet) * (1.0 - smoothstep(0.4, 0.9, footprint));
}

fn ground_puddle_land_factor(normal_up: f32, height: f32, sea_level: f32, snow_depth: f32) -> f32 {
    // Standing water is near-level. Only actual submerged/coastal water is
    // excluded: the previous four-metre coastline veto hid dry lowland ground.
    return smoothstep(0.995, 0.9995, normal_up)
        * smoothstep(sea_level + 0.15, sea_level + 0.5, height)
        * (1.0 - smoothstep(0.002, 0.02, snow_depth));
}

fn ground_puddle_rain_factor(reach: f32, coverage: f32) -> f32 {
    if (coverage <= 0.0) { return 0.0; }
    // Undo the lighting map's open border fallback before admitting liquid rain.
    let raw_reach = clamp((reach - (1.0 - coverage)) / max(coverage, 1e-5), 0.0, 1.0);
    return coverage * smoothstep(0.6, 0.95, raw_reach);
}

fn ground_puddle_ripple_normal(world_xz: vec2<f32>, time: f32, rain: f32, footprint: f32) -> vec3<f32> {
    // Ground film and retained water share world-fixed stochastic rain events.
    // Each caller retains its existing physical/source/wetness admission.
    let impact = rain_water_impact_normal(world_xz, time, rain, footprint);
    var slope = impact.xz / max(impact.y, 0.001);
    // Thin films keep the old single-packet maximum tilt even when several
    // independent impacts overlap. Retained pools may show their fuller motion.
    slope *= min(1.0, 0.135 / max(length(slope), 0.001));
    return normalize(vec3<f32>(slope.x, 1.0, slope.y));
}
