// FIRE-001: the fire solver, one dense grid per fire.
//
// Port of the solver in dgreenheck/threejs-fire-pro @ c284f0b1 (MIT, (c) 2026 Daniel Greenheck;
// see CREDITS.md): fluid-advection.wgsl (RK2 traces), fluid-scalar-transport.wgsl (MacCormack),
// fluid-field-evolution.wgsl + flame-common.wgsl (flame lifetime, its smoke / heat / expansion
// ramps, cooling and dissipation), fluid-forces.wgsl (buoyancy from heat, smoke weight,
// vorticity confinement, damping, source velocity), force-fields.wgsl + curl-noise.wgsl
// (turbulence) and the divergence / projection.
//
// What OP changes: a dense box around each fire instead of the sparse, GPU-allocated bricks;
// velocity at cell centres instead of on a staggered MAC grid; Jacobi iterations, warm-started
// from the previous step, instead of the multigrid V-cycles; open sides and top (zero pressure
// outside), solid ground under the bottom row. The fields are Fire Pro's: smoke, heat, flame
// lifetime (fuel is not used here).

struct SimParams {
    origin_cell: vec4<f32>, // grid min corner (world, m), w: cell size (m)
    dims: vec4<u32>,        // nx, ny, nz, ground under the bottom row (1)
    step: vec4<f32>,        // dt, time, noise seed, unused
    emitter: vec4<f32>,     // centre (world), radius (m)
    emit: vec4<f32>,        // flame (0..1), heat / s, smoke / s, source rise speed (m/s)
    flame: vec4<f32>,       // lifespan (s), smoke rate, heat rate, expansion rate (1/s)
    motion: vec4<f32>,      // buoyancy, smoke weight, damping (1/s), vorticity
    losses: vec4<f32>,      // cooling (1/s), dissipation (1/s), turbulence strength, turbulence scale
    wind: vec4<f32>,        // wind velocity x, bed patchiness (0..1), wind z, response (1/s)
    extra: vec4<f32>,       // x: flame (1) or smoke only (0), y: share of the wind taken up,
                            // z: the box frame's turn about the vertical through the emitter (rad),
                            // w: a bursting charge's outward speed (m/s, 0 for none)
    tint: vec4<f32>,        // FIRE-002: grey share of new smoke: x mean, y swing (fields .w)
    // the solids (Fire Pro's colliders, solids.wgsl): 10 x 10 columns over the box, each open
    // from floor to ceiling (world y); a wall is a column whose ceiling is below its floor
    floor_h: array<vec4<f32>, 25>,
    ceil_h: array<vec4<f32>, 25>,
};

@group(0) @binding(0) var<uniform> S: SimParams;
@group(0) @binding(1) var lin: sampler;
@group(0) @binding(2) var fieldsIn: texture_3d<f32>; // smoke, heat, flame lifetime, (fuel)
@group(0) @binding(3) var velIn: texture_3d<f32>;
@group(0) @binding(4) var auxIn: texture_3d<f32>;    // forward-advected fields / curl
@group(0) @binding(5) var pIn: texture_3d<f32>;      // pressure (r32float)
@group(0) @binding(6) var dIn: texture_3d<f32>;      // divergence / expansion (r32float)
@group(0) @binding(7) var outRgba: texture_storage_3d<rgba16float, write>;
@group(0) @binding(8) var outR: texture_storage_3d<r32float, write>;

fn dims() -> vec3<i32> { return vec3<i32>(S.dims.xyz); }

fn solidAt(p: vec3<f32>) -> bool {
    // the engine probed the columns around the bed (the box itself may sit downwind of it) in
    // world axes; a wind-turned box's frame turns back about the emitter first
    let cs = vec2<f32>(cos(S.extra.z), sin(S.extra.z));
    let d = p.xz - S.emitter.xz;
    let world = vec3<f32>(S.emitter.x + d.x * cs.x - d.y * cs.y, p.y, S.emitter.z + d.x * cs.y + d.y * cs.x);
    let corner = S.emitter.xz - 5.0 * S.step.w;
    let c = clamp(vec2<i32>(floor((world.xz - corner) / S.step.w)), vec2<i32>(0), vec2<i32>(9));
    let k = c.y * 10 + c.x;
    let fl = S.floor_h[k / 4][k % 4];
    let ce = S.ceil_h[k / 4][k % 4];
    return world.y < fl || world.y > ce;
}
fn solidCell(id: vec3<i32>) -> bool { return solidAt(S.origin_cell.xyz + (vec3<f32>(id) + 0.5) * S.origin_cell.w); }

