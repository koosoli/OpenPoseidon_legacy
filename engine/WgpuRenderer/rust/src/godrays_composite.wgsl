// Volumetric sun shafts — COMPOSITE pass.
//
// Depth-aware (joint bilateral) upsample of the low-res march, added to the linear HDR scene.
//
// Why depth-aware and not a plain bilinear stretch: the march runs at a fraction of the screen
// resolution, so one low-res texel straddling a roof edge holds the average of "sky, full shaft"
// and "roof, no shaft". Stretched bilinearly that average bleeds a bright fringe around every
// silhouette — the classic half-res-volumetrics halo. Weighting each of the four neighbours by how
// closely its depth matches THIS pixel's depth makes the roof pixels take the roof neighbours and
// the sky pixels take the sky ones, and the fringe disappears.
//
// The march stores its own source depth in alpha, so no second low-res depth target is needed.
//
// Blend is additive (One, One) into the HDR target, BEFORE bloom / auto-exposure / tonemap: shafts
// are light, so they should bloom, drive eye adaptation and roll off through the same curve as
// every other light in the frame. Added after the tonemap they would be a flat white wash that no
// exposure change can affect, which is exactly what "additive white overlay" looks like.

struct GodRays {
    inv_view_proj: mat4x4<f32>,
    sun: vec4<f32>,
    sun_color: vec4<f32>,
    cam_pos: vec4<f32>,
    tune0: vec4<f32>,
    tune1: vec4<f32>,
    cloud_map: vec4<f32>,
    // xy = low-res target size in texels, zw = 1/size.
    lo_res: vec4<f32>,
};

@group(0) @binding(0) var<uniform> gr: GodRays;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(0) @binding(2) var rays_tex: texture_2d<f32>;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VsOut {
    let uv = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    var out: VsOut;
    out.uv = uv;
    out.clip = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    return out;
}

@fragment
fn fs_composite(in: VsOut) -> @location(0) vec4<f32> {
    let full_dims = vec2<f32>(textureDimensions(scene_depth));
    let full_texel = vec2<i32>(clamp(in.clip.xy, vec2<f32>(0.0), full_dims - vec2<f32>(1.0)));
    let depth_here = textureLoad(scene_depth, full_texel, 0);

    let lo_size = max(gr.lo_res.xy, vec2<f32>(1.0, 1.0));
    let lo_max = vec2<i32>(lo_size) - vec2<i32>(1, 1);
    // Continuous low-res coordinate of this pixel, minus the half-texel that turns it into the
    // 2x2 footprint's lower-left corner.
    let lo_coord = in.uv * lo_size - vec2<f32>(0.5, 0.5);
    let base = floor(lo_coord);
    let frac = lo_coord - base;
    let base_i = vec2<i32>(base);

    var sum = vec3<f32>(0.0, 0.0, 0.0);
    var weight_sum = 0.0;
    for (var j = 0; j < 2; j = j + 1) {
        for (var i = 0; i < 2; i = i + 1) {
            let p = clamp(base_i + vec2<i32>(i, j), vec2<i32>(0, 0), lo_max);
            let tap = textureLoad(rays_tex, p, 0);
            let wx = select(1.0 - frac.x, frac.x, i == 1);
            let wy = select(1.0 - frac.y, frac.y, j == 1);
            // Bilinear weight divided by the depth disagreement. Reversed-Z is ~1/distance, so
            // the metric is naturally scale-free: it separates hard at the near silhouettes where
            // the halo would be, and relaxes toward plain bilinear far away where it would not.
            // The 1e-4 floor keeps a zero-area corner from being dropped entirely.
            let w = max(wx * wy, 1.0e-4) / (1.0e-3 + abs(tap.a - depth_here));
            sum = sum + tap.rgb * w;
            weight_sum = weight_sum + w;
        }
    }

    // Clamped so a degenerate frame (NaN camera, unproject blow-up) cannot write a fireball into
    // the HDR target that bloom would then smear across the whole screen.
    let rays = clamp(sum / max(weight_sum, 1.0e-6), vec3<f32>(0.0, 0.0, 0.0), vec3<f32>(64.0, 64.0, 64.0));
    return vec4<f32>(rays, 0.0);
}
