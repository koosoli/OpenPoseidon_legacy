// REN-GI-001/002 — the irradiance probe volume (docs/roadmap/decisions/REN-GI-00*.md).
//
// A world-anchored grid of probes around the camera (32 x 8 x 32, `spacing` metres apart),
// each holding an AMBIENT CUBE: irradiance/pi for the six axis directions. Every frame one
// eighth of the probes are re-integrated; the rays are answered by the scene proxies the
// renderer already owns, not by a ray tracer:
//
//   * the terrain heightfield (group 1, the conform bind group's `hm`): a ground hit is
//     radiance = ground albedo x (sun x sun visibility x cos + sky irradiance);
//   * the interior sky-visibility dome maps (frame.skyvis_*): an upward ray that the dome
//     says is roofed hits a wall/roof of neutral albedo, lit by the sky from the open side;
//   * otherwise the ray sees the sky: SH-9 sky radiance in that direction;
//
// and, stage 2 (REN-GI-002), the SUN PROXY: a reflective shadow map rendered from the sun over
// the same box (depth + albedo + normal of every sunlit surface, walls and trees included). Each
// probe gathers the first sun bounce from a disc of its texels -- the coloured light a sunlit
// wall or a bright field throws into the shade next to it.
//
// Direct sun at the shaded pixel is NOT here (the cascade shadow maps do that per pixel); this
// is the indirect + sky term, which shade() blends over its analytic sky-dome ambient. The
// integration accumulates with hysteresis into a storage buffer and mirrors the result into a
// sampled 3D texture (six y-slabs of 8 probes: slab k = axis k) for the fragment shaders.
#import frame::{frame, sky_sh, sky_irradiance, terrain_sun_shadow, cloud_sun_shadow, interior_sky_reach_dir, sky_vis_ao}

struct HmParams {
    origin: vec2<f32>,
    terrain_grid: f32,
    enabled: f32,
    hm_width: u32,
    hm_height: u32,
    cdlod_base: f32,
    cdlod_ratio: f32,
    cdlod_morph: f32,
    cdlod_mode: f32,
};
@group(1) @binding(0) var hm: texture_2d<f32>;
@group(1) @binding(1) var<uniform> hm_params: HmParams;

struct GiParams {
    origin: vec4<f32>,        // xyz = world position of probe (0,0,0); w = spacing (m)
    dims: vec4<f32>,          // x,y,z = probe counts; w = this frame's batch index (0..7)
    knobs: vec4<f32>,         // x = hysteresis, y = ground gain, z = wall albedo, w = rays
    ground: vec4<f32>,        // rgb = ground albedo, w = ray length (m)
    reset: vec4<f32>,         // x = 1 when the volume moved this frame (history invalid)
    rsm_vp: mat4x4<f32>,      // ABSOLUTE world -> sun proxy clip (ortho)
    rsm_inv_vp: mat4x4<f32>,  // sun proxy clip -> absolute world
    rsm: vec4<f32>,           // x = gather radius (m), y = samples, z = gain, w = valid (0/1)
    rsm_meta: vec4<f32>,      // x = texel size (m), y = resolution, z,w = unused
};
@group(2) @binding(0) var<uniform> gi: GiParams;
// Twelve vec4 per probe: 0..5 irradiance/pi for +X -X +Y -Y +Z -Z (.a of entry 0 = 1 once
// written), 6..11 the matching distance moments (mean r, mean r^2) for the visibility test.
@group(2) @binding(1) var<storage, read_write> state: array<vec4<f32>>;
@group(2) @binding(2) var probes_out: texture_storage_3d<rgba16float, write>;
// REN-GI-003: per-face distance moments (mean r, mean r^2) of what each probe's rays hit.
// The shading uses them for a Chebyshev visibility test between probe and pixel, which is
// what stops a lit outdoor probe bleeding through a wall into a room.
@group(2) @binding(6) var dist_out: texture_storage_3d<rgba16float, write>;
// The sun proxy (REN-GI-002): depth (forward-Z, 0..1), albedo (a = 1 where something was
// drawn), normal packed n * 0.5 + 0.5.
@group(2) @binding(3) var rsm_depth: texture_depth_2d;
@group(2) @binding(4) var rsm_albedo: texture_2d<f32>;
@group(2) @binding(5) var rsm_normal: texture_2d<f32>;