// solids.wgsl enforceFaces: no flow into a solid neighbour, none at all inside one
fn enforce(id: vec3<i32>, v_in: vec3<f32>) -> vec3<f32> {
    if (solidCell(id)) { return vec3<f32>(0.0); }
    var v = v_in;
    if (v.x > 0.0 && solidCell(id + vec3<i32>(1, 0, 0))) { v.x = 0.0; }
    if (v.x < 0.0 && solidCell(id - vec3<i32>(1, 0, 0))) { v.x = 0.0; }
    if (v.y > 0.0 && solidCell(id + vec3<i32>(0, 1, 0))) { v.y = 0.0; }
    if (v.y < 0.0 && solidCell(id - vec3<i32>(0, 1, 0))) { v.y = 0.0; }
    if (v.z > 0.0 && solidCell(id + vec3<i32>(0, 0, 1))) { v.z = 0.0; }
    if (v.z < 0.0 && solidCell(id - vec3<i32>(0, 0, 1))) { v.z = 0.0; }
    return v;
}
fn inside(id: vec3<i32>) -> bool { return all(id >= vec3<i32>(0)) && all(id < dims()); }
fn centre(id: vec3<i32>) -> vec3<f32> { return S.origin_cell.xyz + (vec3<f32>(id) + 0.5) * S.origin_cell.w; }
fn uvw(world: vec3<f32>) -> vec3<f32> { return (world - S.origin_cell.xyz) / (vec3<f32>(dims()) * S.origin_cell.w); }

fn velAt(world: vec3<f32>) -> vec3<f32> { return textureSampleLevel(velIn, lin, uvw(world), 0.0).xyz; }
// the fields at a world point; outside the box is clean air (clamping to the edge smeared the
// wall's values back in as streaks)
fn fieldsAt(tex: texture_3d<f32>, world: vec3<f32>) -> vec4<f32> {
    let q = uvw(world);
    if (any(q < vec3<f32>(0.0)) || any(q > vec3<f32>(1.0))) { return vec4<f32>(0.0); }
    return textureSampleLevel(tex, lin, q, 0.0);
}
fn velCell(id: vec3<i32>) -> vec3<f32> {
    if (!inside(id)) { return vec3<f32>(0.0); }
    return textureLoad(velIn, id, 0).xyz;
}
fn pCell(id: vec3<i32>, here: f32) -> f32 {
    // open sides and top: still air at zero pressure; the ground and solids mirror the cell
    if (id.y < 0 && S.dims.w == 1u) { return here; }
    if (!inside(id)) { return 0.0; }
    if (solidCell(id)) { return here; }
    return textureLoad(pIn, id, 0).x;
}
fn curlMag(id: vec3<i32>) -> f32 {
    let c = clamp(id, vec3<i32>(0), dims() - 1);
    return textureLoad(auxIn, c, 0).w;
}

// Fire Pro's RK2 trace back over one step (fluid-advection.wgsl): the midpoint velocity
fn traceBack(p: vec3<f32>, dt: f32) -> vec3<f32> {
    let v0 = velAt(p);
    let mid = p - 0.5 * dt * v0;
    return p - dt * velAt(mid);
}

@compute @workgroup_size(4, 4, 4)
fn cs_clear(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    textureStore(outRgba, id, vec4<f32>(0.0));
    textureStore(outR, id, vec4<f32>(0.0));
}

@compute @workgroup_size(4, 4, 4)
fn cs_advect_vel(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let src = traceBack(centre(id), S.step.x);
    textureStore(outRgba, id, vec4<f32>(enforce(id, velAt(src)), 0.0));
}

