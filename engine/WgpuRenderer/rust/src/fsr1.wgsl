// FSR 1.0 — EASU (edge-adaptive spatial upsampling) + RCAS (robust contrast-adaptive
// sharpening), ported to WGSL from AMD's FidelityFX ffx_a.h / ffx_fsr1.h (MIT license,
// Copyright (c) 2021 Advanced Micro Devices, Inc.).
// GPL-compatible — this backend ships in every build, unlike DLSS.
//
// Our chain slot hands us LINEAR HDR, but FSR1 is specified for tonemapped [0,1]
// input. AMD's own answer ships in the same header: SRTM, the simple reversible
// tone-mapper c/(max3(c)+1). EASU tonemaps every tap on load, RCAS sharpens in the
// tonemapped domain and inverts SRTM on store — HDR in, HDR out, chain unchanged.
//
// Faithful port, texel-major instead of AMD's component-planar layout (same math,
// same gather quads); f32 path only (no packed-f16 variant — OFP content is cheap).

struct FsrParams {
    // FsrEasuCon(): output-pixel -> input-viewport mapping and gather offsets.
    con0: vec4<f32>,
    con1: vec4<f32>,
    con2: vec4<f32>,
    con3: vec4<f32>,
    // x = RCAS sharpness (linear, 2^-stops); y/z/w unused.
    rcas: vec4<f32>,
};

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> params: FsrParams;
@group(0) @binding(3) var dst: texture_storage_2d<rgba16float, write>;