const PI: f32 = 3.14159265359;
const BATCHES: u32 = 8u;

fn hm_load(ix: i32, iz: i32) -> f32 {
    let cx = clamp(ix, 0, i32(hm_params.hm_width) - 1);
    let cz = clamp(iz, 0, i32(hm_params.hm_height) - 1);
    return textureLoad(hm, vec2<i32>(cx, cz), 0).x;
}

fn ground_height(world_xz: vec2<f32>) -> f32 {
    let t = (world_xz - hm_params.origin) / hm_params.terrain_grid;
    let base = floor(t);
    let ix = i32(base.x);
    let iz = i32(base.y);
    let f = t - base;
    let y00 = hm_load(ix, iz);
    let y01 = hm_load(ix + 1, iz);
    let y10 = hm_load(ix, iz + 1);
    let y11 = hm_load(ix + 1, iz + 1);
    return mix(mix(y00, y01, f.x), mix(y10, y11, f.x), f.y);
}

fn ground_normal(world_xz: vec2<f32>) -> vec3<f32> {
    let e = hm_params.terrain_grid;
    let hx = ground_height(world_xz + vec2<f32>(e, 0.0)) - ground_height(world_xz - vec2<f32>(e, 0.0));
    let hz = ground_height(world_xz + vec2<f32>(0.0, e)) - ground_height(world_xz - vec2<f32>(0.0, e));
    return normalize(vec3<f32>(-hx, 2.0 * e, -hz));
}

// SH-9 sky RADIANCE in direction d (the plain basis; sky_irradiance is the cosine-convolved
// version of the same coefficients).
fn sky_radiance(d: vec3<f32>) -> vec3<f32> {
    let x = d.x;
    let y = d.y;
    let z = d.z;
    var r = sky_sh.c[0].rgb * 0.282095;
    r += sky_sh.c[1].rgb * (0.488603 * y);
    r += sky_sh.c[2].rgb * (0.488603 * z);
    r += sky_sh.c[3].rgb * (0.488603 * x);
    r += sky_sh.c[4].rgb * (1.092548 * x * y);
    r += sky_sh.c[5].rgb * (1.092548 * y * z);
    r += sky_sh.c[6].rgb * (0.315392 * (3.0 * z * z - 1.0));
    r += sky_sh.c[7].rgb * (1.092548 * x * z);
    r += sky_sh.c[8].rgb * (0.546274 * (x * x - y * y));
    return max(r, vec3<f32>(0.0));
}

// Sun visibility at a world point from the two long-range proxies (terrain ceiling mask and
// cloud transmittance); the cascade maps are per-pixel work and too short-ranged to matter here.
fn sun_visibility(p: vec3<f32>) -> f32 {
    return (1.0 - terrain_sun_shadow(p.xz, p.y)) * cloud_sun_shadow(p.xz);
}

fn hash11(n: f32) -> f32 {
    return fract(sin(n * 12.9898) * 43758.5453);
}

// Fibonacci-sphere direction i of K, rotated about Y by `rot` so consecutive updates of a
// probe see different directions and the hysteresis averages them.
fn fib_dir(i: u32, k: u32, rot: f32) -> vec3<f32> {
    let fi = f32(i) + 0.5;
    let y = 1.0 - 2.0 * fi / f32(k);
    let r = sqrt(max(0.0, 1.0 - y * y));
    let phi = fi * 2.399963 + rot;
    return vec3<f32>(r * cos(phi), y, r * sin(phi));
}

