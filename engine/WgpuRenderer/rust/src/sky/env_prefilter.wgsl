// REFLECT-001 phase D -- GGX prefilter of the sky reflection env map (equirect).
//
// Mip k of the env texture holds the sky as a surface of perceptual roughness k / (ENV_MIPS - 1)
// reflects it. Each level is filtered from the level above it with the INCREMENTAL lobe (the
// lobe that, convolved with the one already in level k-1, gives level k's), so every pass needs
// only a small, fixed sample count and the chain costs a few hundred thousand taps a frame.
//
// Mip 0 is the unchanged sky bake. Every existing consumer (water, glass, ground wetness, fog,
// the SH projection) reads level 0 explicitly, so their pixels do not change.

struct Params {
    alpha: f32,      // incremental GGX alpha for this level (roughness^2 space)
    width: f32,      // target level size in texels
    height: f32,
    _pad: f32,
};

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var src_samp: sampler;
@group(0) @binding(2) var<uniform> params: Params;

const PI: f32 = 3.14159265;
const SAMPLES: u32 = 32u;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    let xy = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VsOut;
    o.pos = vec4<f32>(xy * 2.0 - vec2<f32>(1.0), 0.0, 1.0);
    return o;
}

// Same equirect convention as shader3d.wgsl::sky_env_sample:
//   u = 0.5 + atan2(z, x) / 2pi,  v = acos(y) / pi
fn dir_from_uv(uv: vec2<f32>) -> vec3<f32> {
    let phi = (uv.x - 0.5) * 2.0 * PI;
    let theta = uv.y * PI;
    let s = sin(theta);
    return vec3<f32>(s * cos(phi), cos(theta), s * sin(phi));
}

fn uv_from_dir(d: vec3<f32>) -> vec2<f32> {
    return vec2<f32>(0.5 + atan2(d.z, d.x) / (2.0 * PI), acos(clamp(d.y, -1.0, 1.0)) / PI);
}

fn hammersley(i: u32, n: u32) -> vec2<f32> {
    return vec2<f32>(f32(i) / f32(n), f32(reverseBits(i)) * 2.3283064365386963e-10);
}

@fragment
fn fs_prefilter(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let uv = frag.xy / vec2<f32>(params.width, params.height);
    let n = dir_from_uv(uv);
    if (params.alpha <= 0.0001) {
        return vec4<f32>(textureSampleLevel(src, src_samp, uv, 0.0).rgb, 1.0);
    }
    let up = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), abs(n.y) > 0.999);
    let t = normalize(cross(up, n));
    let b = cross(n, t);
    let a2 = params.alpha * params.alpha;
    var acc = vec3<f32>(0.0);
    var weight = 0.0;
    for (var i = 0u; i < SAMPLES; i = i + 1u) {
        let xi = hammersley(i, SAMPLES);
        let phi = 2.0 * PI * xi.x;
        let cos_t = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        let sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0));
        let h = t * (sin_t * cos(phi)) + b * (sin_t * sin(phi)) + n * cos_t;
        // N = V = R, the usual split-sum assumption.
        let l = 2.0 * dot(n, h) * h - n;
        let ndl = dot(n, l);
        if (ndl > 0.0) {
            acc = acc + textureSampleLevel(src, src_samp, uv_from_dir(l), 0.0).rgb * ndl;
            weight = weight + ndl;
        }
    }
    return vec4<f32>(acc / max(weight, 1e-4), 1.0);
}
