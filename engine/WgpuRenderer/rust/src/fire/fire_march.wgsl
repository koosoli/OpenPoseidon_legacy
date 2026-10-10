// FIRE-001: the fire volumes drawn into the lit scene.
//
// Port of the camera march and shading of dgreenheck/threejs-fire-pro @ c284f0b1 (MIT,
// (c) 2026 Daniel Greenheck; see CREDITS.md): volume-march.wgsl (front-to-back integration with
// the analytic step integral, jittered samples), fire-emission.wgsl (smoke absorption and
// scattering, flame absorption from its glow, Kirchhoff emission j = sigma_a B(T) with the
// temperature from heat and the blackbody table of blackbody.ts) and the sun lobe of
// fireProDirect.
//
// What OP changes: one full-screen pass over up to four dense boxes, nearest first; the march
// stops at OP's reversed-Z scene depth; the smoke is lit by OP's sun and ambient with a short
// march toward the sun through the same box for its self-shadow (Fire Pro: a light volume).

const MAX_FIRES: u32 = 4u;

struct Box {
    origin_cell: vec4<f32>, // min corner relative to the camera (m), w: cell size
    dims_fade: vec4<f32>,   // nx, ny, nz, fade in (0..1)
};

struct MarchParams {
    boxes: array<Box, 4>,
    inv_view_proj: mat4x4<f32>, // camera-relative
    sizes: vec4<f32>,           // render width, height, box count, time
    sun_dir: vec4<f32>,         // toward the sun, w: Henyey-Greenstein g
    sun_radiance: vec4<f32>,    // w: smoke self-shadow density
    ambient: vec4<f32>,         // w: soot glow
    smoke: vec4<f32>,           // smoke colour (linear), w: smoke density (1/m per unit)
    fire: vec4<f32>,            // temperature (K), brightness, flame opacity (1/m), gain
};

@group(0) @binding(0) var<uniform> M: MarchParams;
@group(0) @binding(1) var lin: sampler;
@group(0) @binding(2) var depthTex: texture_depth_2d;
@group(0) @binding(3) var lut: texture_2d<f32>;
@group(0) @binding(4) var field0: texture_3d<f32>;
@group(0) @binding(5) var field1: texture_3d<f32>;
@group(0) @binding(6) var field2: texture_3d<f32>;
@group(0) @binding(7) var field3: texture_3d<f32>;

