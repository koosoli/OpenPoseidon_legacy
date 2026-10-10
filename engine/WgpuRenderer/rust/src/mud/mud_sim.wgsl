// MUD-001: the mud thrown off wheels and boots, as a viscous particle fluid. A clean-room
// position-based fluid after Macklin & Mueller, "Position Based Fluids" (2013): the one-sided
// density constraint C = rho / rho0 - 1 (only compression is corrected, so a free surface never
// pulls itself together) with an XPBD compliance term, solved by Jacobi; then an implicit
// viscosity, (I - dt nu L) v = v0, by Jacobi sweeps with poly6 weights -- the approach and the
// loop structure (substeps, two constraint iterations, implicit viscosity in place of explicit
// XSPH, which diverges at honey-like viscosities) follow dgreenheck/threejs-particle-fluids (MIT,
// (c) Daniel Greenheck; see CREDITS.md). OP: the colliders are the terrain heightmap, and mud
// sticks where it lands (high friction and a settle), then fades.
//
// Buffers: pos (xyz, age; age < 0 = free slot), vel (xyz, drawn radius), pred (xyz, on the
// ground), lam (lambda, density), a 65536-bucket hash of linked lists (heads / next), and vel2,
// the scratch for the position deltas and one viscosity iterate (pred.xyz is the other).

struct Params {
    dt: f32,            // substep (s)
    h: f32,             // smoothing radius (m)
    inv_rho0: f32,      // 1 / rest density
    compliance: f32,    // XPBD alpha tilde = compliance / dt^2 is added to the denominator
    visc_rate: f32,     // nu * dt * 10 / h^2 (the Laplacian's weight)
    radius: f32,        // particle radius (m): the contact distance with the ground
    life: f32,          // seconds a particle lives
    count: u32,         // capacity
    gravity: f32,
    time: f32,
    emit_total: u32,    // particles to emit this frame (over all emitters)
    emitters: u32,
    hm_origin: vec2<f32>,
    hm_grid: f32,
    hm_on: f32,
    hm_size: vec2<u32>,
    volume: f32,        // particle volume (the viscosity weight's V_j)
    _pad: f32,
};

struct Emitter {
    pos_start: vec4<f32>,   // xyz, w: first of this emitter's particles in the frame's emission (as f32)
    vel_spread: vec4<f32>,  // xyz: launch velocity, w: velocity spread (m/s)
    size_count: vec4<f32>,  // x: drawn radius, y: positional spread (m), z: count (as f32), w: ring slot base (as f32)
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read_write> pos: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> vel: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> pred: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> lam: array<vec4<f32>>;
@group(0) @binding(5) var<storage, read_write> heads: array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> next: array<u32>;
@group(0) @binding(7) var<storage, read_write> vel2: array<vec4<f32>>;
@group(0) @binding(8) var<storage, read> emitters: array<Emitter>;
@group(0) @binding(9) var heightmap: texture_2d<f32>;

const NONE: u32 = 0xffffffffu;
const HASH: u32 = 65536u;
const PI: f32 = 3.14159265;

fn alive(i: u32) -> bool { return pos[i].w >= 0.0; }

fn hash3(c: vec3<i32>) -> u32 {
    let h = (u32(c.x) * 73856093u) ^ (u32(c.y) * 19349663u) ^ (u32(c.z) * 83492791u);
    return h & (HASH - 1u);
}
fn cellOf(p: vec3<f32>) -> vec3<i32> { return vec3<i32>(floor(p / P.h)); }

fn poly6(r2: f32) -> f32 {
    let h2 = P.h * P.h;
    if (r2 >= h2) { return 0.0; }
    let d = h2 - r2;
    return 315.0 / (64.0 * PI * pow(P.h, 9.0)) * d * d * d;
}
fn spikyGrad(r: vec3<f32>) -> vec3<f32> {
    let l = length(r);
    if (l >= P.h || l < 1e-6) { return vec3<f32>(0.0); }
    let d = P.h - l;
    return (-45.0 / (PI * pow(P.h, 6.0))) * d * d * (r / l);
}

// the ground under xz (the terrain heightmap, the grass's triangle split); -1e9 without one
fn hmLoad(ix: i32, iz: i32) -> f32 {
    let x = clamp(ix, 0, i32(P.hm_size.x) - 1);
    let z = clamp(iz, 0, i32(P.hm_size.y) - 1);
    return textureLoad(heightmap, vec2<i32>(x, z), 0).x;
}
fn ground(xz: vec2<f32>) -> f32 {
    if (P.hm_on < 0.5) { return -1e9; }
    let coord = (xz - P.hm_origin) / P.hm_grid;
    let cell = floor(coord);
    let f = coord - cell;
    let ix = i32(cell.x);
    let iz = i32(cell.y);
    let y00 = hmLoad(ix, iz);
    let y01 = hmLoad(ix + 1, iz);
    let y10 = hmLoad(ix, iz + 1);
    let y11 = hmLoad(ix + 1, iz + 1);
    if (f.x <= 1.0 - f.y) { return y00 + (y10 - y00) * f.y + (y01 - y00) * f.x; }
    return y10 + (y01 - y11) - (y10 - y11) * f.x - (y01 - y11) * f.y;
}

fn rnd(seed: u32) -> f32 {
    var x = seed * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    x = (x >> 22u) ^ x;
    return f32(x) / 4294967296.0;
}

// one thread per particle emitted this frame: find its emitter, take its ring slot
@compute @workgroup_size(64)
fn cs_emit(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= P.emit_total) { return; }
    var e = 0u;
    for (var i = 0u; i < P.emitters; i++) {
        if (f32(k) >= emitters[i].pos_start.w) { e = i; }
    }
    let em = emitters[e];
    let local = k - u32(em.pos_start.w);
    if (f32(local) >= em.size_count.z) { return; }
    let slot = (u32(em.size_count.w) + local) % P.count;
    let s = bitcast<u32>(P.time) ^ (k * 9781u);
    let jitter = vec3<f32>(rnd(s) - 0.5, rnd(s + 1u) * 0.5, rnd(s + 2u) - 0.5) * em.size_count.y;
    let dv = vec3<f32>(rnd(s + 3u) - 0.5, rnd(s + 4u) - 0.3, rnd(s + 5u) - 0.5) * 2.0 * em.vel_spread.w;
    pos[slot] = vec4<f32>(em.pos_start.xyz + jitter, 0.0);
    vel[slot] = vec4<f32>(em.vel_spread.xyz + dv, em.size_count.x * (0.7 + 0.6 * rnd(s + 6u)));
    pred[slot] = vec4<f32>(em.pos_start.xyz + jitter, 0.0);
    lam[slot] = vec4<f32>(0.0);
}