// MacCormack, first half: the plain semi-Lagrangian result
@compute @workgroup_size(4, 4, 4)
fn cs_advect_fwd(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let src = traceBack(centre(id), S.step.x);
    var f = fieldsAt(fieldsIn, src);
    if (solidCell(id)) { f = vec4<f32>(0.0); }
    textureStore(outRgba, id, f);
}

// flame-common.wgsl: the authored ramps (FLAME-LIFETIME-001 defaults, flame.ts)
fn rampIntegral(channel: u32, lo: f32, hi: f32) -> f32 {
    var xs: array<f32, 4>;
    var ys: array<f32, 4>;
    var n = 4u;
    if (channel == 0u) { // smoke
        xs = array<f32, 4>(0.0, 0.2, 0.5, 1.0); ys = array<f32, 4>(0.0, 1.0, 0.0, 0.0);
    } else if (channel == 1u) { // heat
        xs = array<f32, 4>(0.0, 0.2, 1.0, 1.0); ys = array<f32, 4>(0.0, 1.0, 1.0, 1.0); n = 3u;
    } else { // expansion
        xs = array<f32, 4>(0.0, 0.5, 0.75, 1.0); ys = array<f32, 4>(0.0, 0.0, 1.0, 1.0);
    }
    var area = 0.0;
    for (var i = 1u; i < n; i++) {
        let ax = xs[i - 1u]; let bx = xs[i];
        let left = max(lo, ax);
        let right = min(hi, bx);
        if (right > left && bx > ax) {
            let y0 = mix(ys[i - 1u], ys[i], (left - ax) / (bx - ax));
            let y1 = mix(ys[i - 1u], ys[i], (right - ax) / (bx - ax));
            area += (right - left) * (y0 + y1) * 0.5;
        }
    }
    return area;
}

