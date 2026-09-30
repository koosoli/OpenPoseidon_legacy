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
    // Retail photographed foliage is very dark in linear space (median around
    // .01, not .1). The old .008..060 gate left most positively coated leaves
    // green. Compress that photographic shading into a readable frost response
    // while retaining genuinely black recesses and ordered luminance contrast.
    let relief = 0.30 + 0.70 * sqrt(lum / (lum + 0.05));
    let recess = smoothstep(0.001, 0.010, lum);
    // Only metre-scale diffuse clumps: no crystalline points, powder normals or
    // time seed on moving/subpixel leaf cards. UV/albedo detail moves with leaves.
    var mottling = 0.0;
    if (footprint < 0.9) {
        let p = world.xz + world.y * vec2<f32>(0.17, -0.11);
        mottling = snow_noise(p / 1.8 + vec2<f32>(-31.2, 14.7)).x * 0.025 * snow_band(footprint, 1.8);
    }
    let frost = vec3<f32>(0.78, 0.80, 0.82) * relief * (1.0 + mottling);
    // Covered dark recesses are neutral snow shade, never the original green
    // hue. The caller still mixes this by actual physical snow coverage; source
    // colour remains exact where coating/receiver/shelter admission is zero.
    return mix(vec3<f32>(lum), frost, recess);
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

// Match the actual material's eight-sampler ABI (bit0 clamp U, bit1 clamp V).
// Far triangular strips and canopy tops repeat outside [0,1]; their photographed
// foliage repeats with them. Clamped near atlases never wrap into a bark region.
fn forest_snow_sample_uv(uv: vec2<f32>, sampler_index: u32) -> vec2<f32> {
    if (!all(abs(uv) < vec2<f32>(1e20)) || sampler_index > 7u) { return vec2<f32>(1e30); }
    return vec2<f32>(select(fract(uv.x), clamp(uv.x,0.0,1.0), (sampler_index & 1u) != 0u),
        select(fract(uv.y), clamp(uv.y,0.0,1.0), (sampler_index & 2u) != 0u));
}

// Four texels inside an authored 1024px atlas boundary: protect neighbouring
// bark/ground regions, including mip bleed. The exact texture is CPU-admitted.
fn forest_snow_crown_rect(uv: vec2<f32>, lo: vec2<f32>, hi: vec2<f32>) -> f32 {
    let edge = vec2<f32>(0.004);
    let a = smoothstep(lo, lo + edge, uv);
    let b = 1.0 - smoothstep(hi - edge, hi, uv);
    return a.x * a.y * b.x * b.y;
}

