// Projects the sky reflection env map (equirect, linear radiance) into 9 spherical-harmonic RGB
// coefficients for diffuse sky irradiance (object + terrain ambient — water look plan Stage 4a
// follow-up). Once per frame, right after the env bake. The lit shaders evaluate irradiance(n)
// from these coeffs — see frame::sky_irradiance.
//
// THIS RAN ON ONE THREAD UNTIL 2026-08-31, and the comment here said "a single serial pass is
// simplest + trivially correct (the pass is tiny)". It was not tiny. PERF-010 gave the
// atmosphere its first timer regions and this pass measured **1.812 ms on perf_field at
// 1600x900 — 16% of the whole GPU frame** — to reduce a coarse 128x64 grid to nine
// coefficients, because the dispatch was `(1, 1, 1)` against `@workgroup_size(1)`: one lane on
// a GPU with thousands. Serial was not the wrong call because it was serial; it was the wrong
// call because nobody had measured it, and the code asserted the answer instead.
//
// Now 64 lanes and a tree reduction in workgroup memory. The MATH IS UNCHANGED: same coarse
// grid, same sample positions, same solid angle, same basis constants. Only the order of
// summation differs, so the result moves by float-addition-order noise and no more — which is
// the property to preserve, because these nine numbers are the ambient term of every lit
// surface in the world and a real change to them would be visible everywhere at once.

@group(0) @binding(0) var env: texture_2d<f32>;

struct Sh {
    // First nine remain the existing full-sphere surface radiance. The next
    // nine project only the upper sky for fog, without inventing ground light.
    c: array<vec4<f32>, 18>,
};
@group(0) @binding(1) var<storage, read_write> sh: Sh;

// RFG-186: what the lower hemisphere radiates. The env map below the horizon only continues the
// horizon sky, so every surface facing sideways or down was lit from BELOW by blue sky -- shade
// read cold and dark (a wall in shade: ours 48/57/72, Reforger's 84/80/69, warm from the sunlit
// ground in front of it). With `albedo.w` > 0.5 the lower half is replaced by the ground as a
// Lambert reflector: albedo x (sky irradiance on it + the sun's) / pi. The sun term arrives already
// dimmed by cloud cover (EngineWgpu cloudDim), so an overcast sky moves light from the beam into
// this soft fill instead of losing it.
struct Ground {
    sun: vec4<f32>,    // rgb = frame sun_diffuse (E / pi), w = sun direction y (up = 1)
    albedo: vec4<f32>, // rgb = linear ground albedo, w = enable
    // RFG-196: x = the indirect/direct illuminance ratio a native world asks for, y = enable,
    // z = the largest lift allowed, w = how much of the ADDED light is desaturated (Lighting_Eden's
    // indirect colour is white: lifting a blue sky as-is turns every sunlit road blue-white). Reforger's Lighting_Eden.et: DirectLightLV 15, IndirectLightLV
    // 14 -> 0.5. The sky's irradiance on level ground is lifted (never lowered) to that share of the
    // sun's beam; ours sat near 3% (a shaded pavement 5.2 EV under the sunlit road beside it where
    // Reforger shows 1.7 EV, matched pose shadowProbe).
    norm: vec4<f32>,
    // RFG-200: x = 1 / the native dome scale (EngineWgpu WGR_NATIVE_SKY_SCALE, 2.5). The dome is
    // brightened to Reforger's pale sky, but the LIGHT it casts must stay the physical sky's:
    // projected at 2.5x, the sky's own blue outweighed the neutral lift below and tinted every
    // crown (forestOwner blue 52 vs 44, +1.13 vs +0.91 EV). 1 elsewhere.
    phys: vec4<f32>,
};
@group(0) @binding(2) var<uniform> ground: Ground;

const PI: f32 = 3.14159265359;
const LANES: u32 = 64u;

