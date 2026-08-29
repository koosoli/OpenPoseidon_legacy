// REN-TEMP-001E/F/G — static-world velocity from depth + the previous camera, and its
// debug view. Standalone module (no frame:: import): the pass runs outside the camera
// UBO's dynamic-offset world and carries its own matrices, because it needs the
// UNJITTERED current AND previous camera, and Frame holds only the current jittered one.
//
// Conventions (all measured against this renderer, not assumed):
//   * stored depth is REVERSED: forward ndc z = 1.0 - stored (water.wgsl seabed_depth);
//     the clear value 0.0 is "far / nothing drawn", and stored d = near / distance.
//   * inv_view_proj maps forward-NDC -> CAMERA-RELATIVE world (view translation is
//     zeroed; gfx3d camera upload inverts view and proj separately in f64).
//   * a camera-relative point rebases between frames as
//     rel_prev = rel_cur + (cam_pos_cur - cam_pos_prev).
//   * output is UV-space motion, current -> previous (multiply by the render size for
//     pixels). Vendor backends translate sign/scale to their own convention at the
//     integration boundary, not here.

struct TemporalParams {
    // Current UNJITTERED forward-NDC -> camera-relative world (f64-inverted on the CPU).
    inv_view_proj: mat4x4<f32>,
    // Previous UNJITTERED camera-relative world -> clip.
    prev_view_proj: mat4x4<f32>,
    // xyz = cam_pos_cur - cam_pos_prev (metres, world); w = 1 history valid, 0 = reset
    // (first frame, size change, mode toggle) -> velocity is written as zero.
    cam_delta: vec4<f32>,
    // x = debug magnitude scale, y unused, zw = the DEBUG target's size in pixels
    // (the velocity texture is render-res, the debug view draws at output-res).
    misc: vec4<f32>,
};

@group(0) @binding(0) var scene_depth: texture_depth_2d;
@group(0) @binding(1) var<uniform> tp: TemporalParams;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
};

@vertex
fn vs_fullscreen(@builtin(vertex_index) vi: u32) -> VsOut {
    var out: VsOut;
    let x = f32(i32(vi) / 2) * 4.0 - 1.0;
    let y = f32(i32(vi) & 1) * 4.0 - 1.0;
    out.pos = vec4<f32>(x, y, 0.0, 1.0);
    return out;
}

@fragment
fn fs_velocity(in: VsOut) -> @location(0) vec2<f32> {
    if (tp.cam_delta.w < 0.5) {
        return vec2<f32>(0.0, 0.0);
    }
    let dims = vec2<f32>(textureDimensions(scene_depth));
    let frag = vec2<i32>(in.pos.xy);
    // Clamp the far-clear 0 to a tiny stored depth instead of branching: stored d =
    // near/distance, so 1e-7 IS a point hundreds of kilometres out, and the sky then
    // reprojects by camera rotation with negligible (sub-1e-4 px) translation parallax.
    let d = max(textureLoad(scene_depth, frag, 0), 1e-7);
    let uv = in.pos.xy / dims;
    let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    let rel_h = tp.inv_view_proj * vec4<f32>(ndc.x, ndc.y, 1.0 - d, 1.0);
    let rel = rel_h.xyz / rel_h.w;
    let prev_clip = tp.prev_view_proj * vec4<f32>(rel + tp.cam_delta.xyz, 1.0);
    // Behind the previous camera: the point was not on screen last frame — there is no
    // valid motion to report. Zero is the deliberate "no history" answer, not a guess.
    if (prev_clip.w <= 1e-6) {
        return vec2<f32>(0.0, 0.0);
    }
    let prev_ndc = prev_clip.xy / prev_clip.w;
    let delta_ndc = ndc - prev_ndc;
    return vec2<f32>(delta_ndc.x * 0.5, delta_ndc.y * -0.5);
}

// ---------------------------------------------------------------------------------
// REN-TEMP-001K — temporal debug RESOLVE: a deliberately minimal accumulator whose only
// job is to expose bad temporal inputs (wrong jitter sign, wrong velocity scale, missing
// resets) before a vendor upscaler hides or amplifies them. Reproject history along
// velocity, reject by 3x3 neighbourhood clamp, blend 90/10. NOT a production TAA —
// the plan forbids polishing it into one.