// MacCormack, second half, and the field evolution (fluid-field-evolution.wgsl)
@compute @workgroup_size(4, 4, 4)
fn cs_evolve(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let dt = S.step.x;
    let p = centre(id);
    let forward = textureLoad(auxIn, id, 0);
    // the reverse trace of the advected result, and the correction, clamped to the donors
    let v0 = velAt(p);
    let ahead = p + dt * velAt(p + 0.5 * dt * v0);
    let reverse = fieldsAt(auxIn, ahead);
    var corrected = forward + 0.5 * (textureLoad(fieldsIn, id, 0) - reverse);
    let src = traceBack(p, dt);
    let f = uvw(src) * vec3<f32>(dims()) - 0.5;
    let b = vec3<i32>(floor(f));
    var lo = vec4<f32>(1e9);
    var hi = vec4<f32>(-1e9);
    for (var k = 0; k < 8; k++) {
        let c = clamp(b + vec3<i32>(k & 1, (k >> 1) & 1, k >> 2), vec3<i32>(0), dims() - 1);
        let s = textureLoad(fieldsIn, c, 0);
        lo = min(lo, s);
        hi = max(hi, s);
    }
    corrected = clamp(corrected, lo, hi);
    let transported = max(corrected, vec4<f32>(0.0));

    // the emitter: a flattened sphere at the base (fluid-sources.wgsl emitterWeight)
    // BOOM-001: a burst (extra.w > 0) blows from a near-round charge, not a flat bed: a flattened
    // emitter made the fireball a lens
    // (a ground burst's thrown earth, tint.w: a tall column of soil, ~3 radii high)
    let flat = select(0.45, select(0.9, 3.2, S.tint.w > 0.5), S.extra.w > 0.0);
    let d = (p - S.emitter.xyz) / max(vec3<f32>(S.emitter.w, S.emitter.w * flat, S.emitter.w), vec3<f32>(0.03));
    let r = length(d);
    var weight = 0.0;
    if (r < 1.0) { weight = 1.0 - smoothstep(0.5, 1.0, r); }
    if (S.tint.w > 0.5 && weight > 0.0) {
        // BOOM-001: thrown earth leaves in separate jets (the crown of spikes in the reference
        // photos) and a central column, not as one ball: emit only where the crown has a jet
        let a = atan2(d.z, d.x);
        let jets = abs(sin(a * 3.5 + S.step.z) * sin(a * 2.3 + S.step.z * 1.7 + 1.3));
        let core = 1.0 - smoothstep(0.2, 0.4, length(d.xz));
        weight *= max(smoothstep(0.3, 0.75, jets), core);
    }
    var lifetime = transported.z;
    var sourceProducts = vec2<f32>(0.0);
    if (weight > 0.0) {
        // fluid-sources.wgsl burstVariation: where the bed burns, a pattern that drifts upward
        // and churns, so a big bed is many flamelets, not one tongue (none on a small bed)
        var vary = vec2<f32>(1.0);
        if (S.wind.y > 0.0) {
            let t = S.step.y;
            let q = (p - S.emitter.xyz) / 0.55 + vec3<f32>(3.17, -t * 1.7, 11.29) + vec3<f32>(0.0, 0.0, t * 0.35);
            vary = vec2<f32>(1.0) - S.wind.y * vec2<f32>(vnoise(q), vnoise(q + vec3<f32>(17.3, 2.8, 8.6)));
        }
        if (S.extra.x > 0.5) { lifetime = max(lifetime, S.emit.x * weight * vary.y); }
        sourceProducts = vec2<f32>(S.emit.z, S.emit.y * vary.x) * weight * dt * S.emit.x;
    }
    // evaluateFlame: one step of lifetime, and what the flame produces over it
    var products = vec3<f32>(0.0);
    let before = clamp(lifetime, 0.0, 1.0);
    var after = 0.0;
    if (before > 0.0) {
        let life = S.flame.x;
        after = max(0.0, before - dt / life);
        products = vec3<f32>(S.flame.y * life * rampIntegral(0u, after, before),
                             S.flame.z * life * rampIntegral(1u, after, before),
                             S.flame.w * life * rampIntegral(2u, after, before) / dt);
    }
    let losses = vec2<f32>(S.losses.y, S.losses.x); // smoke dissipation, heat cooling
    let dh = transported.xy * exp(-losses * dt) + (sourceProducts + products.xy) * exp(-losses * dt * 0.5);
    // FIRE-002: the grey share of the smoke, a tracer carried in .w (fuel is not used here): the
    // smoke made this step is grey by a share that drifts in patches through the source and in
    // time, so a wreck's plume comes out black and grey by turns
    var greyNow = 0.0;
    if (S.tint.x > 0.0 || S.tint.y > 0.0) {
        let tg = S.step.y;
        let qg = (p - S.emitter.xyz) / max(S.emitter.w, 0.3) * 0.7 + vec3<f32>(S.step.z, -tg * 0.8, tg * 0.23);
        greyNow = clamp(S.tint.x + S.tint.y * (vnoise(qg) - 0.5) * 2.0, 0.0, 1.0);
    }
    let made = (sourceProducts.x + products.x) * exp(-losses.x * dt * 0.5);
    let greyW = transported.w * exp(-losses.x * dt) + made * greyNow;
    if (solidCell(id)) {
        // nothing lives inside a wall, a roof or the ground
        textureStore(outRgba, id, vec4<f32>(0.0));
        textureStore(outR, id, vec4<f32>(0.0));
        return;
    }
    textureStore(outRgba, id, vec4<f32>(max(dh, vec2<f32>(0.0)), after, clamp(greyW, 0.0, max(dh.x, 0.0))));
    textureStore(outR, id, vec4<f32>(products.z, 0.0, 0.0, 0.0));
}