@compute @workgroup_size(64)
fn cs_predict(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    var v = vel[i].xyz;
    v.y -= P.gravity * P.dt;
    v *= 1.0 - min(0.4 * P.dt, 0.5); // air drag on a small wet blob
    vel[i] = vec4<f32>(v, vel[i].w);
    pred[i] = vec4<f32>(pos[i].xyz + v * P.dt, 0.0);
}

@compute @workgroup_size(64)
fn cs_clear(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x < HASH) { atomicStore(&heads[gid.x], NONE); }
}

@compute @workgroup_size(64)
fn cs_insert(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count) { return; }
    if (!alive(i)) { next[i] = NONE; return; }
    next[i] = atomicExchange(&heads[hash3(cellOf(pred[i].xyz))], i);
}

// density and lambda (one-sided: only a compressed particle is pushed apart)
@compute @workgroup_size(64)
fn cs_lambda(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    let pi = pred[i].xyz;
    let c0 = cellOf(pi);
    var rho = 0.0;
    var gradI = vec3<f32>(0.0);
    var gradSq = 0.0;
    for (var dz = -1; dz <= 1; dz++) {
        for (var dy = -1; dy <= 1; dy++) {
            for (var dx = -1; dx <= 1; dx++) {
                var j = atomicLoad(&heads[hash3(c0 + vec3<i32>(dx, dy, dz))]);
                var guard = 0u;
                loop {
                    if (j == NONE || guard >= 64u) { break; }
                    guard++;
                    let r = pi - pred[j].xyz;
                    let r2 = dot(r, r);
                    if (r2 < P.h * P.h) {
                        rho += poly6(r2);
                        if (j != i) {
                            let g = spikyGrad(r) * P.inv_rho0;
                            gradI += g;
                            gradSq += dot(g, g);
                        }
                    }
                    j = next[j];
                }
            }
        }
    }
    let c = rho * P.inv_rho0 - 1.0;
    var l = 0.0;
    if (c > 0.0) {
        l = -c / (dot(gradI, gradI) + gradSq + P.compliance / (P.dt * P.dt) + 1e-12);
    }
    lam[i] = vec4<f32>(l, rho, 0.0, 0.0);
}