@group(0) @binding(0) var res_color: texture_2d<f32>;    // this frame's resolved HDR
@group(0) @binding(1) var res_velocity: texture_2d<f32>; // uv-space motion, cur -> prev
@group(0) @binding(2) var res_history: texture_2d<f32>;  // last frame's accumulator
@group(0) @binding(3) var res_samp: sampler;             // bilinear, clamp-to-edge
@group(0) @binding(4) var<uniform> res_params: TemporalParams; // cam_delta.w = history valid

@fragment
fn fs_temporal_resolve(in: VsOut) -> @location(0) vec4<f32> {
    let dims = vec2<f32>(textureDimensions(res_color));
    let frag = vec2<i32>(in.pos.xy);
    let uv = in.pos.xy / dims;
    let cur = textureLoad(res_color, frag, 0);
    if (res_params.cam_delta.w < 0.5) {
        return cur; // explicit reset: history is not consulted at all
    }
    let vel = textureLoad(res_velocity, frag, 0).xy;
    let prev_uv = uv - vel;
    if (prev_uv.x < 0.0 || prev_uv.y < 0.0 || prev_uv.x > 1.0 || prev_uv.y > 1.0) {
        return cur; // reprojected off-screen: no history exists there
    }
    var hist = textureSampleLevel(res_history, res_samp, prev_uv, 0.0);
    // 3x3 neighbourhood clamp — the standard minimal disocclusion/ghosting rejection.
    var lo = cur.rgb;
    var hi = cur.rgb;
    for (var dy = -1; dy <= 1; dy = dy + 1) {
        for (var dx = -1; dx <= 1; dx = dx + 1) {
            let n = textureLoad(res_color, frag + vec2<i32>(dx, dy), 0).rgb;
            lo = min(lo, n);
            hi = max(hi, n);
        }
    }
    let clamped = clamp(hist.rgb, lo, hi);
    return vec4<f32>(mix(cur.rgb, clamped, 0.9), cur.a);
}

// History display (WGR_TEMPORAL_DEBUG=2): the accumulator is linear HDR, so roll it off
// with a plain Reinhard for inspection — the point is stability/sharpness/ghost trails,
// not colour fidelity.
@group(0) @binding(0) var hist_view_tex: texture_2d<f32>;
@group(0) @binding(4) var<uniform> hist_params: TemporalParams; // misc.zw = dst size

@fragment
fn fs_history_debug(in: VsOut) -> @location(0) vec4<f32> {
    let dims = vec2<f32>(textureDimensions(hist_view_tex));
    let uv = in.pos.xy / max(hist_params.misc.zw, vec2<f32>(1.0, 1.0));
    let c = textureLoad(hist_view_tex, vec2<i32>(uv * dims), 0).rgb;
    let mapped = c / (vec3<f32>(1.0) + c);
    return vec4<f32>(mapped, 1.0);
}

// ---------------------------------------------------------------------------------
// Debug view: velocity direction/magnitude as colour, drawn over the tonemapped frame.
//   R = 0.5 + vel.x * scale, G = 0.5 + vel.y * scale  (grey = static)
//   B = 1 on a history-reset frame, so a reset is visible for exactly one frame.

@group(0) @binding(0) var velocity_tex: texture_2d<f32>;
@group(0) @binding(1) var<uniform> dbg: TemporalParams;

@fragment
fn fs_velocity_debug(in: VsOut) -> @location(0) vec4<f32> {
    let vdims = vec2<f32>(textureDimensions(velocity_tex));
    let uv = in.pos.xy / max(dbg.misc.zw, vec2<f32>(1.0, 1.0));
    let texel = vec2<i32>(uv * vdims);
    let vel = textureLoad(velocity_tex, texel, 0).xy;
    let scale = dbg.misc.x;
    let reset = select(0.0, 1.0, dbg.cam_delta.w < 0.5);
    let r = clamp(0.5 + vel.x * scale, 0.0, 1.0);
    let g = clamp(0.5 + vel.y * scale, 0.0, 1.0);
    return vec4<f32>(r, g, reset, 1.0);
}
