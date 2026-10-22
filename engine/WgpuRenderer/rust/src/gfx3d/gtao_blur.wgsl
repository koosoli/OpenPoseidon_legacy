// Bilateral denoise for GTAO (docs/screen-space-ao-plan.md §4).
//
// This is the load-bearing step, not a polish pass. The project runs MSAA with NO TAA, so
// there is no history to accumulate the GTAO dither into — this blur is the entire denoise
// budget. If it is weakened, the interleaved-gradient noise in the AO becomes visible
// directly.
//
// Separable: dispatched twice, horizontal then vertical, with `axis` selecting. A separable
// pass is O(2N) taps instead of O(N²) and is what makes a radius wide enough to erase the
// dither affordable.
//
// Edge-aware on TWO signals, and both are needed:
//   - view-depth difference stops AO bleeding across silhouettes, which would otherwise draw
//     a dark halo around every object against the sky;
//   - normal divergence stops it bleeding across creases, so a wall-floor junction keeps its
//     contact darkening instead of smearing it flat.
// Depth alone is not enough: two surfaces meeting at a crease are at nearly the same depth.

struct BlurParams {
    // xy = size in px, zw = 1/size.
    screen: vec4<f32>,
    // x = axis (0 = horizontal, 1 = vertical), y = radius in taps,
    // z = depth rejection scale, w = normal rejection power.
    tuning: vec4<f32>,
};

@group(0) @binding(0) var depth_tex: texture_depth_2d;
@group(0) @binding(1) var normal_tex: texture_2d<f32>;
@group(0) @binding(2) var ao_in: texture_2d<f32>;
@group(0) @binding(3) var<uniform> params: BlurParams;
@group(0) @binding(4) var ao_out: texture_storage_2d<rgba16float, write>;

// Must match shaders/gbuffer.wgsl's oct_encode. Duplicated for the same reason gtao.wgsl
// duplicates it: include_str! module, no naga_oil composer.
fn oct_decode(e: vec2<f32>) -> vec3<f32> {
    var v = vec3<f32>(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
        v = vec3<f32>((vec2<f32>(1.0) - abs(v.yx)) * s, v.z);
    }
    return normalize(v);
}

// PERF-GTAO-BLUR-STRIP: one workgroup filters a 64-pixel STRIP along the blur axis. The strip and
// its halo are loaded into workgroup memory once -- depth, the DECODED normal and the AO -- so a
// texel is fetched and oct-decoded once per strip instead of once per tap (2r+1 = 13 times at the
// default radius 6). Each pixel then runs exactly the loop it always did, over the same clamped
// neighbours in the same order: the result is bit-identical. Radii beyond STRIP_HALO take the
// direct path below, unchanged.
const STRIP: i32 = 64;
const STRIP_HALO: i32 = 16;
var<workgroup> strip_depth: array<f32, 96>;
var<workgroup> strip_normal: array<vec3<f32>, 96>;
var<workgroup> strip_ao: array<vec4<f32>, 96>;

fn blur_axis() -> vec2<i32> {
    return select(vec2<i32>(1, 0), vec2<i32>(0, 1), params.tuning.x > 0.5);
}

fn blur_store(px: vec2<i32>, filtered: vec4<f32>) {
    let bent_len = length(filtered.xyz);
    let bent = select(vec3<f32>(0.0, 0.0, -1.0), filtered.xyz / bent_len, bent_len > 1e-5);
    textureStore(ao_out, px, vec4<f32>(bent, filtered.a));
}