// Radiance arriving at probe p from direction d (sky and the two occluder proxies), and the
// distance to what it hit (a large value when it saw the sky).
fn ray_radiance(p: vec3<f32>, d: vec3<f32>) -> vec4<f32> {
    let ray_len = gi.ground.w;
    // 1. Terrain: exponentially spaced march against the heightfield.
    if (hm_params.enabled > 0.5 && d.y < 0.95) {
        var t_prev = 0.0;
        for (var s = 1; s <= 12; s++) {
            let t = ray_len * pow(f32(s) / 12.0, 2.0);
            let q = p + d * t;
            let h = ground_height(q.xz);
            if (q.y < h) {
                let tm = 0.5 * (t_prev + t);
                let qm = p + d * tm;
                let hm_mid = ground_height(qm.xz);
                var hit = q;
                if (qm.y < hm_mid) {
                    hit = qm;
                }
                let n = ground_normal(hit.xz);
                let sun = normalize(frame.sun_dir_world.xyz);
                let sun_l = frame.sun_diffuse.rgb * sun_visibility(hit) * max(dot(n, sun), 0.0) * gi.knobs.y;
                let sky_l = sky_irradiance(n) * sky_vis_ao(hit.xz);
                return vec4<f32>(gi.ground.rgb * (sun_l + sky_l), length(hit - p));
            }
            t_prev = t;
        }
    }
    // 2. Roof / wall from the dome maps: pick the sampled direction nearest to d.
    if (frame.skyvis.x > 0.5 && d.y > 0.1) {
        var best = 0;
        var best_dot = -1.0;
        for (var i = 0; i < 5; i++) {
            let dd = dot(d, frame.skyvis_dir[i].xyz);
            if (dd > best_dot) {
                best_dot = dd;
                best = i;
            }
        }
        let reach = interior_sky_reach_dir(p, d, best);
        if (reach < 0.5) {
            // A roof or canopy: the dome map gives no distance, so charge it half the ray
            // length -- near enough for the visibility test, and never zero (which would read
            // as "this probe is inside something solid").
            return vec4<f32>(vec3<f32>(gi.knobs.z) * sky_irradiance(-d) * 0.5, ray_len * 0.5);
        }
    }
    // 3. Sky: nothing in the way, so the distance is "far" for the visibility test.
    return vec4<f32>(sky_radiance(d), ray_len * 4.0);
}

// Blend one face into the state buffer and mirror it into the sampled texture's slab. The
// state holds 12 vec4 per probe: 0..5 radiance, 6..11 the distance moments.
fn store_face(base: u32, f: u32, cur: vec3<f32>, alpha: f32, px: u32, py: u32, pz: u32, ny: u32) {
    let old = state[base + f].rgb;
    let v = mix(old, cur, alpha);
    state[base + f] = vec4<f32>(v, 1.0);
    textureStore(probes_out, vec3<i32>(i32(px), i32(py + f * ny), i32(pz)), vec4<f32>(v, 1.0));
}

fn store_moments(base: u32, f: u32, mean: f32, mean2: f32, alpha: f32,
                 px: u32, py: u32, pz: u32, ny: u32) {
    let old = state[base + 6u + f].xy;
    let v = mix(old, vec2<f32>(mean, mean2), alpha);
    state[base + 6u + f] = vec4<f32>(v.x, v.y, 0.0, 1.0);
    textureStore(dist_out, vec3<i32>(i32(px), i32(py + f * ny), i32(pz)),
                 vec4<f32>(v.x, v.y, 0.0, 1.0));
}

// Vogel disc sample i of n, rotated by `rot`; unit radius.
fn vogel(i: u32, n: u32, rot: f32) -> vec2<f32> {
    let r = sqrt((f32(i) + 0.5) / f32(n));
    let a = f32(i) * 2.399963 + rot;
    return vec2<f32>(r * cos(a), r * sin(a));
}