@vertex
fn vs_fullscreen(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    let x = f32((vi << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(vi & 2u) * 2.0 - 1.0;
    return vec4<f32>(x, y, 0.0, 1.0);
}

fn unproject(ndc: vec2<f32>, depth_rev: f32) -> vec3<f32> {
    let p = M.inv_view_proj * vec4<f32>(ndc, 1.0 - depth_rev, 1.0);
    return p.xyz / p.w;
}

fn sampleField(i: u32, q: vec3<f32>) -> vec4<f32> {
    switch i {
        case 0u: { return textureSampleLevel(field0, lin, q, 0.0); }
        case 1u: { return textureSampleLevel(field1, lin, q, 0.0); }
        case 2u: { return textureSampleLevel(field2, lin, q, 0.0); }
        default: { return textureSampleLevel(field3, lin, q, 0.0); }
    }
}

// the field at camera-relative `rel`, faded out at the box's sides and top so the box never
// shows (the bottom stands on the ground and keeps its edge)
fn fieldAt(i: u32, rel: vec3<f32>) -> vec4<f32> {
    let b = M.boxes[i];
    let q = (rel - b.origin_cell.xyz) / (b.dims_fade.xyz * b.origin_cell.w);
    if (any(q < vec3<f32>(0.0)) || any(q > vec3<f32>(1.0))) { return vec4<f32>(0.0); }
    let side = min(min(q.x, 1.0 - q.x), min(q.z, 1.0 - q.z));
    let fade = smoothstep(0.0, 0.1, side) * smoothstep(0.0, 0.15, 1.0 - q.y) * b.dims_fade.w;
    let f = sampleField(i, q);
    return vec4<f32>(f.x * fade, f.y, f.z * fade, f.w);
}

fn boxSpan(i: u32, dir: vec3<f32>) -> vec2<f32> {
    let b = M.boxes[i];
    let lo = b.origin_cell.xyz;
    let hi = lo + b.dims_fade.xyz * b.origin_cell.w;
    let inv = 1.0 / select(vec3<f32>(1e-6), dir, abs(dir) > vec3<f32>(1e-6));
    let t0 = lo * inv;
    let t1 = hi * inv;
    let tmin = min(t0, t1);
    let tmax = max(t0, t1);
    return vec2<f32>(max(max(max(tmin.x, tmin.y), tmin.z), 0.0), min(min(tmax.x, tmax.y), tmax.z));
}

// fire-emission.wgsl fireProGlow
fn glow(life: f32) -> f32 { return clamp(life * 5.0, 0.0, 1.0); }

// fireProEmission: thermal emission of the flame and the hot soot
fn emission(f: vec4<f32>) -> vec3<f32> {
    let absorption = (vec3<f32>(1.0) - M.smoke.xyz) * max(0.0, f.x) * M.smoke.w;
    let source = absorption * M.ambient.w + vec3<f32>(glow(f.z) * M.fire.z);
    if (all(source == vec3<f32>(0.0))) { return vec3<f32>(0.0); }
    let heat = max(0.0, f.y);
    let kelvin = 800.0 + heat / (heat + 2.0) * (M.fire.x - 800.0);
    let size = f32(textureDimensions(lut).x);
    let u = (clamp((kelvin - 500.0) / 9500.0, 0.0, 1.0) * (size - 1.0) + 0.5) / size;
    let spectrum = textureSampleLevel(lut, lin, vec2<f32>(u, 0.5), 0.0);
    let thermal = spectrum.rgb * exp2(clamp(spectrum.a, -24.0, 10.0));
    return thermal * source * M.fire.y * M.fire.w;
}

// optical depth toward the sun through the same box (four widening steps)
fn sunShadow(i: u32, rel: vec3<f32>) -> f32 {
    let cell = M.boxes[i].origin_cell.w;
    var tau = 0.0;
    var t = cell;
    for (var k = 0; k < 2; k++) {
        let ds = cell * (2.0 + f32(k) * 3.0);
        tau += max(fieldAt(i, rel + M.sun_dir.xyz * (t + ds * 0.5)).x, 0.0) * ds;
        t += ds;
    }
    return exp(-tau * M.sun_radiance.w);
}

@fragment
fn fs_fire(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let size = max(M.sizes.xy, vec2<f32>(1.0));
    let ndc = vec2<f32>(frag.x / size.x * 2.0 - 1.0, 1.0 - frag.y / size.y * 2.0);
    let dir = normalize(unproject(ndc, 1.0));
    let count = u32(M.sizes.z);
    // any box on this ray at all? (most pixels leave here)
    var hit = false;
    for (var i = 0u; i < count; i++) {
        let s = boxSpan(i, dir);
        if (s.y > s.x) { hit = true; }
    }
    if (!hit) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
    // debug (WGR_FIRE_GAIN < 0): the box, and the largest smoke / heat / flame along the ray
    if (M.fire.w < 0.0) {
        var mx = vec4<f32>(0.0);
        for (var i = 0u; i < count; i++) {
            let s = boxSpan(i, dir);
            if (s.y <= s.x) { continue; }
            for (var k = 0u; k < 64u; k++) {
                let t = s.x + (s.y - s.x) * (f32(k) + 0.5) / 64.0;
                mx = max(mx, sampleField(i, (dir * t - M.boxes[i].origin_cell.xyz) / (M.boxes[i].dims_fade.xyz * M.boxes[i].origin_cell.w)));
            }
        }
        return vec4<f32>(0.05 + min(mx.y, 4.0), min(mx.z, 1.0) * 4.0, min(mx.x, 4.0), 0.5);
    }

    let depth = textureLoad(depthTex, vec2<i32>(frag.xy), 0);
    var limit = 1e9;
    if (depth > 1e-7) { limit = length(unproject(ndc, depth)); }
    var seed = vec3<u32>(vec2<u32>(frag.xy), u32(M.sizes.w * 60.0)) * 1664525u + 1013904223u;
    seed.x += seed.y * seed.z;
    seed ^= seed >> vec3<u32>(16u);
    seed.x += seed.y * seed.z;
    let jitter = f32(seed.x >> 8u) / 16777216.0;

    let mu = dot(dir, M.sun_dir.xyz);
    let g = M.sun_dir.w;
    let phase = (1.0 - g * g) / pow(max(0.05, 1.0 + g * g - 2.0 * g * mu), 1.5);
    let direct = M.sun_radiance.rgb * (0.35 + phase * 0.3);
    let sunLit = M.sun_dir.y > 0.0 && max(direct.r, max(direct.g, direct.b)) > 0.02;

    var transmittance = 1.0;
    var radiance = vec3<f32>(0.0);
    // boxes arrive nearest first (the CPU sorts them); they do not overlap in practice
    for (var i = 0u; i < count; i++) {
        let s = boxSpan(i, dir);
        let t0 = s.x;
        let t1 = min(s.y, limit);
        if (t1 <= t0 || transmittance < 0.008) { continue; }
        let cell = M.boxes[i].origin_cell.w;
        let len = t1 - t0;
        let steps = clamp(u32(ceil(len / cell)), 1u, 96u);
        let ds = len / f32(steps);
        for (var k = 0u; k < steps; k++) {
            let rel = dir * (t0 + (f32(k) + jitter) * ds);
            let f = fieldAt(i, rel);
            if (f.x + glow(f.z) > 0.002) {
                let density = max(0.0, f.x) * M.smoke.w;
                var scattering = vec3<f32>(0.0);
                if (density > 0.0) {
                    // the self-shadow only matters while the sun is up
                    var sunVis = 1.0;
                    if (sunLit) { sunVis = sunShadow(i, rel); }
                    scattering = M.smoke.xyz * (M.ambient.xyz + direct * sunVis) * density;
                }
                let sigma = density + glow(f.z) * M.fire.z;
                let src = scattering + emission(f);
                let tau = sigma * ds;
                var integral = ds * (1.0 - 0.5 * tau + tau * tau / 6.0);
                if (tau >= 0.001) { integral = (1.0 - exp(-tau)) / sigma; }
                radiance += transmittance * src * integral;
                transmittance *= 1.0 - min(1.0, sigma * integral);
                if (transmittance < 0.008) { break; }
            }
        }
    }
    // premultiplied: the pipeline blends src + dst * srcAlpha
    return vec4<f32>(radiance, transmittance);
}

// the upscalers' history-control ("reactive") mask: the fire moves without motion vectors, so
// DLSS is told to trust the current frame where it is (a coarse march of its opacity)
@fragment
fn fs_fire_mask(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let size = max(M.sizes.xy, vec2<f32>(1.0));
    let ndc = vec2<f32>(frag.x / size.x * 2.0 - 1.0, 1.0 - frag.y / size.y * 2.0);
    let dir = normalize(unproject(ndc, 1.0));
    let depth = textureLoad(depthTex, vec2<i32>(frag.xy), 0);
    var limit = 1e9;
    if (depth > 1e-7) { limit = length(unproject(ndc, depth)); }
    var tau = 0.0;
    for (var i = 0u; i < u32(M.sizes.z); i++) {
        let s = boxSpan(i, dir);
        let t1 = min(s.y, limit);
        if (t1 <= s.x) { continue; }
        let ds = (t1 - s.x) / 16.0;
        for (var k = 0u; k < 16u; k++) {
            let f = fieldAt(i, dir * (s.x + (f32(k) + 0.5) * ds));
            tau += (max(f.x, 0.0) * M.smoke.w + glow(f.z) * M.fire.z * 3.0) * ds;
        }
    }
    return vec4<f32>(clamp((1.0 - exp(-tau)) * 2.0, 0.0, 1.0), 0.0, 0.0, 1.0);
}