// The original per-pixel kernel, kept for radii the strip cannot hold.
fn blur_direct(px: vec2<i32>, dims: vec2<i32>) {
    let d_centre = textureLoad(depth_tex, px, 0);
    if (d_centre <= 0.0) {
        textureStore(ao_out, px, vec4<f32>(0.0, 0.0, -1.0, 1.0));
        return;
    }
    let n_centre = oct_decode(textureLoad(normal_tex, px, 0).xy);

    let axis = blur_axis();
    let radius = i32(max(params.tuning.y, 1.0));
    let depth_scale = max(params.tuning.z, 1e-4);
    let normal_power = max(params.tuning.w, 1.0);

    var sum = vec4<f32>(0.0);
    var weight_sum = 0.0;
    for (var i = -radius; i <= radius; i = i + 1) {
        let q = clamp(px + axis * i, vec2<i32>(0), dims - vec2<i32>(1));
        let dq = textureLoad(depth_tex, q, 0);
        if (dq <= 0.0) {
            continue; // never pull sky into a surface's AO
        }

        let x = f32(i) / f32(radius);
        let w_spatial = exp(-3.0 * x * x);

        let rel = abs(dq - d_centre) / max(max(dq, d_centre), 1e-6);
        let w_depth = exp(-rel / depth_scale);

        let nq = oct_decode(textureLoad(normal_tex, q, 0).xy);
        let w_normal = pow(clamp(dot(nq, n_centre), 0.0, 1.0), normal_power);

        let w = w_spatial * w_depth * w_normal;
        sum = sum + textureLoad(ao_in, q, 0) * w;
        weight_sum = weight_sum + w;
    }

    blur_store(px, select(textureLoad(ao_in, px, 0), sum / weight_sum, weight_sum > 1e-5));
}

// Dispatched as (ceil(axis length / STRIP), cross length, 1): x = strip along the axis, y = the
// row (horizontal pass) or column (vertical pass).
@compute @workgroup_size(64, 1, 1)
fn cs_gtao_blur(@builtin(workgroup_id) wg: vec3<u32>, @builtin(local_invocation_index) li: u32) {
    let dims = vec2<i32>(textureDimensions(ao_out));
    let axis = blur_axis();
    let along = i32(wg.x) * STRIP + i32(li);
    let px = select(vec2<i32>(along, i32(wg.y)), vec2<i32>(i32(wg.y), along), params.tuning.x > 0.5);
    let inside = px.x < dims.x && px.y < dims.y;
    let radius = i32(max(params.tuning.y, 1.0));
    if (radius > STRIP_HALO) {
        if (inside) {
            blur_direct(px, dims);
        }
        return;
    }

    // Load the strip and its halo: tile slot t holds axis coordinate start - STRIP_HALO + t, clamped
    // to the image exactly as the per-tap clamp below would.
    let start = vec2<i32>(i32(wg.x) * STRIP) * axis + vec2<i32>(i32(wg.y)) * (vec2<i32>(1) - axis);
    for (var t = i32(li); t < STRIP + 2 * STRIP_HALO; t = t + STRIP) {
        let q = clamp(start + axis * (t - STRIP_HALO), vec2<i32>(0), dims - vec2<i32>(1));
        strip_depth[t] = textureLoad(depth_tex, q, 0);
        strip_normal[t] = oct_decode(textureLoad(normal_tex, q, 0).xy);
        strip_ao[t] = textureLoad(ao_in, q, 0);
    }
    workgroupBarrier();
    if (!inside) {
        return;
    }

    let c = i32(li) + STRIP_HALO;
    let d_centre = strip_depth[c];
    if (d_centre <= 0.0) {
        textureStore(ao_out, px, vec4<f32>(0.0, 0.0, -1.0, 1.0));
        return;
    }
    let n_centre = strip_normal[c];

    let depth_scale = max(params.tuning.z, 1e-4);
    let normal_power = max(params.tuning.w, 1.0);

    var sum = vec4<f32>(0.0);
    var weight_sum = 0.0;
    for (var i = -radius; i <= radius; i = i + 1) {
        // The strip is only STRIP wide; a tap past the image edge clamps to the edge texel, which
        // is the tile slot of the clamped coordinate -- recompute it rather than trust c + i.
        let qa = clamp(dot(px, axis) + i, 0, dot(dims, axis) - 1);
        let slot = qa - dot(start, axis) + STRIP_HALO;
        let dq = strip_depth[slot];
        if (dq <= 0.0) {
            continue; // never pull sky into a surface's AO
        }

        let x = f32(i) / f32(radius);
        let w_spatial = exp(-3.0 * x * x);

        let rel = abs(dq - d_centre) / max(max(dq, d_centre), 1e-6);
        let w_depth = exp(-rel / depth_scale);

        let nq = strip_normal[slot];
        let w_normal = pow(clamp(dot(nq, n_centre), 0.0, 1.0), normal_power);

        let w = w_spatial * w_depth * w_normal;
        sum = sum + strip_ao[slot] * w;
        weight_sum = weight_sum + w;
    }

    blur_store(px, select(strip_ao[c], sum / weight_sum, weight_sum > 1e-5));
}
