// FIRE-001: the fire volumes drawn into the lit scene.
//
// Port of the camera march and shading of dgreenheck/threejs-fire-pro @ c284f0b1 (MIT,
// (c) 2026 Daniel Greenheck; see CREDITS.md): volume-march.wgsl (front-to-back integration with
// the analytic step integral, jittered samples), fire-emission.wgsl (smoke absorption and
// scattering, flame absorption from its glow, Kirchhoff emission j = sigma_a B(T) with the
// temperature from heat and the blackbody table of blackbody.ts) and the sun lobe of
// fireProDirect.
//
// What OP changes: one full-screen pass over up to six dense boxes, nearest first; the march
// stops at OP's reversed-Z scene depth; the smoke is lit by OP's sun and ambient with a short
// march toward the sun through the same box for its self-shadow (Fire Pro: a light volume).

const MAX_FIRES: u32 = 6u;

struct Box {
    origin_cell: vec4<f32>, // min corner relative to the camera (m), w: cell size
    dims_fade: vec4<f32>,   // nx, ny, nz, fade in (0..1)
    smoke: vec4<f32>,       // smoke colour (linear albedo), w: density (1/m per unit)
    frame: vec4<f32>,       // the frame's pivot (camera-relative), w: its turn about +y (rad)
    look: vec4<f32>,        // FIRE-002: brightness, soot glow, flame opacity (1/m), temperature (K)
    grey: vec4<f32>,        // FIRE-002: the albedo of the smoke's grey share (fields .w)
};

struct MarchParams {
    boxes: array<Box, 6>,
    inv_view_proj: mat4x4<f32>, // camera-relative
    sizes: vec4<f32>,           // render width, height, box count, time
    sun_dir: vec4<f32>,         // toward the sun, w: Henyey-Greenstein g
    sun_radiance: vec4<f32>,    // w: smoke self-shadow density
    ambient: vec4<f32>,         // w: soot glow
    smoke: vec4<f32>,           // unused (each box carries its own smoke colour and density)
    fire: vec4<f32>,            // temperature (K), brightness, flame opacity (1/m), gain
    // BOOM-005: muzzle flashes, two vec4 each: (muzzle camera-relative, age s), (barrel, size)
    flashes: array<vec4<f32>, 16>,
    flash_info: vec4<f32>,      // x: count, z: gain
};

@group(0) @binding(0) var<uniform> M: MarchParams;
@group(0) @binding(1) var lin: sampler;
@group(0) @binding(2) var depthTex: texture_depth_2d;
@group(0) @binding(3) var lut: texture_2d<f32>;
@group(0) @binding(4) var field0: texture_3d<f32>;
@group(0) @binding(5) var field1: texture_3d<f32>;
@group(0) @binding(6) var field2: texture_3d<f32>;
@group(0) @binding(7) var field3: texture_3d<f32>;
@group(0) @binding(8) var field4: texture_3d<f32>;
@group(0) @binding(9) var field5: texture_3d<f32>;

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
        case 3u: { return textureSampleLevel(field3, lin, q, 0.0); }
        case 4u: { return textureSampleLevel(field4, lin, q, 0.0); }
        default: { return textureSampleLevel(field5, lin, q, 0.0); }
    }
}

// a camera-relative point / direction in box i's frame (a plume box is turned to the wind about
// the vertical through its source; an upright box's turn is 0 and this is the identity)
fn toBox(i: u32, rel: vec3<f32>) -> vec3<f32> {
    let f = M.boxes[i].frame;
    let cs = vec2<f32>(cos(f.w), sin(f.w));
    let d = rel.xz - f.xz;
    return vec3<f32>(f.x + d.x * cs.x + d.y * cs.y, rel.y, f.z - d.x * cs.y + d.y * cs.x);
}
fn dirToBox(i: u32, dir: vec3<f32>) -> vec3<f32> {
    let cs = vec2<f32>(cos(M.boxes[i].frame.w), sin(M.boxes[i].frame.w));
    return vec3<f32>(dir.x * cs.x + dir.z * cs.y, dir.y, -dir.x * cs.y + dir.z * cs.x);
}
// box i's texture coordinates of camera-relative `rel`
fn boxQ(i: u32, rel: vec3<f32>) -> vec3<f32> {
    let b = M.boxes[i];
    return (toBox(i, rel) - b.origin_cell.xyz) / (b.dims_fade.xyz * b.origin_cell.w);
}

