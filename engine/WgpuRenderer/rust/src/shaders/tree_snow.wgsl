#define_import_path tree_snow
#import frame::{frame, interior_rain_coverage, interior_rain_reach, weather_map_coverage, weather_map_reach}
#import snow_material::{snow_surface_coverage, snow_noise, snow_band}

// Smooth broad crown envelope in AUTHORED model height, independent of wind,
// selected LOD, camera and leaf-card normals. CPU proof restricts this to leaf
// cutouts of actual individual static tree owners. Noncutout bark stays dry.
fn tree_snow_height(model_y: f32, min_y: f32, inverse_height: f32, proof: f32) -> f32 {
    if (proof <= 0.5 || inverse_height <= 0.0) { return 0.0; }
    return smoothstep(0.45, 0.85, clamp((model_y - min_y) * inverse_height, 0.0, 1.0));
}

// Input/output are LINEAR reflectance. Constant white erased the photographed
// branch/leaf recesses and exposed crossed-card wedges at complete coverage.
// Keep the already mip-filtered authored luminance in a neutral frost layer;
// dark recesses retain their depth rather than becoming a solid white polygon.
// This is material detail, not colour-based receiver admission or alpha editing.
fn tree_snow_albedo(authored: vec3<f32>, world: vec3<f32>, footprint: f32) -> vec3<f32> {
    let source = clamp(authored, vec3<f32>(0.0), vec3<f32>(1.0));
    let lum = dot(source, vec3<f32>(0.2126, 0.7152, 0.0722));
    let relief = 0.35 + 0.65 * lum / (lum + 0.10);
    let recess = smoothstep(0.008, 0.060, lum);
    // Only metre-scale diffuse clumps: no crystalline points, powder normals or
    // time seed on moving/subpixel leaf cards. UV/albedo detail moves with leaves.
    var mottling = 0.0;
    if (footprint < 0.9) {
        let p = world.xz + world.y * vec2<f32>(0.17, -0.11);
        mottling = snow_noise(p / 1.8 + vec2<f32>(-31.2, 14.7)).x * 0.025 * snow_band(footprint, 1.8);
    }
    let frost = vec3<f32>(0.78, 0.80, 0.82) * relief * (1.0 + mottling);
    return mix(source, frost, recess);
}

fn tree_snow_cover_depth(crown_height: f32, depth: f32, exposure: f32) -> f32 {
#ifdef DISABLE_TREE_SNOW
    return 0.0;
#else
    if (crown_height <= 0.0 || depth <= 0.002 || exposure <= 0.0) { return 0.0; }
    // Card normals describe modeled leaves, not the upper canopy envelope.
    // Keep them intact for lighting, alpha/depth and wind; do not create noisy
    // powder normals or view-facing coat masks on these subpixel cards.
    return clamp(crown_height, 0.0, 1.0) * snow_surface_coverage(depth, 1.0, exposure);
#endif
}

fn tree_snow_coverage(world_pos: vec3<f32>, crown_height: f32, cached_exposure: f32) -> f32 {
    if (crown_height <= 0.0 || (frame.snow_surface.x <= 0.0 && frame.snow_surface.w <= 0.0)) { return 0.0; }
    let world_abs = world_pos + frame.cam_pos.xyz;
    var depth = frame.snow_surface.x;
    if (frame.snow_surface.y >= 0.0 && frame.snow_surface.z > 0.0) {
        depth = max(depth, smoothstep(frame.snow_surface.y,
            frame.snow_surface.y + frame.snow_surface.z, world_abs.y) * frame.snow_surface.w);
    }
    // Real upper-crown external roof ray supplies the far proof. The active
    // zenith map has local precedence, undoing its cosmetic open border.
    let validity = interior_rain_coverage(world_abs);
    var exposed = clamp(cached_exposure, 0.0, 1.0);
    if (validity > 0.0) {
        exposed = clamp((interior_rain_reach(world_abs) - (1.0 - validity)) / validity, 0.0, 1.0);
    }
    return tree_snow_cover_depth(crown_height, depth, exposed);
}

// Exact stock far forest atlases only. These masks describe inspected authored
// upper foliage regions, not an inferred per-tree component or cluster height.
// Exposed lower trunks remain outside both envelopes. The merged atlas's entire
// right half (small forest/ground/unrelated broadleaf card) is deliberately dry.
fn forest_snow_atlas_crown(uv: vec2<f32>, category: u32) -> f32 {
    if (!all(abs(uv) < vec2<f32>(1e20)) || uv.y < 0.0 || uv.y > 1.01) { return 0.0; }
    if (category == 1u) {
        if (uv.x < -0.002 || uv.x > 1.002) { return 0.0; }
        return 1.0 - smoothstep(0.27, 0.42, uv.y);
    }
    if (category == 2u) {
        if (uv.x < -0.002 || uv.x > 0.501) { return 0.0; }
        // Keep the authored atlas seam conservatively clear, including mip bleed.
        let seam = 1.0 - smoothstep(0.48, 0.5, uv.x);
        return seam * (1.0 - smoothstep(0.24, 0.38, uv.y));
    }
    return 0.0;
}

fn forest_snow_map_cover(crown: f32, depth: f32, valid: f32, reach: f32) -> f32 {
    if (valid < 0.5) { return 0.0; }
    return tree_snow_cover_depth(crown, depth, clamp(reach, 0.0, 1.0));
}

fn forest_snow_coverage(world_pos: vec3<f32>, uv: vec2<f32>, category: u32) -> f32 {
    let crown = forest_snow_atlas_crown(uv, category);
    if (crown <= 0.0 || (frame.snow_surface.x <= 0.0 && frame.snow_surface.w <= 0.0)) { return 0.0; }
    let world_abs = world_pos + frame.cam_pos.xyz;
    if (!all(abs(world_abs) < vec3<f32>(1e20))) { return 0.0; }
    // Dedicated physical map only: pending/outside/border/overflow has no proof.
    // Do not invoke artistic near-map openness or a CPU lease for the whole forest.
    let valid = weather_map_coverage(world_abs);
    if (valid < 0.5) { return 0.0; }
    var depth = frame.snow_surface.x;
    if (frame.snow_surface.y >= 0.0 && frame.snow_surface.z > 0.0) {
        depth = max(depth, smoothstep(frame.snow_surface.y,
            frame.snow_surface.y + frame.snow_surface.z, world_abs.y) * frame.snow_surface.w);
    }
    return forest_snow_map_cover(crown, depth, valid, weather_map_reach(world_abs));
}