// ---- ffx_a.h approximations (bit tricks preserved exactly) ----
fn prx_lo_rcp(a: f32) -> f32 {
    return bitcast<f32>(0x7ef07ebbu - bitcast<u32>(a));
}
fn prx_med_rcp(a: f32) -> f32 {
    let b = bitcast<f32>(0x7ef19fffu - bitcast<u32>(a));
    return b * (-b * a + 2.0);
}
fn prx_lo_rsq(a: f32) -> f32 {
    return bitcast<f32>(0x5f347d74u - (bitcast<u32>(a) >> 1u));
}
fn max3(a: f32, b: f32, c: f32) -> f32 {
    return max(a, max(b, c));
}
fn min3(a: f32, b: f32, c: f32) -> f32 {
    return min(a, min(b, c));
}
fn min3v(a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> vec3<f32> {
    return min(a, min(b, c));
}
fn max3v(a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> vec3<f32> {
    return max(a, max(b, c));
}

// SRTM: linear HDR {0..inf} -> {0..1}, and back (inverse guards the c=1 pole).
fn srtm(c: vec3<f32>) -> vec3<f32> {
    return c * (1.0 / (max3(c.r, c.g, c.b) + 1.0));
}
fn srtm_inv(c: vec3<f32>) -> vec3<f32> {
    return c * (1.0 / max(1.0 / 32768.0, 1.0 - max3(c.r, c.g, c.b)));
}

// Luma-times-2 proxy used throughout FSR1.
fn luma2(c: vec3<f32>) -> f32 {
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// FsrEasuSetF: accumulate gradient direction + length for one bilinear corner.
// (a,b,c,d,e are the '+' neighbourhood lumas around 'c'.)
fn easu_set(
    dir: ptr<function, vec2<f32>>,
    len: ptr<function, f32>,
    w: f32,
    l_a: f32,
    l_b: f32,
    l_c: f32,
    l_d: f32,
    l_e: f32,
) {
    let dc = l_d - l_c;
    let cb = l_c - l_b;
    var len_x = max(abs(dc), abs(cb));
    len_x = prx_lo_rcp(len_x);
    let dir_x = l_d - l_b;
    (*dir).x += dir_x * w;
    len_x = clamp(abs(dir_x) * len_x, 0.0, 1.0);
    len_x *= len_x;
    *len += len_x * w;
    let ec = l_e - l_c;
    let ca = l_c - l_a;
    var len_y = max(abs(ec), abs(ca));
    len_y = prx_lo_rcp(len_y);
    let dir_y = l_e - l_a;
    (*dir).y += dir_y * w;
    len_y = clamp(abs(dir_y) * len_y, 0.0, 1.0);
    len_y *= len_y;
    *len += len_y * w;
}

// FsrEasuTapF: one anisotropic lanczos-2-approximation tap.
fn easu_tap(
    ac: ptr<function, vec3<f32>>,
    aw: ptr<function, f32>,
    off: vec2<f32>,
    dir: vec2<f32>,
    len: vec2<f32>,
    lob: f32,
    clp: f32,
    c: vec3<f32>,
) {
    var v: vec2<f32>;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    var d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    var wb = (2.0 / 5.0) * d2 - 1.0;
    var wa = lob * d2 - 1.0;
    wb *= wb;
    wa *= wa;
    wb = (25.0 / 16.0) * wb - (25.0 / 16.0 - 1.0);
    let w = wb * wa;
    *ac += c * w;
    *aw += w;
}

@compute @workgroup_size(8, 8)
fn easu(@builtin(global_invocation_id) gid: vec3<u32>) {
    let out_dims = textureDimensions(dst);
    if (gid.x >= out_dims.x || gid.y >= out_dims.y) {
        return;
    }
    // Position of 'f' (top-left of the centre 2x2) in input-viewport space.
    var pp = vec2<f32>(vec2<u32>(gid.xy)) * params.con0.xy + params.con0.zw;
    let fp = floor(pp);
    pp -= fp;

    // The 12-tap kernel's four gather quads (see FsrEasuCon's diagram).
    let p0 = fp * params.con1.xy + params.con1.zw;
    let p1 = p0 + params.con2.xy;
    let p2 = p0 + params.con2.zw;
    let p3 = p0 + params.con3.xy;
    let bczz_r = textureGather(0, src, samp, p0);
    let bczz_g = textureGather(1, src, samp, p0);
    let bczz_b = textureGather(2, src, samp, p0);
    let ijfe_r = textureGather(0, src, samp, p1);
    let ijfe_g = textureGather(1, src, samp, p1);
    let ijfe_b = textureGather(2, src, samp, p1);
    let klhg_r = textureGather(0, src, samp, p2);
    let klhg_g = textureGather(1, src, samp, p2);
    let klhg_b = textureGather(2, src, samp, p2);
    let zzon_r = textureGather(0, src, samp, p3);
    let zzon_g = textureGather(1, src, samp, p3);
    let zzon_b = textureGather(2, src, samp, p3);

    // Texel-major reconstruction, SRTM-tonemapped on load (FSR1 wants [0,1] input).
    let t_b = srtm(vec3(bczz_r.x, bczz_g.x, bczz_b.x));
    let t_c = srtm(vec3(bczz_r.y, bczz_g.y, bczz_b.y));
    let t_i = srtm(vec3(ijfe_r.x, ijfe_g.x, ijfe_b.x));
    let t_j = srtm(vec3(ijfe_r.y, ijfe_g.y, ijfe_b.y));
    let t_f = srtm(vec3(ijfe_r.z, ijfe_g.z, ijfe_b.z));
    let t_e = srtm(vec3(ijfe_r.w, ijfe_g.w, ijfe_b.w));
    let t_k = srtm(vec3(klhg_r.x, klhg_g.x, klhg_b.x));
    let t_l = srtm(vec3(klhg_r.y, klhg_g.y, klhg_b.y));
    let t_h = srtm(vec3(klhg_r.z, klhg_g.z, klhg_b.z));
    let t_g = srtm(vec3(klhg_r.w, klhg_g.w, klhg_b.w));
    let t_o = srtm(vec3(zzon_r.z, zzon_g.z, zzon_b.z));
    let t_n = srtm(vec3(zzon_r.w, zzon_g.w, zzon_b.w));

    let b_l = luma2(t_b);
    let c_l = luma2(t_c);
    let i_l = luma2(t_i);
    let j_l = luma2(t_j);
    let f_l = luma2(t_f);
    let e_l = luma2(t_e);
    let k_l = luma2(t_k);
    let l_l = luma2(t_l);
    let h_l = luma2(t_h);
    let g_l = luma2(t_g);
    let o_l = luma2(t_o);
    let n_l = luma2(t_n);

    // Direction/length accumulated at the four bilinear corners.
    var dir = vec2<f32>(0.0);
    var len = 0.0;
    easu_set(&dir, &len, (1.0 - pp.x) * (1.0 - pp.y), b_l, e_l, f_l, g_l, j_l);
    easu_set(&dir, &len, pp.x * (1.0 - pp.y), c_l, f_l, g_l, h_l, k_l);
    easu_set(&dir, &len, (1.0 - pp.x) * pp.y, f_l, i_l, j_l, k_l, n_l);
    easu_set(&dir, &len, pp.x * pp.y, g_l, j_l, k_l, l_l, o_l);

    // Normalise direction (with zero-gradient cleanup), shape length.
    let dir2 = dir * dir;
    var dir_r = dir2.x + dir2.y;
    let zro = dir_r < (1.0 / 32768.0);
    dir_r = prx_lo_rsq(dir_r);
    dir_r = select(dir_r, 1.0, zro);
    dir.x = select(dir.x, 1.0, zro);
    dir *= vec2(dir_r);
    len = len * 0.5;
    len *= len;
    let stretch = (dir.x * dir.x + dir.y * dir.y) * prx_lo_rcp(max(abs(dir.x), abs(dir.y)));
    let len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
    let lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    let clp = prx_lo_rcp(lob);

    // Dering window = min/max of the nearest 2x2 (f, g, j, k).
    let min4 = min(min3v(t_f, t_g, t_j), t_k);
    let max4 = max(max3v(t_f, t_g, t_j), t_k);

    var ac = vec3<f32>(0.0);
    var aw = 0.0;
    easu_tap(&ac, &aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, t_b);
    easu_tap(&ac, &aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, t_c);
    easu_tap(&ac, &aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, t_i);
    easu_tap(&ac, &aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, t_j);
    easu_tap(&ac, &aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, t_f);
    easu_tap(&ac, &aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, t_e);
    easu_tap(&ac, &aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, t_k);
    easu_tap(&ac, &aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, t_l);
    easu_tap(&ac, &aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, t_h);
    easu_tap(&ac, &aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, t_g);
    easu_tap(&ac, &aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, t_o);
    easu_tap(&ac, &aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, t_n);

    let pix = min(max4, max(min4, ac * vec3(1.0 / aw)));
    textureStore(dst, vec2<i32>(gid.xy), vec4(pix, 1.0));
}

// ---------------------------------------------------------------------------------
// RCAS. Reads the EASU intermediate (tonemapped domain) by texel, sharpens, inverts
// SRTM back to linear HDR on store. FSR_RCAS_LIMIT = 0.25 - 1/16 (AMD default).

const RCAS_LIMIT: f32 = 0.25 - (1.0 / 16.0);

// Sharpen-only variant for the post-DLSS pass: the input is LINEAR HDR (not the
// EASU intermediate), so every tap is SRTM-tonemapped on load and the result is
// inverted on store — same maths, different domain wrapper.
@compute @workgroup_size(8, 8)
fn rcas_hdr(@builtin(global_invocation_id) gid: vec3<u32>) {
    let out_dims = textureDimensions(dst);
    if (gid.x >= out_dims.x || gid.y >= out_dims.y) {
        return;
    }
    let sp = vec2<i32>(gid.xy);
    let dims = vec2<i32>(textureDimensions(src));
    let cb = clamp(sp + vec2(0, -1), vec2(0), dims - 1);
    let cd = clamp(sp + vec2(-1, 0), vec2(0), dims - 1);
    let cf = clamp(sp + vec2(1, 0), vec2(0), dims - 1);
    let ch = clamp(sp + vec2(0, 1), vec2(0), dims - 1);
    let b = srtm(textureLoad(src, cb, 0).rgb);
    let d = srtm(textureLoad(src, cd, 0).rgb);
    let e = srtm(textureLoad(src, sp, 0).rgb);
    let f = srtm(textureLoad(src, cf, 0).rgb);
    let h = srtm(textureLoad(src, ch, 0).rgb);
    let mn4 = min(min3v(b, d, f), h);
    let mx4 = max(max3v(b, d, f), h);
    let hit_min = mn4 / (4.0 * mx4);
    let hit_max = (vec3(1.0) - mx4) / (4.0 * mn4 - 4.0);
    let lobe3 = max(-hit_min, hit_max);
    let lobe = max(-RCAS_LIMIT, min(max3(lobe3.r, lobe3.g, lobe3.b), 0.0)) * params.rcas.x;
    let rcp_l = prx_med_rcp(4.0 * lobe + 1.0);
    let pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * vec3(rcp_l);
    textureStore(dst, sp, vec4(srtm_inv(pix), 1.0));
}

@compute @workgroup_size(8, 8)
fn rcas(@builtin(global_invocation_id) gid: vec3<u32>) {
    let out_dims = textureDimensions(dst);
    if (gid.x >= out_dims.x || gid.y >= out_dims.y) {
        return;
    }
    let sp = vec2<i32>(gid.xy);
    let dims = vec2<i32>(textureDimensions(src));
    let cb = clamp(sp + vec2(0, -1), vec2(0), dims - 1);
    let cd = clamp(sp + vec2(-1, 0), vec2(0), dims - 1);
    let cf = clamp(sp + vec2(1, 0), vec2(0), dims - 1);
    let ch = clamp(sp + vec2(0, 1), vec2(0), dims - 1);
    let b = textureLoad(src, cb, 0).rgb;
    let d = textureLoad(src, cd, 0).rgb;
    let e = textureLoad(src, sp, 0).rgb;
    let f = textureLoad(src, cf, 0).rgb;
    let h = textureLoad(src, ch, 0).rgb;

    // Min/max of the cross ring, per channel.
    let mn4 = min(min3v(b, d, f), h);
    let mx4 = max(max3v(b, d, f), h);
    // Limiters (high-precision reciprocals, as the reference requires).
    let hit_min = mn4 / (4.0 * mx4);
    let hit_max = (vec3(1.0) - mx4) / (4.0 * mn4 - 4.0);
    let lobe3 = max(-hit_min, hit_max);
    let lobe = max(-RCAS_LIMIT, min(max3(lobe3.r, lobe3.g, lobe3.b), 0.0)) * params.rcas.x;
    // Resolve with the medium-precision rcp approximation (tonality-safe).
    let rcp_l = prx_med_rcp(4.0 * lobe + 1.0);
    let pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * vec3(rcp_l);
    textureStore(dst, sp, vec4(srtm_inv(pix), 1.0));
}