// Per-lane partial sums. vec3 aligns to 16 bytes in workgroup address space, so this is
// 64 * 9 * 16 = 9216 bytes -- inside the 16 KiB every WebGPU device guarantees.
var<workgroup> partial: array<array<vec3<f32>, 9>, LANES>;

@compute @workgroup_size(64)
fn cs_sky_sh(@builtin(local_invocation_index) lane: u32) {
    let dims = vec2<i32>(textureDimensions(env));
    let w = dims.x;
    let h = dims.y;
    // Coarse integration grid (cap the sample count; SH-9 needs no more).
    let sx = max(w / 128, 1);
    let sy = max(h / 64, 1);
    let dphi = 2.0 * PI / f32(w);
    let dtheta = PI / f32(h);

    var c: array<vec3<f32>, 9>;
    var upper: array<vec3<f32>, 9>;
    for (var i = 0; i < 9; i = i + 1) {
        c[i] = vec3<f32>(0.0);
        upper[i] = vec3<f32>(0.0);
    }

    // Each lane owns a stride of ROWS. The row grid is h/sy steps, which is 64 whenever the env
    // is at least 64 texels tall (the case in every configuration this ships with), so this is
    // one row per lane; the loop handles a taller grid and the bounds test handles a shorter
    // one. Rows are the right axis to split on: a lane then walks a contiguous span of x, which
    // keeps the texture reads coalesced exactly as the serial version's inner loop did.
    var y = i32(lane) * sy;
    loop {
        if (y >= h) { break; }
        // Texel-centre polar angle (v = 0 at zenith .. 1 at nadir), matching fs_sky_env.
        let v = (f32(y) + 0.5) / f32(h);
        let polar = v * PI;
        let sp = sin(polar);
        let cp = cos(polar);
        // Solid angle of the (sx x sy) cell this sample represents.
        let domega = sp * dtheta * dphi * f32(sx) * f32(sy);
        var x = 0;
        loop {
            if (x >= w) { break; }
            let u = (f32(x) + 0.5) / f32(w);
            let azimuth = (u - 0.5) * 2.0 * PI;
            let dir = vec3<f32>(sp * cos(azimuth), cp, sp * sin(azimuth));
            let rad = textureLoad(env, vec2<i32>(x, y), 0).rgb;
            // RFG-186: below the horizon the ground stands in, added analytically after the sum.
            let wr = select(rad * domega, vec3<f32>(0.0), ground.albedo.w > 0.5 && dir.y < 0.0);
            c[0] = c[0] + wr * 0.282095;
            c[1] = c[1] + wr * 0.488603 * dir.y;
            c[2] = c[2] + wr * 0.488603 * dir.z;
            c[3] = c[3] + wr * 0.488603 * dir.x;
            c[4] = c[4] + wr * 1.092548 * dir.x * dir.y;
            c[5] = c[5] + wr * 1.092548 * dir.y * dir.z;
            c[6] = c[6] + wr * 0.315392 * (3.0 * dir.z * dir.z - 1.0);
            c[7] = c[7] + wr * 1.092548 * dir.x * dir.z;
            c[8] = c[8] + wr * 0.546274 * (dir.x * dir.x - dir.y * dir.y);
            if (dir.y > 0.0) {
                upper[0] += wr * 0.282095;
                upper[1] += wr * 0.488603 * dir.y;
                upper[2] += wr * 0.488603 * dir.z;
                upper[3] += wr * 0.488603 * dir.x;
                upper[4] += wr * 1.092548 * dir.x * dir.y;
                upper[5] += wr * 1.092548 * dir.y * dir.z;
                upper[6] += wr * 0.315392 * (3.0 * dir.z * dir.z - 1.0);
                upper[7] += wr * 1.092548 * dir.x * dir.z;
                upper[8] += wr * 0.546274 * (dir.x * dir.x - dir.y * dir.y);
            }
            x = x + sx;
        }
        y = y + i32(LANES) * sy;
    }

    for (var i = 0; i < 9; i = i + 1) {
        partial[lane][i] = c[i];
    }
    workgroupBarrier();

    // Tree reduction. The barrier is OUTSIDE the `if`: in WGSL every invocation in the
    // workgroup must reach the same barrier, and guarding it would be undefined behaviour on
    // the lanes that skipped it -- a class of bug that usually shows up as one vendor's driver
    // hanging and another's working.
    var stride: u32 = LANES / 2u;
    loop {
        if (stride == 0u) { break; }
        if (lane < stride) {
            for (var i = 0; i < 9; i = i + 1) {
                partial[lane][i] = partial[lane][i] + partial[lane + stride][i];
            }
        }
        workgroupBarrier();
        stride = stride / 2u;
    }

    var full: array<vec3<f32>, 9>;
    if (lane == 0u) {
        for (var i = 0; i < 9; i = i + 1) {
            full[i] = partial[0][i];
        }
    }
    // Reuse the 9 KiB scratch instead of doubling workgroup memory beyond the
    // WebGPU 16 KiB guarantee. Both projections share the same environment read.
    workgroupBarrier();
    for (var i = 0; i < 9; i = i + 1) { partial[lane][i] = upper[i]; }
    workgroupBarrier();
    stride = LANES / 2u;
    loop {
        if (stride == 0u) { break; }
        if (lane < stride) {
            for (var i = 0; i < 9; i = i + 1) {
                partial[lane][i] += partial[lane + stride][i];
            }
        }
        workgroupBarrier();
        stride = stride / 2u;
    }
    if (lane == 0u) {
        for (var i = 0; i < 9; i = i + 1) { sh.c[i + 9] = vec4<f32>(partial[0][i], 0.0); }
        // Sky irradiance on level ground from the upper-sky coefficients (frame::sky_irradiance
        // at n = +y, before its 1/pi).
        for (var i = 0; i < 9; i = i + 1) { full[i] = full[i] * ground.phys.x; }
        let u = partial[0];
        var e_sky = ground.phys.x * max(-0.429043 * u[8] + 0.886227 * u[0] - 0.247708 * u[6] + 2.0 * 0.511664 * u[1],
                        vec3<f32>(0.0));
        // RFG-196: lift the sky (the full set, before the ground is added) to the native world's
        // indirect share of the sun's beam (luminance), keeping its colour and its direction.
        if (ground.norm.y > 0.5) {
            let lum = vec3<f32>(0.2126, 0.7152, 0.0722);
            let e_sun_beam = dot(ground.sun.rgb * PI, lum);
            let e_sky_lum = dot(e_sky, lum);
            if (e_sun_beam > 0.0 && e_sky_lum > 1.0e-6) {
                let lift = clamp(ground.norm.x * e_sun_beam / e_sky_lum, 1.0, max(ground.norm.z, 1.0));
                // Desaturation is linear, so it applies per coefficient and keeps the SH a projection.
                let k = lift - 1.0;
                for (var i = 0; i < 9; i = i + 1) {
                    let c = full[i];
                    full[i] = c + k * mix(c, vec3<f32>(dot(c, lum)), ground.norm.w);
                }
                e_sky = e_sky + k * mix(e_sky, vec3<f32>(e_sky_lum), ground.norm.w);
            }
        }
        if (ground.albedo.w > 0.5) {
            // Plus the sun's irradiance, then the ground's Lambert radiance.
            let e_sun = ground.sun.rgb * PI * max(ground.sun.w, 0.0);
            let lg = ground.albedo.rgb * (e_sky + e_sun) / PI;
            // A constant radiance over the lower hemisphere projects onto Y00 and Y10 only:
            // integral of Y00 = 0.282095 * 2 pi, integral of Y10 (0.488603 y) = -0.488603 * pi.
            full[0] = full[0] + lg * (0.282095 * 2.0 * PI);
            full[1] = full[1] - lg * (0.488603 * PI);
        }
        for (var i = 0; i < 9; i = i + 1) { sh.c[i] = vec4<f32>(full[i], 0.0); }
    }
}