@compute @workgroup_size(4, 4, 4)
fn cs_curl(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let h = 0.5 / S.origin_cell.w;
    let vxp = velCell(id + vec3<i32>(1, 0, 0)); let vxm = velCell(id - vec3<i32>(1, 0, 0));
    let vyp = velCell(id + vec3<i32>(0, 1, 0)); let vym = velCell(id - vec3<i32>(0, 1, 0));
    let vzp = velCell(id + vec3<i32>(0, 0, 1)); let vzm = velCell(id - vec3<i32>(0, 0, 1));
    let c = vec3<f32>((vyp.z - vym.z) - (vzp.y - vzm.y),
                      (vzp.x - vzm.x) - (vxp.z - vxm.z),
                      (vxp.y - vxm.y) - (vyp.x - vym.x)) * h;
    textureStore(outRgba, id, vec4<f32>(c, length(c)));
}

// curl-noise.wgsl: the turbulence field, evaluated directly instead of baked into a texture
fn nhash(p: vec3<f32>) -> f32 {
    var v = (bitcast<vec3<u32>>(vec3<i32>(p)) & vec3<u32>(15u)) * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> vec3<u32>(16u);
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return f32(v.x >> 8u) / 16777216.0;
}
// smooth value noise in 0..1 (from the same hash as the curl noise)
fn vnoise(p: vec3<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let s = f * f * (3.0 - 2.0 * f);
    let a = mix(mix(nhash(i), nhash(i + vec3<f32>(1.0, 0.0, 0.0)), s.x),
                mix(nhash(i + vec3<f32>(0.0, 1.0, 0.0)), nhash(i + vec3<f32>(1.0, 1.0, 0.0)), s.x), s.y);
    let b = mix(mix(nhash(i + vec3<f32>(0.0, 0.0, 1.0)), nhash(i + vec3<f32>(1.0, 0.0, 1.0)), s.x),
                mix(nhash(i + vec3<f32>(0.0, 1.0, 1.0)), nhash(i + vec3<f32>(1.0, 1.0, 1.0)), s.x), s.y);
    return mix(a, b, s.z);
}

fn noiseGradient(p: vec3<f32>) -> vec3<f32> {
    let i = floor(p);
    let f = fract(p);
    let s = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    let ds = 30.0 * f * f * (f - 1.0) * (f - 1.0);
    let a = nhash(i);
    let b = nhash(i + vec3<f32>(1.0, 0.0, 0.0));
    let c = nhash(i + vec3<f32>(0.0, 1.0, 0.0));
    let d = nhash(i + vec3<f32>(1.0, 1.0, 0.0));
    let e = nhash(i + vec3<f32>(0.0, 0.0, 1.0));
    let f1 = nhash(i + vec3<f32>(1.0, 0.0, 1.0));
    let g = nhash(i + vec3<f32>(0.0, 1.0, 1.0));
    let h = nhash(i + vec3<f32>(1.0, 1.0, 1.0));
    return vec3<f32>(
        mix(mix(b - a, d - c, s.y), mix(f1 - e, h - g, s.y), s.z),
        mix(mix(c - a, d - b, s.x), mix(g - e, h - f1, s.x), s.z),
        mix(mix(e - a, f1 - b, s.x), mix(g - c, h - d, s.x), s.y)) * ds;
}
fn curlNoise(q: vec3<f32>) -> vec3<f32> {
    let p = q - floor(q / 16.0) * 16.0;
    let dx = noiseGradient(p);
    let dy = noiseGradient(p + vec3<f32>(7.1, 3.7, 11.3));
    let dz = noiseGradient(p + vec3<f32>(5.2, 7.9, 9.5));
    return vec3<f32>(dz.y - dy.z, dx.z - dz.x, dy.x - dx.y);
}

