#define_import_path gbuffer

// Partial G-buffer helpers for the depth+normal prepass (docs/depth-prepass-plan.md).
// The prepass' one colour attachment is a view-space normal target, octahedral-encoded
// into Rg16Float — compact and banding-free, which the future SSAO/GTAO/SSR consumers
// need (precision matters for AO). Encode in the prepass fragment; a consumer decodes
// with oct_decode once one exists (none in Stage 1).

// sign() returns 0 at 0, which would collapse a hemisphere fold; force +1 there.
fn sign_nz(v: vec2<f32>) -> vec2<f32> {
    return select(vec2<f32>(-1.0), vec2<f32>(1.0), v >= vec2<f32>(0.0));
}

// Unit vector -> vec2 in [-1,1] (Cigolle et al. octahedral mapping).
fn oct_encode(n_in: vec3<f32>) -> vec2<f32> {
    let n = n_in / (abs(n_in.x) + abs(n_in.y) + abs(n_in.z));
    let folded = (1.0 - abs(vec2<f32>(n.y, n.x))) * sign_nz(n.xy);
    return select(n.xy, folded, n.z < 0.0);
}

// Inverse of oct_encode: vec2 in [-1,1] -> unit vector. Kept alongside the encoder so
// the first screen-space consumer has it ready.
fn oct_decode(e: vec2<f32>) -> vec3<f32> {
    var v = vec3<f32>(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        let xy = (1.0 - abs(vec2<f32>(v.y, v.x))) * sign_nz(v.xy);
        v = vec3<f32>(xy.x, xy.y, v.z);
    }
    return normalize(v);
}

// Alpha-to-coverage sharpen for cutout foliage. Rescales the sampled alpha so the
// alpha-test transition spans ~1px (Wyman/McGuire "Anti-Aliased Alpha Test"), which keeps
// the A2C coverage from dissolving foliage at distance (where mip'd alpha trends toward the
// average) while still dithering the edge across MSAA samples. MUST be called from uniform
// control flow (uses fwidth) and produce IDENTICAL results in the prepass and the colour
// pass so their per-sample coverage masks match by construction.
fn a2c_coverage(alpha: f32, cutoff: f32) -> f32 {
    return clamp((alpha - cutoff) / max(fwidth(alpha), 1e-4) + 0.5, 0.0, 1.0);
}

// Coverage-preserving alpha for cutout foliage at distance. A leaf card's alpha is sparse,
// so every mip's box filter drags it toward the average and, a few mips down, UNDER the
// 0.5 cutoff everywhere: the canopy dissolves to a pixel-speckle while its shadow (a solid
// LOD) stays full. The sharpen above cannot help -- it rescales about the cutoff, and once
// alpha sits below it over the whole card there is no transition left to sharpen. The
// standard answer (Castano; UE "mip alpha scale") is to lift alpha with the mip level so the
// covered fraction at the cutoff stays roughly what mip 0 had. `k` is the per-mip gain
// (0 = off, the previous behaviour); dims is the texture's mip-0 size in texels. Uniform
// control flow only (derivatives).
fn mip_alpha_boost(alpha: f32, uv: vec2<f32>, dims: vec2<f32>, k: f32) -> f32 {
    if (k <= 0.0) {
        return alpha;
    }
    let dx = dpdx(uv) * dims;
    let dy = dpdy(uv) * dims;
    let lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8));
    return clamp(alpha * (1.0 + max(lod, 0.0) * k), 0.0, 1.0);
}
