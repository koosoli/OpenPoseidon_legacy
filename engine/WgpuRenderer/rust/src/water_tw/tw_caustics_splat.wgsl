// Tidewater Native — caustics by photon splatting (W5b): Caustics.js `CausticLayer`
// (dgreenheck/tidewater @ 4811ba48, MIT, © DRG Software Solutions LLC). A grid over one FFT tile
// (plus a margin: the tile repeats) is drawn into the layer's texture: each vertex is a point on
// the wave surface, the sun ray is refracted through its normal and followed down D metres, and the
// vertex is placed where it lands; the fragment writes the area ratio (surface patch / its image),
// the light concentration, with additive blending. R: the shallow focal plane, G: the deep one.
//
// OP: the tile size, depth, grid, margin and cascade come from a uniform (Tidewater bakes them
// into the code), so the live cascade-size scale needs no rebuild.

struct CauParams {
    sun_dir: vec4<f32>, // toward the sun (world), w unused
    p: vec4<f32>,       // tile size (m), focal depth D (m), margin (fraction of the tile), cells across [-margin, 1 + margin]
    q: vec4<f32>,       // cascade, slope mip level, channel (0 = R, 1 = G), unused
};

@group(0) @binding(0) var<uniform> cau: CauParams;
@group(0) @binding(1) var oceanDerivatives: texture_2d_array<f32>;
@group(0) @binding(2) var smpLinearRepeat: sampler;

struct CauOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) vOld: vec2<f32>,
    @location(1) vNew: vec2<f32>,
};

@vertex
fn vs_cau(@builtin(vertex_index) vi: u32) -> CauOut {
    let grid = u32(cau.p.w);
    let L = cau.p.x;
    let D = cau.p.y;
    let margin = cau.p.z;
    let cx = vi % (grid + 1u);
    let cy = vi / (grid + 1u);
    let uv = vec2<f32>(vec2<u32>(cx, cy)) / f32(grid) * (1.0 + 2.0 * margin) - margin;
    let d = textureSampleLevel(oceanDerivatives, smpLinearRepeat, uv, i32(cau.q.x), cau.q.y);
    let s = vec2<f32>(d.x / max(d.z + 1.0, 0.3), d.y / max(d.w + 1.0, 0.3));
    let n = normalize(vec3<f32>(-s.x, 1.0, -s.y));
    let sunDir = cau.sun_dir.xyz;
    let T = refract(-sunDir, n, 1.0 / 1.333);
    let tDown = max(-T.y, 0.15);
    // the flat-surface refraction offset is removed so the pattern stays registered with the entry
    // point (the lookup re-applies it with the real depth)
    let T0 = refract(-sunDir, vec3<f32>(0.0, 1.0, 0.0), 1.0 / 1.333);
    let off = (T.xz / tDown - T0.xz / max(-T0.y, 0.15)) * D;
    let p = uv * L;
    let qq = p + off;
    var o: CauOut;
    o.vOld = p;
    o.vNew = qq;
    let ndc = qq / L * 2.0 - 1.0;
    o.pos = vec4<f32>(ndc.x, ndc.y, 0.0, 1.0);
    return o;
}

@fragment
fn fs_cau(in: CauOut) -> @location(0) vec4<f32> {
    // area ratio between the surface patch and its image on the floor
    let ao = abs(dpdx(in.vOld).x * dpdy(in.vOld).y - dpdx(in.vOld).y * dpdy(in.vOld).x);
    let an = abs(dpdx(in.vNew).x * dpdy(in.vNew).y - dpdx(in.vNew).y * dpdy(in.vNew).x);
    // soft limit: a single nearly-folded cell must not become a flat white hot spot
    let I = ao / max(an + ao * (1.0 / 8.0), 1e-9);
    let ch = cau.q.z;
    return vec4<f32>(select(0.0, I, ch < 0.5), select(0.0, I, ch > 0.5), 0.0, 1.0);
}

// ---- mip chain: 2x2 box of the level above (one bilinear tap at the shared corner)
@group(0) @binding(3) var cauSrc: texture_2d<f32>;
@group(0) @binding(4) var cauSmp: sampler;

struct MipOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_mip(@builtin(vertex_index) vi: u32) -> MipOut {
    let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
    var o: MipOut;
    o.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

@fragment
fn fs_mip(in: MipOut) -> @location(0) vec4<f32> {
    return textureSampleLevel(cauSrc, cauSmp, in.uv, 0.0);
}
