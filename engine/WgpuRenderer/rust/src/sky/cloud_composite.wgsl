#import layered_fog_optics::{FogConsumer, fog_depth_fraction}
// Over-scene cloud composite (plan Phase 1). Upsamples the low-res cloud buffer produced by
// fs_cloud (sky.wgsl) — (inscatter.rgb, transmittance.a) — and outputs it PREMULTIPLIED so a
// fixed-function blend does `out = inscatter + scene*transmittance` over the lit HDR scene:
//   color: src_factor = One, dst_factor = Src (fragment.a = transmittance)
//
// The upsample is DEPTH-AWARE (bilateral). A plain bilinear stretch of the half-res buffer smears the
// cloud-vs-geometry silhouette — a sharp foreground occluder (the player's aircraft) ends up with a
// jaggy, half-res cloud edge against its full-res outline. Each full-res pixel blends a 3x3 neighbourhood
// of low-res cloud texels weighted by BOTH a Gaussian AND how well each texel's scene depth matches
// this pixel's scene depth. Across a depth discontinuity (plane vs sky behind) the mismatched texels
// are downweighted to preserve the full-res geometry silhouette. In smooth regions this also filters
// fixed ray-march jitter. Separate module (not sky.wgsl) so it keeps its own
// group(0) binding namespace.

@group(0) @binding(0) var cloud_lo: texture_2d<f32>;
@group(0) @binding(1) var cloud_samp: sampler;
// Full-res resolved prepass depth (reversed-Z: 0 = far/sky). Same view fs_cloud bounded its march to.
@group(0) @binding(2) var scene_depth: texture_depth_2d;

@group(0) @binding(3) var layer_fog: texture_3d<f32>;
@group(0) @binding(4) var<uniform> layer_packet: FogConsumer;
@group(0) @binding(5) var<uniform> layer_control: vec4<f32>;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VsOut {
    let uv = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    var out: VsOut;
    out.clip = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let full = vec2<f32>(textureDimensions(scene_depth));
    let lo = vec2<f32>(textureDimensions(cloud_lo));
    let lo_i = vec2<i32>(lo) - vec2<i32>(1);

    // This pixel's scene depth (the reference the low-res taps are matched against).
    let px = vec2<i32>(clamp(in.uv, vec2<f32>(0.0), vec2<f32>(0.9999)) * full);
    let d_ref = textureLoad(scene_depth, px, 0);

    // Centre the reconstruction kernel on the nearest low-res texel.
    let cf = in.uv * lo - 0.5;
    let base = floor(cf + 0.5);

    // Linearise the reference depth to ~view distance before comparing. Raw reversed-Z is hopelessly
    // non-linear: an occluder a km below the clouds has depth ~near/dist ~ 5e-5, a hair away from the
    // sky's 0, so a raw-depth test can't tell a distant object from the sky and the upsample bleeds
    // back to bilinear (jaggy edges, WORSE the farther the occluder). 1/d is proportional to distance,
    // so a RELATIVE distance difference cleanly separates surfaces at any range.
    let lin_ref = 1.0 / max(d_ref, 1e-6);

    var acc = vec4<f32>(0.0);
    var wsum = 0.0;
    // A small spatial reconstruction kernel suppresses fixed march jitter.
    // It adds no ray-march steps, temporal lag or additional render target.
    for (var k = 0; k < 9; k = k + 1) {
        let tc = base + vec2<f32>(f32(k % 3 - 1), f32(k / 3 - 1));
        let tci = clamp(vec2<i32>(tc), vec2<i32>(0), lo_i);
        let cloud = textureLoad(cloud_lo, tci, 0);
        // Depth this low-res texel represents: the full-res depth at its centre (each low-res pixel was
        // marched from one full-res depth tap). exp(-K * relative-distance-diff) rejects mismatched taps
        // at a silhouette (=> edge snaps to the full-res geometry outline) and is ~1 where depth is flat
        // (=> spatial smoothing). The small floor keeps a spatial fallback so a thin sliver whose depth
        // matches no tap can't collapse to transmittance 0 (which would punch a black hole).
        let cpx = vec2<i32>(clamp((tc + 0.5) / lo, vec2<f32>(0.0), vec2<f32>(0.9999)) * full);
        let d_lo = textureLoad(scene_depth, cpx, 0);
        let lin_lo = 1.0 / max(d_lo, 1e-6);
        let rel = abs(lin_ref - lin_lo) / max(max(lin_ref, lin_lo), 1e-6);
        let delta = tc - cf;
        let spatial = exp2(-2.0 * dot(delta, delta));
        let w = spatial * max(exp(-rel * 24.0), 0.02);
        acc += cloud * w;
        wsum += w;
    }
    let c = acc / max(wsum, 1e-6);
    if (layer_control.x < 0.5 || layer_packet.control.x < 0.5) { return c; }
    // The finite layers are certified below the cloud deck. The background
    // already contains their L/T; transport cloud radiance through them once.
    let fraction = fog_depth_fraction(layer_packet,in.uv,d_ref);
    let local = textureSampleLevel(layer_fog,cloud_samp,
        vec3<f32>(in.uv,sqrt(fraction)),0.0);
    return vec4<f32>(c.rgb * local.a + local.rgb * (1.0-c.a),c.a);
}
