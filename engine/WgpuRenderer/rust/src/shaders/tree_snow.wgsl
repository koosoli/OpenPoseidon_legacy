#define_import_path tree_snow
#import frame::{frame, interior_rain_coverage, interior_rain_reach}
#import snow_material::snow_surface_coverage

// Smooth broad crown envelope in AUTHORED model height, independent of wind,
// selected LOD, camera and leaf-card normals. CPU proof restricts this to leaf
// cutouts of actual individual static tree owners. Noncutout bark stays dry.
fn tree_snow_height(model_y: f32, min_y: f32, inverse_height: f32, proof: f32) -> f32 {
    if (proof <= 0.5 || inverse_height <= 0.0) { return 0.0; }
    return smoothstep(0.45, 0.85, clamp((model_y - min_y) * inverse_height, 0.0, 1.0));
}

fn tree_snow_cover_depth(crown_height: f32, depth: f32, exposure: f32) -> f32 {
#ifdef DISABLE_TREE_SNOW
    return 0.0;
#else
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