// the field at camera-relative `rel`, faded out at the box's sides and top so the box never
// shows (the bottom stands on the ground and keeps its edge)
fn fieldAt(i: u32, rel: vec3<f32>) -> vec4<f32> {
    let b = M.boxes[i];
    let q = boxQ(i, rel);
    if (any(q < vec3<f32>(0.0)) || any(q > vec3<f32>(1.0))) { return vec4<f32>(0.0); }
    let side = min(min(q.x, 1.0 - q.x), min(q.z, 1.0 - q.z));
    let fade = smoothstep(0.0, 0.1, side) * smoothstep(0.0, 0.15, 1.0 - q.y) * b.dims_fade.w;
    let f = sampleField(i, q);
    return vec4<f32>(f.x * fade, f.y, f.z * fade, f.w * fade);
}

fn boxSpan(i: u32, dirWorld: vec3<f32>) -> vec2<f32> {
    let b = M.boxes[i];
    let dir = dirToBox(i, dirWorld);
    let lo = b.origin_cell.xyz - toBox(i, vec3<f32>(0.0));
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

// fireProEmission: thermal emission of the flame and the hot soot (look: the box's own
// brightness, soot glow, flame opacity and temperature)
fn emission(f: vec4<f32>, smoke: vec4<f32>, look: vec4<f32>) -> vec3<f32> {
    let absorption = (vec3<f32>(1.0) - smoke.xyz) * max(0.0, f.x) * smoke.w;
    let source = absorption * look.y + vec3<f32>(glow(f.z) * look.z);
    if (all(source == vec3<f32>(0.0))) { return vec3<f32>(0.0); }
    let heat = max(0.0, f.y);
    let kelvin = 800.0 + heat / (heat + 2.0) * (look.w - 800.0);
    let size = f32(textureDimensions(lut).x);
    let u = (clamp((kelvin - 500.0) / 9500.0, 0.0, 1.0) * (size - 1.0) + 0.5) / size;
    let spectrum = textureSampleLevel(lut, lin, vec2<f32>(u, 0.5), 0.0);
    let thermal = spectrum.rgb * exp2(clamp(spectrum.a, -24.0, 10.0));
    return thermal * source * look.x * M.fire.w;
}

// ---- BOOM-005: the muzzle flash, calculated ------------------------------------------------
// (owner, 2026-10-19: "it must not look like a cartoony texture but should be calculated")
// No sheet and no sprite: an emission field marched like the fire, its light the blackbody
// radiance of the gas's temperature (the same table). What leaves a rifle's muzzle, in the
// local frame (x along the barrel, metres for a rifle; `size` scales it):
//   primary flash   -- the propellant gas at the exit, white-hot (~3400 K), a few cm, round the
//                      first shock cell (the Mach disc) ~3.5 cm out;
//   port jets       -- the flash hider's slots blow 4-5 thin jets out sideways and forward,
//                      cooling as they spread;
//   secondary flash -- a decimetre or two out the fuel-rich gas meets the air and burns: a
//                      larger, turbulent, cooler (1900-2400 K) orange-yellow cloud, the bulk of
//                      what one sees. It grows as the frame goes on.
// A 60 fps frame lasts longer than the flash itself (~2-10 ms), so like a camera the frame
// the shot falls in shows all of it; it is gone within two more.
fn flashHash(p: vec3<i32>, salt: u32) -> f32 {
    var h = (bitcast<u32>(p.x) * 1597334677u) ^ (bitcast<u32>(p.y) * 3812015801u) ^ (bitcast<u32>(p.z) * 2798796415u) ^ salt;
    h = (h ^ (h >> 16u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return f32((h ^ (h >> 16u)) & 0x00ffffffu) / 16777215.0;
}
fn flashNoise(p: vec3<f32>, salt: u32) -> f32 {
    let i = vec3<i32>(floor(p));
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let a = mix(mix(flashHash(i, salt), flashHash(i + vec3<i32>(1, 0, 0), salt), u.x),
                mix(flashHash(i + vec3<i32>(0, 1, 0), salt), flashHash(i + vec3<i32>(1, 1, 0), salt), u.x), u.y);
    let b = mix(mix(flashHash(i + vec3<i32>(0, 0, 1), salt), flashHash(i + vec3<i32>(1, 0, 1), salt), u.x),
                mix(flashHash(i + vec3<i32>(0, 1, 1), salt), flashHash(i + vec3<i32>(1, 1, 1), salt), u.x), u.y);
    return mix(a, b, u.z);
}
fn blackbody(kelvin: f32) -> vec3<f32> {
    let size = f32(textureDimensions(lut).x);
    let u = (clamp((kelvin - 500.0) / 9500.0, 0.0, 1.0) * (size - 1.0) + 0.5) / size;
    let spectrum = textureSampleLevel(lut, lin, vec2<f32>(u, 0.5), 0.0);
    return spectrum.rgb * exp2(clamp(spectrum.a, -24.0, 10.0));
}
// (emission density 1/m, temperature K) at local point q (metres / size)
fn flashField(q: vec3<f32>, grow: f32, salt: u32) -> vec2<f32> {
    let rho2 = q.y * q.y + q.z * q.z;
    // primary flash round the first shock cell
    let c = q - vec3<f32>(0.035, 0.0, 0.0);
    // (first capture, 2026-10-19: at 140 the core outshone everything and bloomed into a round
    // white blob; the hider's star is what a rifle flash looks like in daylight)
    let core = 75.0 * exp(-dot(c, c) / (0.014 * 0.014));
    // the gas column out of the bore, widening
    let r0 = 0.006 + 0.12 * max(q.x, 0.0);
    let bore = 60.0 * exp(-rho2 / (r0 * r0)) * (1.0 - smoothstep(0.0, 0.07, q.x)) * step(-0.005, q.x);
    // the hider's port jets
    let ports = 4.0 + floor(flashHash(vec3<i32>(7, 3, 1), salt) * 2.0);
    let turn = flashHash(vec3<i32>(1, 9, 4), salt) * 6.2831853;
    var jets = 0.0;
    for (var k = 0.0; k < ports; k += 1.0) {
        let a = turn + k * 6.2831853 / ports;
        let j = normalize(vec3<f32>(0.55, cos(a), sin(a)));
        let s = dot(q, j);
        // each jet its own length: the prongs of the star are never even
        let reach = 0.08 + 0.06 * flashHash(vec3<i32>(i32(k), 5, 2), salt);
        if (s <= 0.0 || s > reach) { continue; }
        let perp = q - j * s;
        let r = 0.003 + 0.07 * s;
        jets += 120.0 * exp(-dot(perp, perp) / (r * r)) * (1.0 - s / reach);
    }
    // the secondary flash: an ellipsoid out ahead, eaten into tongues by turbulence
    let centre = vec3<f32>(0.11 * grow, 0.0, 0.0);
    let e = (q - centre) / vec3<f32>(0.10 * grow, 0.04 * grow, 0.04 * grow);
    let n = 0.55 * flashNoise(q * 38.0, salt) + 0.3 * flashNoise(q * 85.0 + vec3<f32>(3.1, 7.7, 1.3), salt)
          + 0.15 * flashNoise(q * 170.0, salt);
    let body = 1.0 - dot(e, e);
    let burn = 16.0 * smoothstep(0.1, 0.5, body + 1.2 * (n - 0.5));
    let density = core + bore + jets + burn;
    if (density < 0.01) { return vec2<f32>(0.0); }
    // temperature: the density-weighted mix of the parts (one table lookup per sample)
    // (the ports' gas is nearly as hot as the bore's: at 2400 K the jets were invisible beside a
    // 3200 K core -- blackbody brightness falls steeply with temperature)
    let kelvin = (core * 3000.0 + bore * 2950.0 + jets * 2850.0 + burn * (2100.0 + 400.0 * n)) / density;
    return vec2<f32>(density, kelvin);
}
// the flashes' light along the ray (nearer than any fire in practice: added in front)
fn flashRadiance(dir: vec3<f32>, limit: f32, jitter: f32) -> vec3<f32> {
    let count = u32(M.flash_info.x);
    var sum = vec3<f32>(0.0);
    for (var i = 0u; i < count; i++) {
        let a = M.flashes[2u * i];
        let b = M.flashes[2u * i + 1u];
        let age = a.w;
        let fade = select(exp(-(age - 0.017) / 0.008), 1.0, age < 0.017);
        if (fade < 0.01) { continue; }
        let size = b.w;
        let grow = 1.0 + 1.2 * clamp(age / 0.02, 0.0, 1.0);
        let fwd = normalize(b.xyz);
        let centre = a.xyz + fwd * (0.09 * size * grow);
        let radius = 0.15 * size * grow;
        // the ray against the flash's bounding sphere
        let tc = dot(centre, dir);
        let d2 = dot(centre, centre) - tc * tc;
        if (d2 > radius * radius) { continue; }
        let half = sqrt(radius * radius - d2);
        let t0 = max(tc - half, 0.0);
        let t1 = min(tc + half, limit);
        if (t1 <= t0) { continue; }
        // the barrel frame
        var side = cross(fwd, vec3<f32>(0.0, 1.0, 0.0));
        if (dot(side, side) < 1e-4) { side = cross(fwd, vec3<f32>(1.0, 0.0, 0.0)); }
        side = normalize(side);
        let up = cross(side, fwd);
        // every shot shaped differently: seeded from where it was fired (camera-relative, so
        // it also changes as the shooter moves)
        let salt = bitcast<u32>(a.x) ^ (bitcast<u32>(a.z) * 2654435761u) ^ bitcast<u32>(a.y);
        // ~5 mm steps: the port jets are 3-10 mm thick (28 steps over a 0.5 m sphere stepped
        // over them and left a haze); only the pixels round a muzzle pay for it
        let steps = 64u;
        let ds = (t1 - t0) / f32(steps);
        var light = vec3<f32>(0.0);
        for (var k = 0u; k < steps; k++) {
            let p = dir * (t0 + (f32(k) + jitter) * ds) - a.xyz;
            let q = vec3<f32>(dot(p, fwd), dot(p, side), dot(p, up)) / size;
            let f = flashField(q, grow, salt);
            if (f.x > 0.0) { light += blackbody(f.y) * (f.x * ds / size); }
        }
        sum += light * fade;
    }
    return sum * 0.012 * M.flash_info.z;
}

// optical depth toward the sun through the same box (four widening steps)
// optical depth along `dir` through the same box, in `taps` widening steps (a flame's box: 2,
// reaching ~7 cells; a plume's: 4, reaching ~26 cells -- Fire Pro reads a light volume instead)
// BOOM-008 (owner, 2026-10-19: "smoke from explosion should also create shadows"): the smoke's
// shadow on whatever the pixel shows. The sun's ray from the surface point through each box, its
// optical depth marched in 12 steps; the direct sun the surface had is cut by what gets through.
// (The cloud shadow map's smoke blobs are ~8 m a texel: an explosion's column fell between them.)
fn boxSpanFrom(i: u32, origin: vec3<f32>, dirWorld: vec3<f32>) -> vec2<f32> {
    let b = M.boxes[i];
    let dir = dirToBox(i, dirWorld);
    let lo = b.origin_cell.xyz - toBox(i, origin);
    let hi = lo + b.dims_fade.xyz * b.origin_cell.w;
    let inv = 1.0 / select(vec3<f32>(1e-6), dir, abs(dir) > vec3<f32>(1e-6));
    let t0 = lo * inv;
    let t1 = hi * inv;
    let tmin = min(t0, t1);
    let tmax = max(t0, t1);
    return vec2<f32>(max(max(max(tmin.x, tmin.y), tmin.z), 0.0), min(min(tmax.x, tmax.y), tmax.z));
}
fn smokeSunShadow(rel: vec3<f32>, count: u32) -> f32 {
    if (M.sun_dir.y <= 0.02) { return 1.0; }
    let sd = normalize(M.sun_dir.xyz);
    var trans = 1.0;
    for (var i = 0u; i < count; i++) {
        let s = boxSpanFrom(i, rel, sd);
        if (s.y <= s.x) { continue; }
        let ds = (s.y - s.x) / 12.0;
        var tau = 0.0;
        for (var k = 0u; k < 12u; k++) {
            tau += max(fieldAt(i, rel + sd * (s.x + (f32(k) + 0.5) * ds)).x, 0.0) * ds;
        }
        trans *= exp(-tau * M.boxes[i].smoke.w);
    }
    // on a sunny day about three quarters of what lights a surface is the direct sun
    return mix(1.0, trans, 0.75);
}

fn shadowAlong(i: u32, rel: vec3<f32>, dir: vec3<f32>, taps: i32) -> f32 {
    let cell = M.boxes[i].origin_cell.w;
    var tau = 0.0;
    var t = cell;
    for (var k = 0; k < taps; k++) {
        let ds = cell * (2.0 + f32(k) * 3.0);
        tau += max(fieldAt(i, rel + dir * (t + ds * 0.5)).x, 0.0) * ds;
        t += ds;
    }
    return exp(-tau * M.sun_radiance.w * M.boxes[i].smoke.w / 7.11);
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
    // BOOM-008: or the surface here lies in a box's sun shadow
    let surfDepth = textureLoad(depthTex, vec2<i32>(frag.xy), 0);
    var shadow = 1.0;
    if (surfDepth > 1e-7 && M.sun_dir.y > 0.02 && M.fire.w >= 0.0) {
        let surf = unproject(ndc, surfDepth);
        let sd = normalize(M.sun_dir.xyz);
        var inShadow = false;
        for (var i = 0u; i < count; i++) {
            let s = boxSpanFrom(i, surf, sd);
            if (s.y > s.x) { inShadow = true; }
        }
        if (inShadow) {
            shadow = smokeSunShadow(surf, count);
            hit = true;
        }
    }
    // BOOM-005: or a muzzle flash's bounding sphere
    for (var i = 0u; i < u32(M.flash_info.x); i++) {
        let fa = M.flashes[2u * i];
        let fb = M.flashes[2u * i + 1u];
        let fc = fa.xyz + normalize(fb.xyz) * (0.2 * fb.w);
        let ftc = dot(fc, dir);
        let fr = 0.34 * fb.w;
        if (ftc > -fr && dot(fc, fc) - ftc * ftc < fr * fr) { hit = true; }
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
                mx = max(mx, sampleField(i, boxQ(i, dir * t)));
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
    // BOOM-005: the muzzle flashes, pure emission (the gas is transparent), in front
    var radiance = flashRadiance(dir, limit, jitter);
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
                // the smoke's colour here: the box's, mixed toward grey by the grey share it carries
                let greyShare = clamp(f.w / max(f.x, 1e-4), 0.0, 1.0);
                let smoke = vec4<f32>(mix(M.boxes[i].smoke.xyz, M.boxes[i].grey.xyz, greyShare), M.boxes[i].smoke.w);
                let density = max(0.0, f.x) * smoke.w;
                var scattering = vec3<f32>(0.0);
                if (density > 0.0) {
                    // the self-shadow only matters while the sun is up
                    // a plume (bright, wide) needs the longer shadow march and its core darkened
                    // under the sky too; a flame's soot is lit by the fire anyway
                    let plume = M.boxes[i].dims_fade.w > 0.0 && smoke.x + smoke.y + smoke.z > 0.3;
                    // a plume's shadow reaches across the box (6 taps, ~57 cells): its far side and
                    // underside go dark, which is what gives smoke its shape in sunlight
                    let taps = select(2, 6, plume);
                    var sunVis = 1.0;
                    if (sunLit) { sunVis = shadowAlong(i, rel, M.sun_dir.xyz, taps); }
                    var skyVis = 1.0;
                    if (plume) { skyVis = mix(0.1, 1.0, shadowAlong(i, rel, vec3<f32>(0.0, 1.0, 0.0), 4)); }
                    scattering = smoke.xyz * (M.ambient.xyz * skyVis + direct * sunVis) * density;
                }
                let look = M.boxes[i].look;
                let sigma = density + glow(f.z) * look.z;
                let src = scattering + emission(f, smoke, look);
                let tau = sigma * ds;
                var integral = ds * (1.0 - 0.5 * tau + tau * tau / 6.0);
                if (tau >= 0.001) { integral = (1.0 - exp(-tau)) / sigma; }
                radiance += transmittance * src * integral;
                transmittance *= 1.0 - min(1.0, sigma * integral);
                if (transmittance < 0.008) { break; }
            }
        }
    }
    // premultiplied: the pipeline blends src + dst * srcAlpha; the smoke's sun shadow on the
    // surface behind dims the scene there
    return vec4<f32>(radiance, transmittance * shadow);
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
            tau += (max(f.x, 0.0) * M.boxes[i].smoke.w + glow(f.z) * M.boxes[i].look.z * 3.0) * ds;
        }
    }
    return vec4<f32>(clamp((1.0 - exp(-tau)) * 2.0, 0.0, 1.0), 0.0, 0.0, 1.0);
}