@compute @workgroup_size(64)
fn cs_gi_probes(@builtin(global_invocation_id) gid: vec3<u32>) {
    let nx = u32(gi.dims.x);
    let ny = u32(gi.dims.y);
    let nz = u32(gi.dims.z);
    let total = nx * ny * nz;
    let per_batch = (total + BATCHES - 1u) / BATCHES;
    let batch = u32(gi.dims.w);
    let idx = batch * per_batch + gid.x;
    if (gid.x >= per_batch || idx >= total) {
        return;
    }
    let px = idx % nx;
    let py = (idx / nx) % ny;
    let pz = idx / (nx * ny);
    let spacing = gi.origin.w;
    let p = gi.origin.xyz + (vec3<f32>(f32(px), f32(py), f32(pz)) + vec3<f32>(0.5, 0.5, 0.5)) * spacing;

    let k = max(u32(gi.knobs.w), 8u);
    let rot = hash11(f32(idx) + 17.0 * gi.dims.w) * 6.2831853;
    var e0 = vec3<f32>(0.0);
    var e1 = vec3<f32>(0.0);
    var e2 = vec3<f32>(0.0);
    var e3 = vec3<f32>(0.0);
    var e4 = vec3<f32>(0.0);
    var e5 = vec3<f32>(0.0);
    // REN-GI-003: per-face distance moments, weighted by the same cosine. Plain scalars rather
    // than an array of vectors: naga's backend refuses a local array of vectors here.
    var r0 = 0.0; var r1 = 0.0; var r2 = 0.0; var r3 = 0.0; var r4 = 0.0; var r5 = 0.0;
    var q0 = 0.0; var q1 = 0.0; var q2 = 0.0; var q3 = 0.0; var q4 = 0.0; var q5 = 0.0;
    var c0 = 0.0; var c1 = 0.0; var c2 = 0.0; var c3 = 0.0; var c4 = 0.0; var c5 = 0.0;
    let w = 4.0 * PI / f32(k);
    for (var i = 0u; i < k; i++) {
        let d = fib_dir(i, k, rot);
        let ray = ray_radiance(p, d);
        let l = ray.rgb * w;
        let dist = ray.w;
        let dsq = dist * dist;
        let wx0 = max(d.x, 0.0);
        let wx1 = max(-d.x, 0.0);
        let wy0 = max(d.y, 0.0);
        let wy1 = max(-d.y, 0.0);
        let wz0 = max(d.z, 0.0);
        let wz1 = max(-d.z, 0.0);
        e0 += l * wx0;
        e1 += l * wx1;
        e2 += l * wy0;
        e3 += l * wy1;
        e4 += l * wz0;
        e5 += l * wz1;
        r0 += dist * wx0;
        r1 += dist * wx1;
        r2 += dist * wy0;
        r3 += dist * wy1;
        r4 += dist * wz0;
        r5 += dist * wz1;
        q0 += dsq * wx0;
        q1 += dsq * wx1;
        q2 += dsq * wy0;
        q3 += dsq * wy1;
        q4 += dsq * wz0;
        q5 += dsq * wz1;
        c0 += wx0;
        c1 += wx1;
        c2 += wy0;
        c3 += wy1;
        c4 += wz0;
        c5 += wz1;
    }

    // REN-GI-002: the first sun bounce from the sun proxy. Each sample is a sunlit surface
    // element of area A = pi r^2 / n around the probe's projection; its radiance toward the
    // probe is albedo * sun * cos(theta_sun) / pi, and it subtends A cos(theta_q) / d^2.
    if (gi.rsm.w > 0.5) {
        let clip = gi.rsm_vp * vec4<f32>(p, 1.0);
        let uv = vec2<f32>(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
        if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0) {
            let res = gi.rsm_meta.y;
            let texel_m = max(gi.rsm_meta.x, 1e-3);
            let radius_px = gi.rsm.x / texel_m;
            let n = max(u32(gi.rsm.y), 4u);
            let area = PI * gi.rsm.x * gi.rsm.x / f32(n);
            let sun = normalize(frame.sun_dir_world.xyz);
            let sun_col = frame.sun_diffuse.rgb * gi.rsm.z;
            let rot2 = hash11(f32(idx) * 0.37 + 5.0 * gi.dims.w) * 6.2831853;
            for (var s = 0u; s < n; s++) {
                let o = vogel(s, n, rot2) * radius_px;
                let tc = vec2<i32>(clamp(uv * res + o, vec2<f32>(0.0), vec2<f32>(res - 1.0)));
                let alb = textureLoad(rsm_albedo, tc, 0);
                if (alb.a < 0.5) {
                    continue;
                }
                let depth = textureLoad(rsm_depth, tc, 0);
                let ndc = vec2<f32>((f32(tc.x) + 0.5) / res * 2.0 - 1.0, 1.0 - (f32(tc.y) + 0.5) / res * 2.0);
                let wq = gi.rsm_inv_vp * vec4<f32>(ndc, depth, 1.0);
                let q = wq.xyz / wq.w;
                let nq = normalize(textureLoad(rsm_normal, tc, 0).xyz * 2.0 - 1.0);
                let dv = p - q;
                let d2 = max(dot(dv, dv), 0.25);
                let d = sqrt(d2);
                let dir = dv / d;
                let cos_q = max(dot(nq, dir), 0.0);
                let cos_sun = max(dot(nq, sun), 0.0);
                if (cos_q <= 0.0 || cos_sun <= 0.0) {
                    continue;
                }
                // radiance * solid angle (the receiver cosine is applied per face below)
                let l = alb.rgb * sun_col * cos_sun / PI * (area * cos_q / d2);
                // Radiance only: the moments describe the GEOMETRY the rays met, and a
                // gathered bounce is not a ray hit.
                let back = -dir; // direction from the probe toward the sample
                if (gi.rsm_meta.z > 0.5) {
                    // REN-GI-007 ablation: same energy, spread over every face. If a shaded
                    // wall lifts under this and not under the directional store, the bounce
                    // is present and simply lands in a face a vertical normal cannot read.
                    let f = l * (1.0 / 3.0);
                    e0 += f; e1 += f; e2 += f; e3 += f; e4 += f; e5 += f;
                } else {
                    e0 += l * max(back.x, 0.0);
                    e1 += l * max(-back.x, 0.0);
                    e2 += l * max(back.y, 0.0);
                    e3 += l * max(-back.y, 0.0);
                    e4 += l * max(back.z, 0.0);
                    e5 += l * max(-back.z, 0.0);
                }
            }
        }
    }

    let base = idx * 12u;
    let had = state[base].a;
    var alpha = clamp(gi.knobs.x, 0.05, 1.0);
    if (had < 0.5 || gi.reset.x > 0.5) {
        alpha = 1.0;
    }
    store_face(base, 0u, e0 / PI, alpha, px, py, pz, ny);
    store_face(base, 1u, e1 / PI, alpha, px, py, pz, ny);
    store_face(base, 2u, e2 / PI, alpha, px, py, pz, ny);
    store_face(base, 3u, e3 / PI, alpha, px, py, pz, ny);
    store_face(base, 4u, e4 / PI, alpha, px, py, pz, ny);
    store_face(base, 5u, e5 / PI, alpha, px, py, pz, ny);
    store_moments(base, 0u, r0 / max(c0, 1e-4), q0 / max(c0, 1e-4), alpha, px, py, pz, ny);
    store_moments(base, 1u, r1 / max(c1, 1e-4), q1 / max(c1, 1e-4), alpha, px, py, pz, ny);
    store_moments(base, 2u, r2 / max(c2, 1e-4), q2 / max(c2, 1e-4), alpha, px, py, pz, ny);
    store_moments(base, 3u, r3 / max(c3, 1e-4), q3 / max(c3, 1e-4), alpha, px, py, pz, ny);
    store_moments(base, 4u, r4 / max(c4, 1e-4), q4 / max(c4, 1e-4), alpha, px, py, pz, ny);
    store_moments(base, 5u, r5 / max(c5, 1e-4), q5 / max(c5, 1e-4), alpha, px, py, pz, ny);
}