// fluid-forces.wgsl centerForce + applyForces, at cell centres
@compute @workgroup_size(4, 4, 4)
fn cs_forces(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let dt = S.step.x;
    let h = S.origin_cell.w;
    let p = centre(id);
    let f = textureLoad(fieldsIn, id, 0);
    var force = vec3<f32>(0.0, S.motion.x * f.y - S.motion.y * f.x, 0.0);
    if (S.motion.w != 0.0) {
        let c = textureLoad(auxIn, id, 0).xyz;
        let eta = vec3<f32>(curlMag(id + vec3<i32>(1, 0, 0)) - curlMag(id - vec3<i32>(1, 0, 0)),
                            curlMag(id + vec3<i32>(0, 1, 0)) - curlMag(id - vec3<i32>(0, 1, 0)),
                            curlMag(id + vec3<i32>(0, 0, 1)) - curlMag(id - vec3<i32>(0, 0, 1)));
        force += cross(eta / (length(eta) + 0.0001), c) * S.motion.w * h;
    }
    if (S.losses.z != 0.0) {
        // force-fields.wgsl turbulence: two octaves, drifting with time
        let t = S.step.y;
        let q = p * S.losses.w * 0.65 + vec3<f32>(S.step.z + t * 0.6, -t * 0.42, S.step.z * 0.25 + t * 0.15);
        force += (curlNoise(q) + 0.45 * curlNoise(q * 2.03 + vec3<f32>(13.7, 5.3, 19.1))) * S.losses.z;
    }
    var v = textureLoad(velIn, id, 0).xyz * exp(-S.motion.z * dt) + dt * force;
    // the wind carries the gas (OP: a relaxation toward the wind, Fire Pro: a constant push), with
    // the ground's boundary layer: weak at the bed, full a few bed radii up, so a flame leans from a
    // base that stays on its fuel instead of being blown off it
    let bedBase = S.emitter.y - 0.45 * S.emitter.w;
    let layer = clamp(sqrt(max(p.y - bedBase, 0.0) / (3.0 * S.emitter.w + 1.0)), 0.2, 1.0);
    let windV = vec3<f32>(S.wind.x * S.extra.y * layer, v.y, S.wind.z * S.extra.y * layer);
    v = mix(v, windV, 1.0 - exp(-S.wind.w * layer * dt));
    // FIRE-002: a bursting charge blows outward (Fire Pro's burst outwardSpeed), while it bursts
    if (S.extra.w > 0.0) {
        let dq = (p - S.emitter.xyz) / max(S.emitter.w, 0.03);
        let rq = length(dq);
        if (rq < 1.6) {
            let w = 1.0 - smoothstep(0.6, 1.6, rq);
            var outward = (p - S.emitter.xyz) / max(length(p - S.emitter.xyz), 1e-3);
            var speed = S.extra.w;
            if (S.tint.w > 0.5) {
                // BOOM-001: a ground burst throws its earth UP and out in a crown of jets: steep
                // in the middle, flatter towards the rim, a skirt along the ground at its foot;
                // the speed varies round the crown so it breaks into separate spikes of soil
                let a = atan2(dq.z, dq.x);
                let seed = S.step.z;
                let jets = abs(sin(a * 3.5 + seed) * sin(a * 2.3 + seed * 1.7 + 1.3));
                let up = 1.6 + 1.2 * jets - 0.9 * smoothstep(0.4, 1.4, length(dq.xz));
                outward = normalize(vec3<f32>(dq.x, max(dq.y, 0.0) + up, dq.z));
                speed = S.extra.w * (0.3 + 1.1 * pow(jets, 1.2));
                // the central column goes straight up, fastest of all
                let core = 1.0 - smoothstep(0.2, 0.4, length(dq.xz));
                outward = normalize(mix(outward, vec3<f32>(0.0, 1.0, 0.0), core));
                speed = mix(speed, S.extra.w * 1.3, core);
                // the skirt: what starts at ground level runs out along it
                if (dq.y < 0.15) {
                    outward = normalize(vec3<f32>(dq.x, 0.25, dq.z));
                    speed = S.extra.w * 0.45;
                }
            }
            // a charge's kick is near-instant for thrown earth (12/s eased the jets in over ~10
            // steps, and the burst was over before they reached speed)
            let kick = select(12.0, 90.0, S.tint.w > 0.5);
            v = mix(v, outward * speed, 1.0 - exp(-kick * w * dt));
        }
    }
    // the source's own velocity (sourceVelocity): a rise out of the bed
    // BOOM-001: a burst (extra.w > 0) blows from a near-round charge, not a flat bed: a flattened
    // emitter made the fireball a lens
    // (a ground burst's thrown earth, tint.w: a tall column of soil, ~3 radii high)
    let flat = select(0.45, select(0.9, 3.2, S.tint.w > 0.5), S.extra.w > 0.0);
    let d = (p - S.emitter.xyz) / max(vec3<f32>(S.emitter.w, S.emitter.w * flat, S.emitter.w), vec3<f32>(0.03));
    let r = length(d);
    if (r < 1.0 && S.emit.w > 0.0) {
        let w = 1.0 - smoothstep(0.5, 1.0, r);
        v.y = mix(v.y, S.emit.w, 1.0 - exp(-8.0 * w * dt * S.emit.x));
    }
    if (id.y == 0 && S.dims.w == 1u) { v.y = max(v.y, 0.0); }
    textureStore(outRgba, id, vec4<f32>(enforce(id, v), 0.0));
}