// All inspected stock forest visual LODs: explicit photographic crown regions,
// never an inferred per-tree component or whole-cluster height. Full foliage
// category 5 is restricted to foliage-only photos, including canopy-top cards.
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
    if (category == 3u) {
        // CWA 00001&krovi4: exclude the bare branch skeleton (U .25-.5,
        // V .25-.5), lower-right bark/log and unused grey atlas regions.
        if (uv.x < 0.0 || uv.x > 1.0) { return 0.0; }
        if (uv.y < 0.25) {
            var c = forest_snow_crown_rect(uv,vec2<f32>(0.0,0.0),vec2<f32>(0.25,0.14));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.25,0.0),vec2<f32>(0.5,0.24)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.5,0.0),vec2<f32>(0.75,0.25)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.75,0.0),vec2<f32>(1.0,0.25)));
            return c;
        }
        if (uv.y < 0.5) {
            var c = forest_snow_crown_rect(uv,vec2<f32>(0.0,0.25),vec2<f32>(0.25,0.5));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.5,0.25),vec2<f32>(0.75,0.5)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.75,0.33),vec2<f32>(1.0,0.5)));
            return c;
        }
        if (uv.y < 0.75) {
            var c = forest_snow_crown_rect(uv,vec2<f32>(0.0,0.5),vec2<f32>(0.25,0.75));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.25,0.5),vec2<f32>(0.5,0.75)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.5,0.5),vec2<f32>(0.75,0.75)));
            return c;
        }
        var c = forest_snow_crown_rect(uv,vec2<f32>(0.0,0.75),vec2<f32>(0.25,0.88));
        c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.25,0.75),vec2<f32>(0.5,0.875)));
        c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.0,0.875),vec2<f32>(0.125,1.0)));
        return c;
    }
    if (category == 4u) {
        // Abel 00011&str_fikovnik: leaf-only tall card, crown photos and canopy
        // top; omit the U .25-.35 bark strip and ALL V>=.75 root/bark/blank tiles.
        if (uv.x < 0.0 || uv.x > 1.0) { return 0.0; }
        if (uv.y < 0.25) {
            var c = forest_snow_crown_rect(uv,vec2<f32>(0.0,0.0),vec2<f32>(0.25,0.18));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.25,0.0),vec2<f32>(0.5,0.17)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.5,0.0),vec2<f32>(0.75,0.5)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.75,0.0),vec2<f32>(1.0,0.22)));
            return c;
        }
        if (uv.y < 0.5) {
            var c = forest_snow_crown_rect(uv,vec2<f32>(0.5,0.0),vec2<f32>(0.75,0.5));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.0,0.25),vec2<f32>(0.25,0.35)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.36,0.25),vec2<f32>(0.5,0.5)));
            c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.75,0.25),vec2<f32>(1.0,0.43)));
            return c;
        }
        if (uv.y >= 0.75) { return 0.0; }
        var c = forest_snow_crown_rect(uv,vec2<f32>(0.0,0.5),vec2<f32>(0.25,0.66));
        c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.25,0.5),vec2<f32>(0.5,0.75)));
        c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.5,0.5),vec2<f32>(0.75,0.75)));
        c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.75,0.5),vec2<f32>(0.875,0.61)));
        c = max(c,forest_snow_crown_rect(uv,vec2<f32>(0.75,0.65),vec2<f32>(0.875,0.72)));
        return c;
    }
    if (uv.x < -0.002 || uv.x > 1.002) { return 0.0; }
    if (category == 5u) { return 1.0; } // inspected foliage-only cutout photos
    if (category == 6u) { return 1.0 - smoothstep(0.65,0.90,uv.y); } // shrub stems protected
    if (category == 7u) { return 1.0 - smoothstep(0.27,0.42,uv.y); } // passable forest strip
    if (category == 8u) { return 1.0 - smoothstep(0.20,0.32,uv.y); } // dark strip, no V>=.5 ground
    if (category == 9u) {
        // lesabelnew's small crowns occupy two upper rows; its lower half is
        // a photographed foliage wall. Protect the first row's exposed stems.
        if (uv.y < 0.25) { return 1.0 - smoothstep(0.12,0.18,uv.y); }
        if (uv.y < 0.5) { return 1.0 - smoothstep(0.43,0.48,uv.y); }
        return 1.0 - smoothstep(0.88,0.98,uv.y);
    }
    // Nogova O family, independently decoded from original O.pbo. The far
    // forest sheet includes LONG middle trunks and lower scattered leaf clumps:
    // neither is part of its upper crown envelope.
    if (category == 10u) { return 1.0 - smoothstep(0.20,0.31,uv.y); }
    if (category == 11u) { return 1.0 - smoothstep(0.36,0.50,uv.y); }
    if (category == 12u) { return 1.0 - smoothstep(0.48,0.62,uv.y); }
    if (category == 13u) {
        // smrcicicek: bare shoot at top, leaf-bearing branches below.
        return smoothstep(0.32,0.42,uv.y) * (1.0 - smoothstep(0.85,0.96,uv.y));
    }
    if (category == 14u) {
        // afn_strom_04_new2's left slender leaf sprays are separate from the
        // large right tree; its extended central trunk is below the crown crop.
        if (uv.x < 0.20) { return 1.0 - smoothstep(0.85,0.98,uv.y); }
        return 1.0 - smoothstep(0.24,0.32,uv.y);
    }
    if (category == 15u) {
        // dd_bush06 LEFT half is a completely bare branch skeleton.
        return smoothstep(0.50,0.53,uv.x) * (1.0 - smoothstep(0.65,0.90,uv.y));
    }
    return 0.0;
}

fn forest_snow_map_cover(crown: f32, depth: f32, valid: f32, reach: f32) -> f32 {
    if (valid < 0.5) { return 0.0; }
    return tree_snow_cover_depth(crown, depth, clamp(reach, 0.0, 1.0));
}

// Opt-in diagnostic uses the existing source crown mask, not a whole-cluster
// colour override. RGB is actual final coverage before lighting/frost albedo;
// w is a diagnostic admission marker, never the sampled/cutout output alpha.
// Black = admitted crown with zero physical coat; white = complete coat.
// Bark, skeleton, ground and unknown atlas categories retain normal rendering.
fn forest_snow_diagnostic_colour(uv: vec2<f32>, category: u32, cover: f32) -> vec4<f32> {
    if (forest_snow_atlas_crown(uv, category) <= 0.0) { return vec4<f32>(0.0); }
    var finite_cover = 0.0;
    if (abs(cover) < 1e20) { finite_cover = clamp(cover, 0.0, 1.0); }
    return vec4<f32>(vec3<f32>(finite_cover), 1.0);
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