// position deltas into vel2
@compute @workgroup_size(64)
fn cs_delta(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    let pi = pred[i].xyz;
    let li = lam[i].x;
    let c0 = cellOf(pi);
    var d = vec3<f32>(0.0);
    for (var dz = -1; dz <= 1; dz++) {
        for (var dy = -1; dy <= 1; dy++) {
            for (var dx = -1; dx <= 1; dx++) {
                var j = atomicLoad(&heads[hash3(c0 + vec3<i32>(dx, dy, dz))]);
                var guard = 0u;
                loop {
                    if (j == NONE || guard >= 64u) { break; }
                    guard++;
                    if (j != i) {
                        let r = pi - pred[j].xyz;
                        if (dot(r, r) < P.h * P.h) { d += (li + lam[j].x) * spikyGrad(r); }
                    }
                    j = next[j];
                }
            }
        }
    }
    vel2[i] = vec4<f32>(d * P.inv_rho0, 0.0);
}

// apply the deltas, then the ground: mud does not sink into it
@compute @workgroup_size(64)
fn cs_apply(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    let lim = P.radius;
    var p = pred[i].xyz + clamp(vel2[i].xyz, vec3<f32>(-lim), vec3<f32>(lim));
    let g = ground(p.xz) + P.radius * 0.6;
    var onGround = pred[i].w;
    if (p.y < g) { p.y = g; onGround = 1.0; }
    pred[i] = vec4<f32>(p, onGround);
}

// velocities from the move; on the ground mud sticks (it does not slide or bounce)
@compute @workgroup_size(64)
fn cs_velocity(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    var v = (pred[i].xyz - pos[i].xyz) / P.dt;
    if (pred[i].w > 0.5) {
        // landed mud sticks: a slow blob on the ground stops dead (the slope cannot keep feeding it
        // speed through the collision), a fast one keeps a little of its slide
        let slide = length(v.xz);
        let keep = select(0.12, 0.0, slide < 0.6);
        v = vec3<f32>(v.x * keep, max(v.y, 0.0) * 0.1, v.z * keep);
    }
    let speed = length(v);
    if (speed > 40.0) { v *= 40.0 / speed; }
    let age = pos[i].w + P.dt;
    if (age > P.life) {
        pos[i] = vec4<f32>(pred[i].xyz, -1.0);
        vel[i] = vec4<f32>(0.0);
        return;
    }
    pos[i] = vec4<f32>(pred[i].xyz, age);
    vel[i] = vec4<f32>(v, vel[i].w);
}

// the implicit viscosity, one Jacobi sweep: x' = (b + rate * sum w_j x_j) / (1 + rate * sum w_j),
// b = vel (this substep's velocities), the iterate read from `src` and written to `dst`
fn viscositySweep(i: u32, fromPred: bool) -> vec3<f32> {
    let pi = pos[i].xyz;
    let c0 = cellOf(pi);
    var sum = vec3<f32>(0.0);
    var wsum = 0.0;
    for (var dz = -1; dz <= 1; dz++) {
        for (var dy = -1; dy <= 1; dy++) {
            for (var dx = -1; dx <= 1; dx++) {
                var j = atomicLoad(&heads[hash3(c0 + vec3<i32>(dx, dy, dz))]);
                var guard = 0u;
                loop {
                    if (j == NONE || guard >= 64u) { break; }
                    guard++;
                    if (j != i) {
                        let r = pi - pos[j].xyz;
                        let w = poly6(dot(r, r)) * P.volume;
                        if (w > 0.0) {
                            var xj = vel2[j].xyz;
                            if (fromPred) { xj = pred[j].xyz; }
                            sum += w * xj;
                            wsum += w;
                        }
                    }
                    j = next[j];
                }
            }
        }
    }
    return (vel[i].xyz + P.visc_rate * sum) / (1.0 + P.visc_rate * wsum);
}

// the first sweep starts from b itself: vel -> vel2
@compute @workgroup_size(64)
fn cs_visc_init(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count) { return; }
    vel2[i] = vec4<f32>(select(vec3<f32>(0.0), vel[i].xyz, alive(i)), 0.0);
}
// vel2 -> pred.xyz (pred's xyz is free once the positions are committed)
@compute @workgroup_size(64)
fn cs_visc_a(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    pred[i] = vec4<f32>(viscositySweep(i, false), pred[i].w);
}
// pred.xyz -> vel2
@compute @workgroup_size(64)
fn cs_visc_b(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    vel2[i] = vec4<f32>(viscositySweep(i, true), 0.0);
}
// the result back into vel
@compute @workgroup_size(64)
fn cs_visc_commit(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= P.count || !alive(i)) { return; }
    vel[i] = vec4<f32>(vel2[i].xyz, vel[i].w);
}