// computeDivergence: the right-hand side h^2 (div - expansion)
@compute @workgroup_size(4, 4, 4)
fn cs_divergence(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let h = S.origin_cell.w;
    var vym = velCell(id - vec3<i32>(0, 1, 0));
    if (id.y == 0 && S.dims.w == 1u) { vym = vec3<f32>(0.0); }
    let div = (velCell(id + vec3<i32>(1, 0, 0)).x - velCell(id - vec3<i32>(1, 0, 0)).x
             + velCell(id + vec3<i32>(0, 1, 0)).y - vym.y
             + velCell(id + vec3<i32>(0, 0, 1)).z - velCell(id - vec3<i32>(0, 0, 1)).z) / (2.0 * h);
    let expansion = textureLoad(dIn, id, 0).x;
    textureStore(outR, id, vec4<f32>(h * h * (div - expansion), 0.0, 0.0, 0.0));
}

@compute @workgroup_size(4, 4, 4)
fn cs_jacobi(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let here = textureLoad(pIn, id, 0).x;
    let sum = pCell(id + vec3<i32>(1, 0, 0), here) + pCell(id - vec3<i32>(1, 0, 0), here)
            + pCell(id + vec3<i32>(0, 1, 0), here) + pCell(id - vec3<i32>(0, 1, 0), here)
            + pCell(id + vec3<i32>(0, 0, 1), here) + pCell(id - vec3<i32>(0, 0, 1), here);
    var rhs = textureLoad(dIn, id, 0).x;
    if (solidCell(id)) { rhs = 0.0; }
    // weighted Jacobi (Fire Pro's smoother weight 2/3 was for the multigrid; plain here
    // converges faster per iteration on a warm start)
    textureStore(outR, id, vec4<f32>((sum - rhs) / 6.0, 0.0, 0.0, 0.0));
}

@compute @workgroup_size(4, 4, 4)
fn cs_project(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = vec3<i32>(gid);
    if (!inside(id)) { return; }
    let h = S.origin_cell.w;
    let here = textureLoad(pIn, id, 0).x;
    let grad = vec3<f32>(pCell(id + vec3<i32>(1, 0, 0), here) - pCell(id - vec3<i32>(1, 0, 0), here),
                         pCell(id + vec3<i32>(0, 1, 0), here) - pCell(id - vec3<i32>(0, 1, 0), here),
                         pCell(id + vec3<i32>(0, 0, 1), here) - pCell(id - vec3<i32>(0, 0, 1), here)) / (2.0 * h);
    // p solves lap(p) = div in world units (cs_jacobi with the h^2 right-hand side)
    var v = textureLoad(velIn, id, 0).xyz - grad;
    if (id.y == 0 && S.dims.w == 1u) { v.y = max(v.y, 0.0); }
    // keep the gas inside the box finite: a runaway cell would smear the whole field
    // (BOOM-001: thrown earth leaves at up to ~70 m/s -- at the gas's 25 m/s cap the dirt
    // burst never rose above ~8 m)
    let speed = length(v);
    let cap = select(25.0, 90.0, S.tint.w > 0.5);
    if (speed > cap) { v = v * (cap / speed); }
    textureStore(outRgba, id, vec4<f32>(enforce(id, v), 0.0));
}
